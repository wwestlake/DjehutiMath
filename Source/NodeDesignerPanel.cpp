#include "NodeDesignerPanel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace
{
constexpr float nodeWidth = 174.0f;
constexpr float headerHeight = 25.0f;
constexpr float rowHeight = 22.0f;
constexpr float bottomPad = 9.0f;
constexpr float pinRadius = 5.0f;
constexpr float minZoom = 0.35f;
constexpr float maxZoom = 2.5f;
constexpr float rerouteSize = 24.0f;
// The reference wrote schemaVersion 2. This copy writes 3: the same document
// plus a connections section naming both pins of every wire, and each
// input reference's output pin. It reads 1 to 3.
constexpr int schematicFormatVersion = 3;

// Copies into `written` the top-level sections, and the keys of each node
// (matched by id), that `previous` has and this editor does not write, so
// saving never drops what the editor does not show. Keys in `clearable` are
// written only when they have a value; their absence means they were
// cleared here, so they are never copied back.
void carryForwardUnmanaged(juce::var& written, const juce::var& previous, const juce::StringArray& clearable)
{
    auto* out = written.getDynamicObject();
    auto* old = previous.getDynamicObject();
    if (out == nullptr || old == nullptr) return;
    for (const auto& prop : old->getProperties())
        if (prop.name.toString() != "kind" && !out->hasProperty(prop.name))
            out->setProperty(prop.name, prop.value.clone());
    std::map<juce::String, juce::DynamicObject*> oldNodes;
    if (auto* arr = old->getProperty("nodes").getArray())
        for (const auto& n : *arr)
            if (auto* o = n.getDynamicObject())
                oldNodes[o->getProperty("id").toString()] = o;
    if (auto* arr = out->getProperty("nodes").getArray())
        for (auto& n : *arr)
            if (auto* o = n.getDynamicObject())
                if (auto it = oldNodes.find(o->getProperty("id").toString()); it != oldNodes.end())
                    for (const auto& prop : it->second->getProperties())
                        if (!clearable.contains(prop.name.toString()) && !o->hasProperty(prop.name))
                            o->setProperty(prop.name, prop.value.clone());
}

juce::String propertyText(const juce::var& object, const juce::Identifier& property, const juce::String& fallback = {})
{
    if (!object.isObject()) return fallback;
    const auto value = object.getProperty(property, {});
    return value.isVoid() ? fallback : value.toString();
}

int propertyInt(const juce::var& object, const juce::Identifier& property, int fallback = 0)
{
    if (!object.isObject()) return fallback;
    const auto value = object.getProperty(property, {});
    return value.isVoid() ? fallback : (int)value;
}

float propertyFloat(const juce::var& object, const juce::Identifier& property, float fallback = 0.0f)
{
    if (!object.isObject()) return fallback;
    const auto value = object.getProperty(property, {});
    return value.isVoid() ? fallback : (float)(double)value;
}

// Nodes whose one setting is text in the inspector: a String's text, a
// Number's value, and the name a component node refers to (pin, parameter
// or state variable).
bool usesTextField(const juce::String& type)
{
    return type == "literal_string" || type == "literal_f64"
        || (type.startsWith("pc_") && type != "pc_time" && type != "pc_timestep");
}

juce::String baseIdForType(const juce::String& type)
{
    if (type == "reroute") return "route";
    if (type == "state_machine_instance") return "machine";
    if (type == "sm_state") return "state";
    if (type == "sm_transition") return "transition";
    if (type == "sm_event") return "event";
    if (type == "param_i64") return "x";
    if (type == "literal_i64") return "number";
    if (type == "literal_string") return "text";
    if (type == "literal_f64") return "number";
    if (type == "pc_pin_voltage") return "v";
    if (type == "pc_pin_current") return "i";
    if (type == "pc_time") return "t";
    if (type == "pc_timestep") return "dt";
    if (type == "pc_parameter") return "param";
    if (type == "pc_state_get") return "state";
    if (type == "pc_state_set") return "set_state";
    if (type == "pc_drive") return "drive";
    if (type == "pc_drive_current") return "drive_i";
    if (type == "pc_set_resistance") return "pin_r";
    if (type == "add") return "add";
    if (type == "sub") return "sub";
    if (type == "mul") return "mul";
    if (type == "div") return "div";
    if (type == "mod") return "mod";
    if (type == "eq") return "equals";
    if (type == "lt") return "less";
    if (type == "gt") return "greater";
    if (type == "if") return "select";
    if (type.startsWith("call_function:")) return type.fromFirstOccurrenceOf(":", false, false);
    return type.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_").toLowerCase();
}

juce::Colour pinColour(const NodeDesignerPanel::Pin& pin)
{
    if (pin.flow == NodeDesignerPanel::PinFlow::Exec) return juce::Colours::white;
    if (pin.flow == NodeDesignerPanel::PinFlow::Stream) return juce::Colour(0xff9c8a5a);
    if (pin.flow == NodeDesignerPanel::PinFlow::Resource) return juce::Colour(0xff9c5a9c);
    if (pin.type == "bool") return juce::Colour(0xffd85a5a);
    if (pin.type == "string") return juce::Colour(0xffd85ad0);
    if (pin.type == "any") return juce::Colour(0xff9a9a9a);
    return juce::Colour(0xff5ad8a0);
}

juce::String pinListText(const std::vector<NodeDesignerPanel::Pin>& pins)
{
    juce::String text;
    for (const auto& pin : pins)
    {
        if (text.isNotEmpty()) text << ", ";
        text << pin.name << ":" << pin.type;
    }
    return text;
}

juce::String flowName(NodeDesignerPanel::PinFlow flow)
{
    switch (flow)
    {
        case NodeDesignerPanel::PinFlow::Data: return "data";
        case NodeDesignerPanel::PinFlow::Exec: return "exec";
        case NodeDesignerPanel::PinFlow::Stream: return "stream";
        case NodeDesignerPanel::PinFlow::Resource: return "resource";
    }
    return "data";
}

NodeDesignerPanel::PinFlow flowFromName(const juce::String& flow)
{
    if (flow == "exec") return NodeDesignerPanel::PinFlow::Exec;
    if (flow == "stream") return NodeDesignerPanel::PinFlow::Stream;
    if (flow == "resource") return NodeDesignerPanel::PinFlow::Resource;
    return NodeDesignerPanel::PinFlow::Data;
}
}

class ClearableSearchBox : public juce::Component
{
public:
    ClearableSearchBox()
    {
        editor.setMultiLine(false);
        editor.setReturnKeyStartsNewLine(false);
        editor.setScrollbarsShown(false);
        editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff20262f));
        editor.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff33424d));
        editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff5aa7c8));
        editor.onTextChange = [this] {
            clearButton.setVisible(editor.getText().isNotEmpty());
            if (onTextChange) onTextChange();
        };
        editor.onEscapeKey = [this] {
            if (editor.getText().isNotEmpty())
            {
                editor.clear();
                if (onTextChange) onTextChange();
            }
        };
        addAndMakeVisible(editor);

        clearButton.setButtonText(juce::String::charToString(0x00D7)); // multiplication sign '×'
        clearButton.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        clearButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff9aa8ba));
        clearButton.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        clearButton.setVisible(false);
        clearButton.onClick = [this] {
            editor.clear();
            editor.grabKeyboardFocus();
            if (onTextChange) onTextChange();
        };
        addAndMakeVisible(clearButton);
    }

    void setTextToShowWhenEmpty(const juce::String& text, juce::Colour colour)
    {
        editor.setTextToShowWhenEmpty(text, colour);
    }

    juce::String getText() const { return editor.getText(); }
    void setText(const juce::String& text) { editor.setText(text); }
    void clear() { editor.clear(); }

    void resized() override
    {
        auto area = getLocalBounds();
        clearButton.setBounds(area.removeFromRight(24).reduced(2));
        editor.setBounds(area);
    }

    std::function<void()> onTextChange;
    juce::TextEditor editor;
    juce::TextButton clearButton;
};

class NodeDesignerPanel::PalettePanel final : public juce::Component
{
public:
    explicit PalettePanel(NodeDesignerPanel& ownerIn)
        : owner(ownerIn), content(*this)
    {
        title.setFont(juce::Font(15.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        filter.setTextToShowWhenEmpty("Filter nodes...", juce::Colour(0xff6b7a8c));
        filter.onTextChange = [this] { content.rebuild(); };
        addAndMakeVisible(filter);

        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(10);
        addAndMakeVisible(viewport);

        content.rebuild();
    }

    void resized() override
    {
        auto b = getLocalBounds();
        title.setBounds(b.removeFromTop(25).reduced(8, 0));
        filter.setBounds(b.removeFromTop(30).reduced(6, 3));
        viewport.setBounds(b);
        content.layoutToWidth(b.getWidth() - viewport.getScrollBarThickness());
    }

    void refreshRows()
    {
        content.rebuild();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff15181d));
    }

private:
    struct Row
    {
        bool header = false;
        juce::String category;
        int templateIndex = -1;
        int count = 0;
        juce::Rectangle<int> bounds;
    };

    class ListContent final : public juce::Component
    {
    public:
        explicit ListContent(PalettePanel& paletteIn) : palette(paletteIn) {}

        void rebuild()
        {
            rows.clear();
            const auto text = palette.filter.getText().trim();
            std::vector<juce::String> categories;
            for (const auto& t : palette.owner.templates)
                if (std::find(categories.begin(), categories.end(), t.category) == categories.end())
                    categories.push_back(t.category);
            std::sort(categories.begin(), categories.end());

            for (const auto& category : categories)
            {
                std::vector<int> matches;
                for (int i = 0; i < (int)palette.owner.templates.size(); ++i)
                {
                    const auto& t = palette.owner.templates[(size_t)i];
                    if (t.category != category) continue;
                    if (text.isEmpty() || t.title.containsIgnoreCase(text) || t.type.containsIgnoreCase(text))
                        matches.push_back(i);
                }
                if (matches.empty()) continue;

                rows.push_back({ true, category, -1, (int)matches.size(), {} });
                const bool expanded = palette.expandedCategories.count(category) != 0 || text.isNotEmpty();
                if (expanded)
                    for (const auto i : matches)
                        rows.push_back({ false, category, i, 0, {} });
            }
            layoutToWidth(getWidth() > 0 ? getWidth() : 200);
        }

        void layoutToWidth(int width)
        {
            int y = 4;
            const int rowW = std::max(40, width - 8);
            for (auto& r : rows)
            {
                const auto h = r.header ? 25 : 31;
                r.bounds = juce::Rectangle<int>(4, y, rowW, h);
                y += h;
            }
            setSize(std::max(40, width), y + 6);
            repaint();
        }

        void paint(juce::Graphics& g) override
        {
            g.fillAll(juce::Colour(0xff181c22));
            for (int i = 0; i < (int)rows.size(); ++i)
                paintRow(g, i, rows[(size_t)i].bounds);
        }

        void paintRow(juce::Graphics& g, int row, juce::Rectangle<int> bounds)
        {
            if (row < 0 || row >= (int)rows.size()) return;
            const auto& r = rows[(size_t)row];
            if (r.header)
            {
                g.setColour(juce::Colour(0xff20262f));
                g.fillRect(bounds);
                const bool expanded = palette.expandedCategories.count(r.category) != 0 || palette.filter.getText().trim().isNotEmpty();
                g.setColour(juce::Colour(0xff9aa8ba));
                g.setFont(juce::Font(12.0f, juce::Font::bold));
                g.drawText((expanded ? "v " : "> ") + r.category + "  (" + juce::String(r.count) + ")",
                           bounds.reduced(7, 0), juce::Justification::centredLeft, true);
                return;
            }

            const auto& t = palette.owner.templates[(size_t)r.templateIndex];
            if (row == hoverRow)
            {
                g.setColour(juce::Colour(0xff24303d));
                g.fillRoundedRectangle(bounds.toFloat().reduced(1.0f), 4.0f);
            }
            g.setColour(t.colour);
            g.fillRoundedRectangle((float)bounds.getX() + 7.0f, (float)bounds.getCentreY() - 5.0f, 10.0f, 10.0f, 3.0f);
            g.setColour(juce::Colour(0xffc7d2df));
            g.setFont(juce::Font(13.0f));
            g.drawText(t.title, bounds.withTrimmedLeft(24).withTrimmedBottom(12), juce::Justification::centredLeft, true);
            g.setColour(juce::Colour(0xff758294));
            g.setFont(juce::Font(10.5f));
            g.drawText(t.type, bounds.withTrimmedLeft(24).withTrimmedTop(14), juce::Justification::centredLeft, true);
        }

        int rowAt(juce::Point<int> position) const
        {
            for (int i = 0; i < (int)rows.size(); ++i)
                if (rows[(size_t)i].bounds.contains(position))
                    return i;
            return -1;
        }

        void mouseMove(const juce::MouseEvent& e) override
        {
            const auto row = rowAt(e.getPosition());
            if (row != hoverRow)
            {
                hoverRow = row;
                repaint();
            }
        }

        void mouseExit(const juce::MouseEvent&) override
        {
            hoverRow = -1;
            repaint();
        }

        void mouseDown(const juce::MouseEvent& e) override
        {
            const auto row = rowAt(e.getPosition());
            if (row < 0 || row >= (int)rows.size()) return;
            const auto& r = rows[(size_t)row];
            if (r.header)
            {
                if (palette.expandedCategories.count(r.category) == 0) palette.expandedCategories.insert(r.category);
                else palette.expandedCategories.erase(r.category);
                rebuild();
                return;
            }
            if (r.templateIndex < 0) return;
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
                container->startDragging(palette.owner.templates[(size_t)r.templateIndex].type, this);
        }

        void mouseDoubleClick(const juce::MouseEvent& e) override
        {
            const auto row = rowAt(e.getPosition());
            if (row < 0 || row >= (int)rows.size()) return;
            const auto& r = rows[(size_t)row];
            if (r.header || r.templateIndex < 0) return;
            palette.owner.addNode(palette.owner.templates[(size_t)r.templateIndex].type, { 120.0f, 120.0f });
            palette.owner.repaint();
        }

        PalettePanel& palette;
        std::vector<Row> rows;
        int hoverRow = -1;
    };

    NodeDesignerPanel& owner;
    juce::Label title { {}, "Nodes" };
    ClearableSearchBox filter;
    juce::Viewport viewport;
    ListContent content;
    std::set<juce::String> expandedCategories { "Sources", "Math", "Logic", "Execution", "Functions", "Variables", "Types", "Streams", "Resources" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PalettePanel)
};

class NodeDesignerPanel::GraphCanvas final : public juce::Component,
                                            public juce::DragAndDropTarget
{
public:
    explicit GraphCanvas(NodeDesignerPanel& ownerIn) : owner(ownerIn)
    {
        setWantsKeyboardFocus(true);
    }

    juce::Point<float> worldToScreen(juce::Point<float> world) const
    {
        return { world.x * zoom + viewOffset.x, world.y * zoom + viewOffset.y };
    }

    juce::Point<float> screenToWorld(juce::Point<float> screen) const
    {
        return { (screen.x - viewOffset.x) / zoom, (screen.y - viewOffset.y) / zoom };
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff11151b));
        drawGrid(g);
        drawConnections(g);
        drawDraggedWire(g);
        drawNodes(g);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        const auto pos = event.position;
        PinHit pin;
        if (hitTestPin(pos, pin))
        {
            draggingWire = true;
            wireStart = pin;
            wireDragPos = pos;
            repaint();
            return;
        }

        const int node = hitTestNode(pos);
        if (node != 0)
        {
            owner.selectedNodeUid = node;
            owner.selectedConnectionIndex = -1;
            draggingNode = true;
            dragNodeUid = node;
            dragStartMouseWorld = screenToWorld(pos);
            if (auto* n = owner.findNode(node))
                dragStartNodeWorld = { n->x, n->y };
            owner.refreshProperties();
            repaint();
            return;
        }

        const int conn = hitTestConnection(pos);
        if (conn >= 0)
        {
            owner.selectedNodeUid = 0;
            owner.selectedConnectionIndex = conn;
            owner.refreshProperties();
            repaint();
            return;
        }

        owner.selectedNodeUid = 0;
        owner.selectedConnectionIndex = -1;
        owner.refreshProperties();
        panning = true;
        panStartMouse = pos;
        panStartOffset = viewOffset;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (draggingWire)
        {
            wireDragPos = event.position;
            repaint();
            return;
        }
        if (draggingNode)
        {
            if (auto* node = owner.findNode(dragNodeUid))
            {
                const auto delta = screenToWorld(event.position) - dragStartMouseWorld;
                node->x = dragStartNodeWorld.x + delta.x;
                node->y = dragStartNodeWorld.y + delta.y;
                owner.refreshProperties();
                repaint();
            }
            return;
        }
        if (panning)
        {
            viewOffset = panStartOffset + (event.position - panStartMouse);
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (event.mods.isPopupMenu()) {
            const int nodeUid = hitTestNode(event.position);
            juce::PopupMenu m;
            // Breakpoint and watch markers are saved with the schematic (its
            // debug section) and used by the Workbench's FRust debugger.
            if (nodeUid > 0) {
                if (auto* n = owner.findNode(nodeUid)) {
                    m.addItem(1, n->breakpoint ? "Clear Breakpoint" : "Set Breakpoint");
                    if (n->breakpoint)
                        m.addItem(4, n->breakpointEnabled ? "Disable Breakpoint" : "Enable Breakpoint");
                    m.addItem(2, n->watched ? "Clear Watch" : "Set Watch");
                }
            } else {
                m.addItem(3, "Compile & Run Schematic");
                m.addItem(5, "Start Debugging");
            }
            m.showMenuAsync(juce::PopupMenu::Options(), [this, nodeUid](int result) {
                if (result == 1 || result == 4) {
                    if (auto* n = owner.findNode(nodeUid)) {
                        if (result == 1) {
                            n->breakpoint = !n->breakpoint;
                            n->breakpointEnabled = true;
                        } else
                            n->breakpointEnabled = !n->breakpointEnabled;
                        owner.debugMarkersChanged();
                        owner.refreshProperties();
                        repaint();
                    }
                } else if (result == 2) {
                    if (auto* n = owner.findNode(nodeUid)) {
                        n->watched = !n->watched;
                        owner.debugMarkersChanged();
                        owner.refreshProperties();
                        repaint();
                    }
                } else if (result == 3) {
                    owner.compileAndRun();
                } else if (result == 5) {
                    if (owner.onDebugRequested != nullptr)
                        owner.onDebugRequested();
                }
            });
            return;
        }

        if (draggingWire)
        {
            draggingWire = false;
            PinHit target;
            if (hitTestPin(event.position, target) && target.nodeUid != 0)
            {
                PinHit output = wireStart.isInput ? target : wireStart;
                PinHit input = wireStart.isInput ? wireStart : target;
                if (!output.isInput && input.isInput && output.nodeUid != input.nodeUid)
                    owner.connectPins(output.nodeUid, output.pinIndex, input.nodeUid, input.pinIndex);
            }
            repaint();
            return;
        }
        draggingNode = false;
        panning = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        const int conn = hitTestConnection(event.position);
        if (conn >= 0 && conn < (int)owner.connections.size())
        {
            const auto connection = owner.connections[(size_t)conn];
            auto& reroute = owner.addRerouteNode(connection, screenToWorld(event.position) - juce::Point<float> { rerouteSize * 0.5f, rerouteSize * 0.5f });
            owner.connections.erase(owner.connections.begin() + conn);
            const auto rerouteUid = reroute.uid;
            owner.connections.push_back({ connection.fromNode, connection.fromPin, rerouteUid, 0,
                                          owner.connectionIdFor(connection.fromNode, connection.fromPin, rerouteUid, 0) });
            owner.connections.push_back({ rerouteUid, 0, connection.toNode, connection.toPin,
                                          owner.connectionIdFor(rerouteUid, 0, connection.toNode, connection.toPin) });
            owner.selectedNodeUid = rerouteUid;
            owner.selectedConnectionIndex = -1;
            owner.refreshProperties();
            repaint();
            return;
        }

        const int node = hitTestNode(event.position);
        if (auto* n = owner.findNode(node))
        {
            if (n->type == "reroute")
                return;
            n->breakpoint = !n->breakpoint;
            n->breakpointEnabled = true;
            owner.debugMarkersChanged();
            owner.refreshProperties();
            repaint();
        }
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        const auto before = screenToWorld(event.position);
        const float factor = wheel.deltaY > 0.0f ? 1.1f : (1.0f / 1.1f);
        zoom = std::clamp(zoom * factor, minZoom, maxZoom);
        viewOffset += event.position - worldToScreen(before);
        repaint();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (auto* focused = juce::Component::getCurrentlyFocusedComponent())
        {
            if (dynamic_cast<juce::TextEditor*>(focused) != nullptr
                || dynamic_cast<juce::Label*>(focused) != nullptr
                || focused->findParentComponentOfClass<juce::TextEditor>() != nullptr)
            {
                return false;
            }
        }

        if (key != juce::KeyPress::deleteKey && key != juce::KeyPress::backspaceKey)
            return false;
        if (owner.selectedConnectionIndex >= 0 && owner.selectedConnectionIndex < (int)owner.connections.size())
        {
            owner.connections.erase(owner.connections.begin() + owner.selectedConnectionIndex);
            owner.selectedConnectionIndex = -1;
            owner.refreshProperties();
            repaint();
            return true;
        }
        if (owner.selectedNodeUid != 0)
        {
            owner.removeNode(owner.selectedNodeUid);
            owner.selectedNodeUid = 0;
            owner.refreshProperties();
            repaint();
            return true;
        }
        return false;
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return details.description.isString();
    }

    void itemDropped(const SourceDetails& details) override
    {
        const auto type = details.description.toString();
        owner.addNode(type, screenToWorld(details.localPosition.toFloat()));
        repaint();
    }

private:
    struct PinHit
    {
        int nodeUid = 0;
        int pinIndex = -1;
        bool isInput = false;
    };

    float nodeHeight(const GraphNode& node) const
    {
        if (node.type == "reroute") return rerouteSize;
        return headerHeight + (float)std::max(node.inputs.size(), node.outputs.size()) * rowHeight + bottomPad;
    }

    juce::Rectangle<float> nodeBounds(const GraphNode& node) const
    {
        const auto topLeft = worldToScreen({ node.x, node.y });
        if (node.type == "reroute")
            return { topLeft.x, topLeft.y, rerouteSize * zoom, rerouteSize * zoom };
        return { topLeft.x, topLeft.y, nodeWidth * zoom, nodeHeight(node) * zoom };
    }

    juce::Point<float> pinPosition(const GraphNode& node, const Pin& pin, int index) const
    {
        if (node.type == "reroute")
        {
            const float x = pin.isInput ? 0.0f : rerouteSize;
            return worldToScreen({ node.x + x, node.y + rerouteSize * 0.5f });
        }
        const float x = pin.isInput ? 0.0f : nodeWidth;
        const float y = headerHeight + ((float)index + 0.5f) * rowHeight;
        return worldToScreen({ node.x + x, node.y + y });
    }

    int hitTestNode(juce::Point<float> pos) const
    {
        for (auto it = owner.nodes.rbegin(); it != owner.nodes.rend(); ++it)
            if (nodeBounds(*it).contains(pos))
                return it->uid;
        return 0;
    }

    bool hitTestPin(juce::Point<float> pos, PinHit& hit) const
    {
        const float radius = pinRadius * zoom + 6.0f;
        for (const auto& node : owner.nodes)
        {
            for (int i = 0; i < (int)node.inputs.size(); ++i)
            {
                if (pinPosition(node, node.inputs[(size_t)i], i).getDistanceFrom(pos) <= radius)
                {
                    hit = { node.uid, i, true };
                    return true;
                }
            }
            for (int i = 0; i < (int)node.outputs.size(); ++i)
            {
                if (pinPosition(node, node.outputs[(size_t)i], i).getDistanceFrom(pos) <= radius)
                {
                    hit = { node.uid, i, false };
                    return true;
                }
            }
        }
        return false;
    }

    bool connectionEndpoints(const Connection& c, juce::Point<float>& from, juce::Point<float>& to) const
    {
        const auto* fromNode = owner.findNode(c.fromNode);
        const auto* toNode = owner.findNode(c.toNode);
        if (fromNode == nullptr || toNode == nullptr) return false;
        if (c.fromPin < 0 || c.fromPin >= (int)fromNode->outputs.size()) return false;
        if (c.toPin < 0 || c.toPin >= (int)toNode->inputs.size()) return false;
        from = pinPosition(*fromNode, fromNode->outputs[(size_t)c.fromPin], c.fromPin);
        to = pinPosition(*toNode, toNode->inputs[(size_t)c.toPin], c.toPin);
        return true;
    }

    int hitTestConnection(juce::Point<float> pos) const
    {
        constexpr float hitDistance = 6.0f;
        constexpr int samples = 24;
        int best = -1;
        float bestDistance = hitDistance;
        for (int i = 0; i < (int)owner.connections.size(); ++i)
        {
            juce::Point<float> from, to;
            if (!connectionEndpoints(owner.connections[(size_t)i], from, to)) continue;
            const float dx = std::max(30.0f, std::abs(to.x - from.x) * 0.5f);
            const juce::Point<float> c1 { from.x + dx, from.y };
            const juce::Point<float> c2 { to.x - dx, to.y };
            for (int s = 0; s <= samples; ++s)
            {
                const float t = (float)s / (float)samples;
                const float u = 1.0f - t;
                const juce::Point<float> p {
                    u * u * u * from.x + 3.0f * u * u * t * c1.x + 3.0f * u * t * t * c2.x + t * t * t * to.x,
                    u * u * u * from.y + 3.0f * u * u * t * c1.y + 3.0f * u * t * t * c2.y + t * t * t * to.y
                };
                const auto d = p.getDistanceFrom(pos);
                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = i;
                }
            }
        }
        return best;
    }

    void drawGrid(juce::Graphics& g)
    {
        const float minor = 32.0f * zoom;
        const float major = minor * 4.0f;
        if (major > 4.0f)
        {
            g.setColour(juce::Colour(0xff26313f));
            for (float x = std::fmod(viewOffset.x, major); x < (float)getWidth(); x += major)
                g.drawVerticalLine((int)x, 0.0f, (float)getHeight());
            for (float y = std::fmod(viewOffset.y, major); y < (float)getHeight(); y += major)
                g.drawHorizontalLine((int)y, 0.0f, (float)getWidth());
        }
        if (minor > 16.0f)
        {
            g.setColour(juce::Colour(0xff1a212a));
            for (float x = std::fmod(viewOffset.x, minor); x < (float)getWidth(); x += minor)
                g.drawVerticalLine((int)x, 0.0f, (float)getHeight());
            for (float y = std::fmod(viewOffset.y, minor); y < (float)getHeight(); y += minor)
                g.drawHorizontalLine((int)y, 0.0f, (float)getWidth());
        }
    }

    const StateDef* findState(const juce::String& id) const
    {
        auto it = std::find_if(owner.states.begin(), owner.states.end(), [&id](const StateDef& s) { return s.id == id; });
        return it == owner.states.end() ? nullptr : &*it;
    }

    juce::Rectangle<float> stateBounds(const StateDef& state) const
    {
        const auto topLeft = worldToScreen({ state.x, state.y });
        return { topLeft.x, topLeft.y, 190.0f * zoom, 82.0f * zoom };
    }

    void drawStateMachine(juce::Graphics& g)
    {
        for (const auto& t : owner.transitions)
        {
            const auto* from = findState(t.from);
            const auto* to = findState(t.to);
            if (from == nullptr || to == nullptr) continue;

            const auto fromBounds = stateBounds(*from);
            const auto toBounds = stateBounds(*to);
            const auto start = fromBounds.getCentre();
            const auto end = toBounds.getCentre();
            drawStateTransition(g, start, end, t.event);
        }

        for (const auto& state : owner.states)
            drawState(g, state);

        if (owner.states.empty())
        {
            g.setColour(juce::Colour(0xff9aa8ba));
            g.setFont(juce::Font(15.0f));
            g.drawFittedText("State Machine diagram: add states, events, and transitions.",
                             getLocalBounds().reduced(24), juce::Justification::centred, 3);
        }
    }

    void drawStateTransition(juce::Graphics& g, juce::Point<float> start, juce::Point<float> end, const juce::String& label)
    {
        juce::Path p;
        p.startNewSubPath(start);
        const auto mid = (start + end) * 0.5f;
        const juce::Point<float> normal { -(end.y - start.y) * 0.12f, (end.x - start.x) * 0.12f };
        p.quadraticTo(mid + normal, end);
        g.setColour(juce::Colour(0xff6cb6ff));
        g.strokePath(p, juce::PathStrokeType(2.0f));

        const auto dir = (end - start);
        const auto len = std::max(1.0f, std::sqrt(dir.x * dir.x + dir.y * dir.y));
        const juce::Point<float> unit { dir.x / len, dir.y / len };
        const juce::Point<float> tip = end - unit * 96.0f * zoom;
        juce::Path arrow;
        arrow.startNewSubPath(tip);
        arrow.lineTo(tip - unit * 10.0f + juce::Point<float> { -unit.y, unit.x } * 6.0f);
        arrow.lineTo(tip - unit * 10.0f + juce::Point<float> { unit.y, -unit.x } * 6.0f);
        arrow.closeSubPath();
        g.fillPath(arrow);

        if (label.isNotEmpty())
        {
            g.setColour(juce::Colour(0xffdce9ee));
            g.setFont(juce::Font(11.5f));
            g.drawText(label, juce::Rectangle<float>(mid.x - 70.0f, mid.y - 13.0f, 140.0f, 22.0f),
                       juce::Justification::centred, true);
        }
    }

    void drawState(juce::Graphics& g, const StateDef& state)
    {
        auto bounds = stateBounds(state);
        g.setColour(state.initial ? juce::Colour(0xff21412f) : juce::Colour(0xff18232d));
        g.fillRoundedRectangle(bounds, 8.0f);
        g.setColour(state.initial ? juce::Colour(0xff67d391) : juce::Colour(0xff4b8fc8));
        g.drawRoundedRectangle(bounds, 8.0f, state.initial ? 2.5f : 1.5f);

        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(14.0f, juce::Font::bold));
        g.drawText(state.name, bounds.reduced(12.0f, 8.0f).removeFromTop(22.0f), juce::Justification::centredLeft, true);

        g.setColour(juce::Colour(0xff9aa8ba));
        g.setFont(juce::Font(11.0f));
        juce::String details;
        if (state.initial) details << "initial";
        if (state.terminal) details << (details.isNotEmpty() ? "  |  " : "") << "terminal";
        if (state.entryAction.isNotEmpty()) details << (details.isNotEmpty() ? "  |  " : "") << "entry: " << state.entryAction;
        if (details.isEmpty()) details = state.id;
        g.drawFittedText(details, bounds.reduced(12.0f, 34.0f).toNearestInt(), juce::Justification::topLeft, 2);
    }

    void drawWire(juce::Graphics& g, juce::Point<float> from, juce::Point<float> to, juce::Colour colour, float thickness = 2.0f)
    {
        juce::Path path;
        path.startNewSubPath(from);
        const float dx = std::max(30.0f, std::abs(to.x - from.x) * 0.5f);
        path.cubicTo(from.x + dx, from.y, to.x - dx, to.y, to.x, to.y);
        g.setColour(colour);
        g.strokePath(path, juce::PathStrokeType(thickness));
    }

    void drawPin(juce::Graphics& g, const Pin& pin, juce::Point<float> pos)
    {
        const auto size = pinRadius * 2.0f * zoom;
        const auto r = juce::Rectangle<float>(pos.x - pinRadius * zoom, pos.y - pinRadius * zoom, size, size);
        g.setColour(pinColour(pin));
        if (pin.flow == PinFlow::Exec)
            g.fillRect(r.expanded(1.0f));
        else if (pin.flow == PinFlow::Stream)
            g.fillRoundedRectangle(r.expanded(1.0f, 0.0f), 2.0f);
        else if (pin.flow == PinFlow::Resource)
        {
            juce::Path diamond;
            diamond.startNewSubPath(pos.x, r.getY() - 1.0f);
            diamond.lineTo(r.getRight() + 1.0f, pos.y);
            diamond.lineTo(pos.x, r.getBottom() + 1.0f);
            diamond.lineTo(r.getX() - 1.0f, pos.y);
            diamond.closeSubPath();
            g.fillPath(diamond);
        }
        else
            g.fillEllipse(r);
    }

    void drawConnections(juce::Graphics& g)
    {
        for (int i = 0; i < (int)owner.connections.size(); ++i)
        {
            const auto& c = owner.connections[(size_t)i];
            juce::Point<float> from, to;
            if (!connectionEndpoints(c, from, to)) continue;
            const auto* n = owner.findNode(c.fromNode);
            const auto colour = n != nullptr && c.fromPin < (int)n->outputs.size()
                ? pinColour(n->outputs[(size_t)c.fromPin])
                : juce::Colours::grey;
            drawWire(g, from, to, i == owner.selectedConnectionIndex ? juce::Colours::white : colour,
                     i == owner.selectedConnectionIndex ? 3.5f : 2.0f);
        }
    }

    void drawDraggedWire(juce::Graphics& g)
    {
        if (!draggingWire) return;
        const auto* node = owner.findNode(wireStart.nodeUid);
        if (node == nullptr) return;
        if (wireStart.isInput)
        {
            if (wireStart.pinIndex >= 0 && wireStart.pinIndex < (int)node->inputs.size())
                drawWire(g, wireDragPos, pinPosition(*node, node->inputs[(size_t)wireStart.pinIndex], wireStart.pinIndex), juce::Colours::white);
        }
        else
        {
            if (wireStart.pinIndex >= 0 && wireStart.pinIndex < (int)node->outputs.size())
                drawWire(g, pinPosition(*node, node->outputs[(size_t)wireStart.pinIndex], wireStart.pinIndex), wireDragPos, juce::Colours::white);
        }
    }

    void drawNodes(juce::Graphics& g)
    {
        for (const auto& node : owner.nodes)
        {
            const auto bounds = nodeBounds(node);
            const bool selected = node.uid == owner.selectedNodeUid;
            if (node.type == "reroute")
            {
                const auto centre = bounds.getCentre();
                const auto radius = juce::jmax(5.0f, 7.0f * zoom);
                const auto dot = juce::Rectangle<float>(centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);
                const auto colour = !node.outputs.empty() ? pinColour(node.outputs.front()) : juce::Colour(0xff7fffd4);
                g.setColour(juce::Colour(0xff11151b));
                g.fillEllipse(dot.expanded(3.0f * zoom));
                g.setColour(colour);
                g.fillEllipse(dot);
                g.setColour(selected ? juce::Colours::white : juce::Colour(0xff384354));
                g.drawEllipse(dot.expanded(selected ? 3.0f : 2.0f), selected ? 2.0f : 1.0f);
                continue;
            }
            g.setColour(juce::Colour(0xff1d2530));
            g.fillRoundedRectangle(bounds, 6.0f);

            auto header = bounds.withHeight(headerHeight * zoom);
            g.setColour(node.colour);
            g.fillRoundedRectangle(header, 6.0f);
            g.fillRect(header.withTop(header.getBottom() - 6.0f * zoom));

            g.setColour(juce::Colours::white);
            g.setFont(juce::Font(std::max(10.0f, 13.0f * zoom), juce::Font::bold));
            const auto displayTitle = ((node.type.startsWith("sm_") || node.type == "state_machine_instance") && node.textValue.isNotEmpty())
                ? node.textValue
                : node.title;
            g.drawText(displayTitle, header.reduced(7.0f * zoom, 0.0f), juce::Justification::centredLeft, true);

            g.setColour(selected ? juce::Colours::white : juce::Colour(0xff384354));
            g.drawRoundedRectangle(bounds, 6.0f, selected ? 2.0f : 1.0f);

            if (node.id == owner.executionNodeId)
            {
                // Execution is stopped at this node.
                g.setColour(juce::Colour(0xffffd60a));
                g.drawRoundedRectangle(bounds.expanded(4.0f), 8.0f, 3.0f);
                juce::Path arrow;
                const float ax = bounds.getX() - 22.0f, ay = bounds.getY() + headerHeight * zoom * 0.5f;
                arrow.addTriangle(ax, ay - 7.0f, ax, ay + 7.0f, ax + 12.0f, ay);
                g.fillPath(arrow);
            }

            if (node.breakpoint)
            {
                // A disabled breakpoint is drawn hollow.
                const auto dot = juce::Rectangle<float>(bounds.getX() - 7.0f, bounds.getY() - 7.0f, 14.0f, 14.0f);
                g.setColour(juce::Colour(0xffff3b30));
                if (node.breakpointEnabled)
                    g.fillEllipse(dot);
                else
                    g.drawEllipse(dot.reduced(1.0f), 2.0f);
                g.setColour(juce::Colour(0xfff8d7da));
                g.drawEllipse(dot, 1.0f);
            }

            if (node.watched)
            {
                const auto badge = juce::Rectangle<float>(bounds.getRight() - 24.0f, bounds.getY() - 5.0f, 24.0f, 15.0f);
                g.setColour(juce::Colour(0xff101820));
                g.fillRoundedRectangle(badge, 5.0f);
                g.setColour(juce::Colour(0xff76d7ff));
                g.drawRoundedRectangle(badge, 5.0f, 1.0f);
                g.setFont(juce::Font(10.0f, juce::Font::bold));
                g.drawText("W", badge, juce::Justification::centred);
            }

            g.setFont(juce::Font(std::max(9.0f, 11.0f * zoom)));
            if (node.type == "sm_state")
            {
                juce::String badges;
                if (node.initial) badges << "initial";
                if (node.terminal) badges << (badges.isNotEmpty() ? " | " : "") << "terminal";
                if (node.entryAction.isNotEmpty()) badges << (badges.isNotEmpty() ? " | " : "") << "entry";
                if (badges.isNotEmpty())
                {
                    g.setColour(juce::Colour(0xff9aa8ba));
                    g.drawText(badges, bounds.reduced(9.0f * zoom, 29.0f * zoom),
                               juce::Justification::topLeft, true);
                }
            }
            else if (node.type == "sm_transition")
            {
                g.setColour(juce::Colour(0xffdce9ee));
                g.drawText("on " + (node.eventName.isNotEmpty() ? node.eventName : juce::String("event")),
                           bounds.reduced(9.0f * zoom, 29.0f * zoom), juce::Justification::topLeft, true);
            }
            for (int i = 0; i < (int)node.inputs.size(); ++i)
            {
                const auto& pin = node.inputs[(size_t)i];
                const auto pos = pinPosition(node, pin, i);
                drawPin(g, pin, pos);
                g.setColour(juce::Colours::lightgrey);
                g.drawText(pin.name, juce::Rectangle<float>(pos.x + 8.0f * zoom, pos.y - rowHeight * zoom * 0.5f,
                                                            nodeWidth * 0.5f * zoom, rowHeight * zoom),
                           juce::Justification::centredLeft, true);
            }
            for (int i = 0; i < (int)node.outputs.size(); ++i)
            {
                const auto& pin = node.outputs[(size_t)i];
                const auto pos = pinPosition(node, pin, i);
                drawPin(g, pin, pos);
                g.setColour(juce::Colours::lightgrey);
                g.drawText(pin.name, juce::Rectangle<float>(pos.x - (nodeWidth * 0.5f + 8.0f) * zoom, pos.y - rowHeight * zoom * 0.5f,
                                                            nodeWidth * 0.5f * zoom, rowHeight * zoom),
                           juce::Justification::centredRight, true);
            }
        }
    }

    NodeDesignerPanel& owner;
    juce::Point<float> viewOffset { 70.0f, 70.0f };
    float zoom = 1.0f;
    bool panning = false;
    juce::Point<float> panStartMouse;
    juce::Point<float> panStartOffset;
    bool draggingNode = false;
    int dragNodeUid = 0;
    juce::Point<float> dragStartMouseWorld;
    juce::Point<float> dragStartNodeWorld;
    bool draggingWire = false;
    PinHit wireStart;
    juce::Point<float> wireDragPos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GraphCanvas)
};

class NodeDesignerPanel::InspectorPanel final : public juce::Component
{
public:
    explicit InspectorPanel(NodeDesignerPanel& ownerIn) : owner(ownerIn)
    {
        addAndMakeVisible(diagramName);
        addAndMakeVisible(integerValue);
        addAndMakeVisible(stringValue);
        addAndMakeVisible(nodeName);
        addAndMakeVisible(accessibility);
        addAndMakeVisible(entryAction);
        addAndMakeVisible(updateAction);
        addAndMakeVisible(exitAction);
        addAndMakeVisible(eventName);
        addAndMakeVisible(payloadType);
        addAndMakeVisible(guardExpression);
        addAndMakeVisible(transitionAction);
        addAndMakeVisible(machineRef);
        addAndMakeVisible(initialToggle);
        addAndMakeVisible(terminalToggle);

        configureEditor(diagramName);
        configureEditor(integerValue);
        configureEditor(stringValue);
        for (auto* editor : { &nodeName, &accessibility, &entryAction, &updateAction, &exitAction,
                              &eventName, &payloadType, &guardExpression, &transitionAction, &machineRef })
            configureEditor(*editor);

        initialToggle.setButtonText("Initial state");
        terminalToggle.setButtonText("Terminal state");
        initialToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffdce9ee));
        terminalToggle.setColour(juce::ToggleButton::textColourId, juce::Colour(0xffdce9ee));

        for (int i = 0; i < maxInputEditors; ++i)
        {
            auto* editor = new juce::TextEditor();
            configureEditor(*editor);
            editor->setVisible(false);
            editor->onTextChange = [this, i] {
                if (auto* node = owner.findNode(owner.selectedNodeUid))
                {
                    if (i >= 0 && i < (int)node->inputs.size() && owner.connectionToInput(node->uid, i) == nullptr)
                    {
                        while ((int)node->inputDefaults.size() <= i)
                            node->inputDefaults.push_back(defaultValueFor(node->inputs[(size_t)node->inputDefaults.size()]));
                        node->inputDefaults[(size_t)i] = inputEditors[i]->getText();
                    }
                }
            };
            addAndMakeVisible(editor);
            inputEditors.add(editor);
        }

        diagramName.onTextChange = [this] {
            owner.diagramName = diagramName.getText().trim().isEmpty() ? juce::String("Untitled Node Schematic") : diagramName.getText();
            owner.titleLabel.setText(owner.diagramName, juce::dontSendNotification);
        };
        integerValue.onTextChange = [this] {
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "literal_i64")
            {
                node->literalValue = integerValue.getText().getIntValue();
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
        stringValue.onTextChange = [this] {
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && usesTextField(node->type))
            {
                node->textValue = stringValue.getText();
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
        nodeName.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && (node->type.startsWith("sm_") || node->type == "state_machine_instance"))
            {
                auto name = nodeName.getText().trim();
                if (name.isEmpty()) name = node->title;
                node->textValue = name;
                node->id = name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_").toLowerCase();
                if (node->id.isEmpty()) node->id = uniqueFallbackId(*node);
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
        accessibility.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_state")
                node->accessibility = accessibility.getText().trim();
        };
        entryAction.onTextChange = [this] { updateSelectedStateText(entryAction, &GraphNode::entryAction); };
        updateAction.onTextChange = [this] { updateSelectedStateText(updateAction, &GraphNode::updateAction); };
        exitAction.onTextChange = [this] { updateSelectedStateText(exitAction, &GraphNode::exitAction); };
        eventName.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && (node->type == "sm_transition" || node->type == "sm_event"))
            {
                node->eventName = eventName.getText().trim();
                if (node->eventName.isEmpty()) node->eventName = node->id;
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
        payloadType.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_event")
                node->payloadType = payloadType.getText().trim();
        };
        guardExpression.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_transition")
                node->guardExpression = guardExpression.getText().trim();
        };
        transitionAction.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_transition")
                node->transitionAction = transitionAction.getText().trim();
        };
        machineRef.onTextChange = [this] {
            if (syncingEditors) return;
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "state_machine_instance")
                node->machineRef = machineRef.getText().trim();
        };
        initialToggle.onClick = [this] {
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_state")
            {
                node->initial = initialToggle.getToggleState();
                if (node->initial)
                    for (auto& other : owner.nodes)
                        if (other.uid != node->uid && other.type == "sm_state")
                            other.initial = false;
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
        terminalToggle.onClick = [this] {
            if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_state")
            {
                node->terminal = terminalToggle.getToggleState();
                if (owner.canvas != nullptr) owner.canvas->repaint();
            }
        };
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff151b22));
        auto area = getLocalBounds().reduced(8);

        drawLabel(g, "Diagram", diagramName.getBounds().translated(0, -18));

        if (const auto* node = owner.findNode(owner.selectedNodeUid))
            drawNode(g, area, *node);
        else if (owner.selectedConnectionIndex >= 0 && owner.selectedConnectionIndex < (int)owner.connections.size())
            drawWire(g, area, owner.connections[(size_t)owner.selectedConnectionIndex]);
        else
            drawHelp(g, area, "Select a node to edit its useful properties.");
    }

    void resized() override
    {
        syncEditorsToSelection();
        auto area = getLocalBounds().reduced(8);
        area.removeFromTop(18);
        diagramName.setBounds(area.removeFromTop(26));
        area.removeFromTop(18);

        integerValue.setVisible(false);
        stringValue.setVisible(false);
        hideStateMachineEditors();
        for (auto* editor : inputEditors)
            editor->setVisible(false);

        if (const auto* node = owner.findNode(owner.selectedNodeUid))
        {
            if (node->type.startsWith("sm_") || node->type == "state_machine_instance")
            {
                layoutStateMachineEditors(*node, area);
            }
            else if (node->type == "literal_i64")
            {
                area.removeFromTop(42);
                integerValue.setBounds(area.removeFromTop(26));
                integerValue.setVisible(true);
            }
            else if (usesTextField(node->type))
            {
                area.removeFromTop(42);
                stringValue.setBounds(area.removeFromTop(52));
                stringValue.setVisible(true);
            }

            layoutInputEditors(*node, area);
        }
    }

    void refreshFromModel()
    {
        syncEditorsToSelection();
        resized();
        repaint();
    }

private:
    void configureEditor(juce::TextEditor& editor)
    {
        editor.setMultiLine(false);
        editor.setReturnKeyStartsNewLine(false);
        editor.setScrollbarsShown(false);
        editor.setSelectAllWhenFocused(true);
        editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff202a33));
        editor.setColour(juce::TextEditor::textColourId, juce::Colour(0xfff0f6f8));
        editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff40525f));
        editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff7fffd4));
        editor.setFont(juce::Font(13.0f));
    }

    void syncEditorsToSelection()
    {
        if (diagramName.getText() != owner.diagramName)
            diagramName.setText(owner.diagramName, juce::dontSendNotification);

        if (const auto* node = owner.findNode(owner.selectedNodeUid))
        {
            if (node->type.startsWith("sm_") || node->type == "state_machine_instance")
            {
                syncingEditors = true;
                const auto displayName = node->textValue.isNotEmpty() ? node->textValue : node->id;
                if (nodeName.getText() != displayName)
                    nodeName.setText(displayName, juce::dontSendNotification);
                if (accessibility.getText() != node->accessibility)
                    accessibility.setText(node->accessibility, juce::dontSendNotification);
                if (entryAction.getText() != node->entryAction)
                    entryAction.setText(node->entryAction, juce::dontSendNotification);
                if (updateAction.getText() != node->updateAction)
                    updateAction.setText(node->updateAction, juce::dontSendNotification);
                if (exitAction.getText() != node->exitAction)
                    exitAction.setText(node->exitAction, juce::dontSendNotification);
                if (eventName.getText() != node->eventName)
                    eventName.setText(node->eventName, juce::dontSendNotification);
                if (payloadType.getText() != node->payloadType)
                    payloadType.setText(node->payloadType, juce::dontSendNotification);
                if (guardExpression.getText() != node->guardExpression)
                    guardExpression.setText(node->guardExpression, juce::dontSendNotification);
                if (transitionAction.getText() != node->transitionAction)
                    transitionAction.setText(node->transitionAction, juce::dontSendNotification);
                if (machineRef.getText() != node->machineRef)
                    machineRef.setText(node->machineRef, juce::dontSendNotification);
                initialToggle.setToggleState(node->initial, juce::dontSendNotification);
                terminalToggle.setToggleState(node->terminal, juce::dontSendNotification);
                syncingEditors = false;
            }

            if (node->type == "literal_i64" && integerValue.getText() != juce::String(node->literalValue))
                integerValue.setText(juce::String(node->literalValue), juce::dontSendNotification);
            if (usesTextField(node->type) && stringValue.getText() != node->textValue)
                stringValue.setText(node->textValue, juce::dontSendNotification);

            for (int i = 0; i < inputEditors.size(); ++i)
            {
                if (i >= (int)node->inputs.size())
                    continue;
                const auto value = i < (int)node->inputDefaults.size() && node->inputDefaults[(size_t)i].isNotEmpty()
                    ? node->inputDefaults[(size_t)i]
                    : defaultValueFor(node->inputs[(size_t)i]);
                if (inputEditors[i]->getText() != value)
                    inputEditors[i]->setText(value, juce::dontSendNotification);
            }
        }
    }

    void layoutInputEditors(const GraphNode& node, juce::Rectangle<int> area)
    {
        int y = area.getY();
        if (node.type == "event_start") y += 78;
        if (node.type == "print") y += 78;
        y += 26;

        for (int i = 0; i < (int)node.inputs.size() && i < inputEditors.size(); ++i)
        {
            const auto& pin = node.inputs[(size_t)i];
            if (pin.flow != PinFlow::Data || owner.connectionToInput(node.uid, i) != nullptr)
            {
                y += 30;
                continue;
            }

            auto field = juce::Rectangle<int>(area.getX() + 88, y, area.getWidth() - 88, 24);
            inputEditors[i]->setBounds(field);
            inputEditors[i]->setVisible(true);
            y += 30;
        }
    }

    void hideStateMachineEditors()
    {
        for (auto* editor : { &nodeName, &accessibility, &entryAction, &updateAction, &exitAction,
                              &eventName, &payloadType, &guardExpression, &transitionAction, &machineRef })
            editor->setVisible(false);
        initialToggle.setVisible(false);
        terminalToggle.setVisible(false);
    }

    void layoutStateMachineEditors(const GraphNode& node, juce::Rectangle<int> area)
    {
        area.removeFromTop(42);
        nodeName.setBounds(area.removeFromTop(26));
        nodeName.setVisible(true);
        area.removeFromTop(24);

        if (node.type == "sm_state")
        {
            accessibility.setBounds(area.removeFromTop(26));
            accessibility.setVisible(true);
            area.removeFromTop(8);
            initialToggle.setBounds(area.removeFromTop(24));
            terminalToggle.setBounds(area.removeFromTop(24));
            initialToggle.setVisible(true);
            terminalToggle.setVisible(true);
            area.removeFromTop(20);
            entryAction.setBounds(area.removeFromTop(28));
            updateAction.setBounds(area.removeFromTop(28));
            exitAction.setBounds(area.removeFromTop(28));
            entryAction.setVisible(true);
            updateAction.setVisible(true);
            exitAction.setVisible(true);
        }
        else if (node.type == "sm_transition")
        {
            eventName.setBounds(area.removeFromTop(26));
            eventName.setVisible(true);
            area.removeFromTop(24);
            guardExpression.setBounds(area.removeFromTop(28));
            transitionAction.setBounds(area.removeFromTop(28));
            guardExpression.setVisible(true);
            transitionAction.setVisible(true);
        }
        else if (node.type == "sm_event")
        {
            eventName.setBounds(area.removeFromTop(26));
            payloadType.setBounds(area.removeFromTop(26));
            eventName.setVisible(true);
            payloadType.setVisible(true);
        }
        else if (node.type == "state_machine_instance")
        {
            machineRef.setBounds(area.removeFromTop(26));
            machineRef.setVisible(true);
        }
    }

    void updateSelectedStateText(juce::TextEditor& source, juce::String GraphNode::*field)
    {
        if (syncingEditors) return;
        if (auto* node = owner.findNode(owner.selectedNodeUid); node != nullptr && node->type == "sm_state")
        {
            node->*field = source.getText().trim();
            if (owner.canvas != nullptr) owner.canvas->repaint();
        }
    }

    juce::String uniqueFallbackId(const GraphNode& node) const
    {
        return node.type == "sm_transition" ? "transition_" + juce::String(node.uid)
             : node.type == "sm_event" ? "event_" + juce::String(node.uid)
             : "state_" + juce::String(node.uid);
    }

    void drawLabel(juce::Graphics& g, const juce::String& text, juce::Rectangle<int> bounds)
    {
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xff9aa8ba));
        g.drawText(text, bounds, juce::Justification::centredLeft, true);
    }

    void drawNode(juce::Graphics& g, juce::Rectangle<int>& area, const GraphNode& node)
    {
        auto y = 70;
        g.setFont(juce::Font(14.0f, juce::Font::bold));
        g.setColour(node.colour);
        g.drawText(node.title, 8, y, getWidth() - 16, 22, juce::Justification::centredLeft, true);
        y += 30;

        if (node.type == "literal_i64")
        {
            drawLabel(g, "Value", integerValue.getBounds().translated(0, -18));
            y = integerValue.getBottom() + 14;
        }
        else if (usesTextField(node.type))
        {
            drawLabel(g, node.type == "literal_string" ? "Text" : node.type == "literal_f64" ? "Value (number)"
                         : node.type.contains("pin") || node.type.startsWith("pc_drive") || node.type == "pc_set_resistance" ? "Pin" : node.type == "pc_parameter" ? "Parameter" : "State variable",
                      stringValue.getBounds().translated(0, -18));
            y = stringValue.getBottom() + 14;
        }
        else if (node.type == "event_start")
        {
            drawHelp(g, { 8, y, getWidth() - 16, 70 }, "Execution begins here. Wire its Start output to action nodes like Print.");
            y += 78;
        }
        else if (node.type == "print")
        {
            drawHelp(g, { 8, y, getWidth() - 16, 70 }, "Print writes its Value input to the IDE console/output target when execution reaches it.");
            y += 78;
        }
        else if (node.type == "sm_state")
        {
            drawLabel(g, "Name", nodeName.getBounds().translated(0, -18));
            drawLabel(g, "Access", accessibility.getBounds().translated(0, -18));
            drawLabel(g, "Actions", entryAction.getBounds().translated(0, -18));
            drawLabel(g, "Entry", entryAction.getBounds().translated(-52, 0));
            drawLabel(g, "Update", updateAction.getBounds().translated(-52, 0));
            drawLabel(g, "Exit", exitAction.getBounds().translated(-52, 0));
            y = exitAction.getBottom() + 14;
        }
        else if (node.type == "sm_transition")
        {
            drawLabel(g, "Name", nodeName.getBounds().translated(0, -18));
            drawLabel(g, "Event", eventName.getBounds().translated(0, -18));
            drawLabel(g, "Guard", guardExpression.getBounds().translated(0, -18));
            drawLabel(g, "Action", transitionAction.getBounds().translated(0, -18));
            y = transitionAction.getBottom() + 14;
        }
        else if (node.type == "sm_event")
        {
            drawLabel(g, "Name", nodeName.getBounds().translated(0, -18));
            drawLabel(g, "Event", eventName.getBounds().translated(0, -18));
            drawLabel(g, "Payload", payloadType.getBounds().translated(0, -18));
            y = payloadType.getBottom() + 14;
        }
        else if (node.type == "state_machine_instance")
        {
            drawLabel(g, "Name", nodeName.getBounds().translated(0, -18));
            drawLabel(g, "Machine", machineRef.getBounds().translated(0, -18));
            drawHelp(g, { 8, machineRef.getBottom() + 14, getWidth() - 16, 78 },
                     "This node hosts a state-machine schematic. Exec/lib graphs drive it with tick, event, and payload inputs.");
            y = machineRef.getBottom() + 104;
        }

        drawInputDefaults(g, { 8, y, getWidth() - 16, getHeight() - y - 8 }, node);
    }

    void drawInputDefaults(juce::Graphics& g, juce::Rectangle<int> area, const GraphNode& node)
    {
        if (node.inputs.empty()) return;

        drawLabel(g, "Inputs", area.removeFromTop(18));
        g.setFont(juce::Font(12.0f));

        for (int i = 0; i < (int)node.inputs.size(); ++i)
        {
            const auto& pin = node.inputs[(size_t)i];
            auto row = area.removeFromTop(30);
            g.setColour(pinColour(pin));
            g.fillRoundedRectangle(row.removeFromLeft(10).withSizeKeepingCentre(8, 8).toFloat(), 4.0f);
            row.removeFromLeft(5);

            if (const auto* wire = owner.connectionToInput(node.uid, i))
            {
                const auto* source = owner.findNode(wire->fromNode);
                g.setColour(juce::Colour(0xff758294));
                g.drawText(pin.name + " : " + pin.type + "  wired from " + (source != nullptr ? source->title : juce::String("unknown")),
                           row, juce::Justification::centredLeft, true);
            }
            else
            {
                g.setColour(juce::Colour(0xffdce9ee));
                if (pin.flow == PinFlow::Data && i < inputEditors.size())
                    g.drawText(pin.name + " : " + pin.type, row.removeFromLeft(82), juce::Justification::centredLeft, true);
                else
                    g.drawText(pin.name + " : " + pin.type, row, juce::Justification::centredLeft, true);
            }
        }
    }

    juce::String defaultValueFor(const Pin& pin) const
    {
        if (pin.type == "bool") return "false";
        if (pin.type == "string") return "\"\"";
        if (pin.type == "exec") return "flow";
        if (pin.flow == PinFlow::Stream) return "stream";
        if (pin.flow == PinFlow::Resource) return "resource";
        return "0";
    }

    void drawWire(juce::Graphics& g, juce::Rectangle<int>& area, const Connection& c)
    {
        const auto* from = owner.findNode(c.fromNode);
        const auto* to = owner.findNode(c.toNode);
        drawHelp(g, area, "Wire: " + (from ? from->title : "?") + " -> " + (to ? to->title : "?"));
    }

    void drawHelp(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text)
    {
        g.setColour(juce::Colour(0xff1b232c));
        g.fillRoundedRectangle(area.toFloat(), 5.0f);
        g.setColour(juce::Colour(0xffdce9ee));
        g.setFont(juce::Font(12.0f));
        g.drawFittedText(text, area.reduced(8), juce::Justification::centredLeft, 4);
    }

    NodeDesignerPanel& owner;
    static constexpr int maxInputEditors = 12;
    juce::TextEditor diagramName;
    juce::TextEditor integerValue;
    juce::TextEditor stringValue;
    juce::TextEditor nodeName;
    juce::TextEditor accessibility;
    juce::TextEditor entryAction;
    juce::TextEditor updateAction;
    juce::TextEditor exitAction;
    juce::TextEditor eventName;
    juce::TextEditor payloadType;
    juce::TextEditor guardExpression;
    juce::TextEditor transitionAction;
    juce::TextEditor machineRef;
    juce::ToggleButton initialToggle;
    juce::ToggleButton terminalToggle;
    bool syncingEditors = false;
    juce::OwnedArray<juce::TextEditor> inputEditors;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanel)
};

// The definition of the programmable component whose program is open: its
// name, pins (name, role, reference), parameters, state variables and
// output resistance. Every change goes to the Workbench (onComponentEdited),
// which validates it, saves the definition and updates the schematic's
// instances; a refused change puts the panel back as it was.
class NodeDesignerPanel::ComponentPanel final : public juce::Component
{
public:
    explicit ComponentPanel(NodeDesignerPanel& ownerIn) : owner(ownerIn)
    {
        viewport.setViewedComponent(&content, false);
        viewport.setScrollBarsShown(true, false);
        viewport.setScrollBarThickness(8);
        addAndMakeVisible(viewport);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff15181d));
        g.setFont(juce::Font(15.0f, juce::Font::bold));
        g.setColour(juce::Colours::white);
        g.drawText("Component", getLocalBounds().reduced(6, 0).removeFromTop(24), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        viewport.setBounds(getLocalBounds().withTrimmedTop(26));
        layout();
    }

    // Rebuilds the rows from the owner's definition.
    void rebuild()
    {
        rows.clear();
        content.removeAllChildren();
        const auto& d = owner.editedDefinition;
        if (!d.isObject())
            return;

        auto label = [this](const juce::String& text) {
            auto l = std::make_unique<juce::Label>(juce::String(), text);
            l->setFont(juce::Font(12.0f, juce::Font::bold));
            l->setColour(juce::Label::textColourId, juce::Colour(0xff9aa8ba));
            return l;
        };
        auto editor = [this](const juce::String& text, std::function<void(const juce::String&)> commit) {
            auto e = std::make_unique<juce::TextEditor>();
            e->setText(text, false);
            e->setFont(juce::Font(13.0f));
            e->setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1b232c));
            e->setColour(juce::TextEditor::textColourId, juce::Colour(0xffdce9ee));
            auto* raw = e.get();
            auto done = [raw, text, commit] {
                if (raw->getText().trim() != text)
                    commit(raw->getText().trim());
            };
            e->onReturnKey = done;
            e->onFocusLost = done;
            return e;
        };
        auto button = [](const juce::String& text, std::function<void()> click) {
            auto b = std::make_unique<juce::TextButton>(text);
            b->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff263942));
            b->setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff0f6f8));
            b->onClick = std::move(click);
            return b;
        };

        // Name.
        {
            Row r;
            r.cells.push_back({ label("Name"), 54 });
            r.cells.push_back({ editor(d.getProperty("name", {}).toString(), [this](const juce::String& v) { change([v](juce::var& e) { e.getDynamicObject()->setProperty("name", v); }); }), 0 });
            add(std::move(r));
        }

        // Pins.
        {
            Row r;
            r.cells.push_back({ label("Pins"), 0 });
            r.cells.push_back({ button("+ Pin", [this] { addPin(); }), 60 });
            add(std::move(r));
        }
        const auto* pins = d.getProperty("pins", {}).getArray();
        const int pinCount = pins != nullptr ? pins->size() : 0;
        if (pinCount == 0)
        {
            Row r;
            r.cells.push_back({ label("No pins yet: + Pin adds one."), 0 });
            add(std::move(r));
        }
        for (int k = 0; k < pinCount; ++k)
        {
            const auto pin = (*pins)[k];
            Row r;
            r.cells.push_back({ editor(pin.getProperty("id", {}).toString(), [this, k](const juce::String& v) {
                change([k, v](juce::var& e) { e.getProperty("pins", {})[k].getDynamicObject()->setProperty("id", v); });
            }), 0 });
            auto role = std::make_unique<juce::ComboBox>();
            role->addItem("input", 1);
            role->addItem("voltage out", 2);
            role->addItem("current out", 3);
            const auto roleText = pin.getProperty("role", "input").toString();
            role->setSelectedId(roleText == "voltage_output" ? 2 : roleText == "current_output" ? 3 : 1, juce::dontSendNotification);
            role->setTooltip("input: no load until the program sets a resistance; voltage out: V behind the output resistance; current out: a current");
            auto* roleRaw = role.get();
            role->onChange = [this, k, roleRaw] {
                const auto id = roleRaw->getSelectedId();
                const juce::String value = id == 2 ? "voltage_output" : id == 3 ? "current_output" : "input";
                change([k, value](juce::var& e) {
                    auto* pin = e.getProperty("pins", {})[k].getDynamicObject();
                    pin->setProperty("role", value);
                    // A pin on its role's default side moves to the new role's
                    // (inputs left, outputs right); one placed elsewhere stays.
                    const auto side = pin->getProperty("side").toString();
                    const bool output = value != "input";
                    if (side.isEmpty() || side == (output ? "left" : "right"))
                    {
                        pin->setProperty("side", output ? "right" : "left");
                        pin->setProperty("order", -1);
                    }
                });
            };
            r.cells.push_back({ std::move(role), 92 });
            auto reference = std::make_unique<juce::ComboBox>();
            reference->addItem("gnd", 1);
            int selected = 1;
            for (int j = 0; j < pinCount; ++j)
                if (j != k)
                {
                    const auto other = (*pins)[j].getProperty("id", {}).toString();
                    reference->addItem(other, j + 2);
                    if (other == pin.getProperty("reference", {}).toString())
                        selected = j + 2;
                }
            reference->setSelectedId(selected, juce::dontSendNotification);
            reference->setTooltip("The pin's branch returns through ground, or through another pin (a floating branch between two terminals)");
            auto* refRaw = reference.get();
            reference->onChange = [this, k, refRaw] {
                const auto value = refRaw->getSelectedId() == 1 ? juce::String() : refRaw->getText();
                change([k, value](juce::var& e) {
                    auto* o = e.getProperty("pins", {})[k].getDynamicObject();
                    if (value.isEmpty()) o->removeProperty("reference"); else o->setProperty("reference", value);
                });
            };
            r.cells.push_back({ std::move(reference), 64 });
            r.cells.push_back({ button("x", [this, k] { change([k](juce::var& e) { e.getProperty("pins", {}).getArray()->remove(k); }); }), 24 });
            add(std::move(r));
        }

        // Parameters and state variables: name and a number.
        auto namedNumbers = [&](const char* title, const char* listKey, const char* valueKey, const char* base) {
            {
                Row r;
                r.cells.push_back({ label(title), 0 });
                r.cells.push_back({ button("+ Add", [this, listKey, valueKey, base] {
                    change([this, listKey, valueKey, base](juce::var& e) {
                        auto* o = new juce::DynamicObject();
                        o->setProperty("name", uniqueName(e.getProperty(listKey, {}), base));
                        o->setProperty(valueKey, 0.0);
                        if (!e.getProperty(listKey, {}).isArray()) e.getDynamicObject()->setProperty(listKey, juce::Array<juce::var>());
                        e.getProperty(listKey, {}).getArray()->add(juce::var(o));
                    });
                }), 60 });
                add(std::move(r));
            }
            const auto* list = d.getProperty(listKey, {}).getArray();
            for (int k = 0; list != nullptr && k < list->size(); ++k)
            {
                const auto item = (*list)[k];
                Row r;
                r.cells.push_back({ editor(item.getProperty("name", {}).toString(), [this, k, listKey](const juce::String& v) {
                    change([k, v, listKey](juce::var& e) { e.getProperty(listKey, {})[k].getDynamicObject()->setProperty("name", v); });
                }), 0 });
                r.cells.push_back({ editor(item.getProperty(valueKey, 0.0).toString(), [this, k, listKey, valueKey](const juce::String& v) {
                    change([k, v, listKey, valueKey](juce::var& e) { e.getProperty(listKey, {})[k].getDynamicObject()->setProperty(valueKey, v.getDoubleValue()); });
                }), 80 });
                r.cells.push_back({ button("x", [this, k, listKey] { change([k, listKey](juce::var& e) { e.getProperty(listKey, {}).getArray()->remove(k); }); }), 24 });
                add(std::move(r));
            }
        };
        namedNumbers("Parameters (name, default)", "parameters", "default", "param");
        namedNumbers("State (name, initial)", "state", "initial", "state");

        {
            Row r;
            r.cells.push_back({ label("Output R (ohm)"), 0 });
            r.cells.push_back({ editor(d.getProperty("outputResistance", 1.0).toString(), [this](const juce::String& v) {
                change([v](juce::var& e) { e.getDynamicObject()->setProperty("outputResistance", v.getDoubleValue()); });
            }), 80 });
            add(std::move(r));
        }
        layout();
    }

private:
    struct Cell
    {
        std::unique_ptr<juce::Component> component;
        int width = 0; // 0 = the rest of the row
    };
    struct Row
    {
        std::vector<Cell> cells;
    };

    void add(Row row)
    {
        for (auto& c : row.cells)
            content.addAndMakeVisible(*c.component);
        rows.push_back(std::move(row));
    }

    void layout()
    {
        const auto width = std::max(120, viewport.getWidth() - viewport.getScrollBarThickness() - 4);
        int y = 2;
        for (auto& row : rows)
        {
            auto area = juce::Rectangle<int>(4, y, width - 4, 24);
            int fixed = 0, flexible = 0;
            for (auto& c : row.cells) { fixed += c.width; if (c.width == 0) ++flexible; }
            const auto rest = std::max(30, (area.getWidth() - fixed - 4 * ((int)row.cells.size() - 1)) / std::max(1, flexible));
            for (auto& c : row.cells)
            {
                c.component->setBounds(area.removeFromLeft(c.width > 0 ? c.width : rest));
                area.removeFromLeft(4);
            }
            y += 28;
        }
        content.setSize(width, y + 4);
    }

    static juce::String uniqueName(const juce::var& list, const juce::String& base)
    {
        for (int n = 1;; ++n)
        {
            const auto candidate = base + juce::String(n);
            bool taken = false;
            if (const auto* items = list.getArray())
                for (const auto& item : *items)
                    taken |= item.getProperty("name", item.getProperty("id", {})).toString().equalsIgnoreCase(candidate);
            if (!taken)
                return candidate;
        }
    }

    void addPin()
    {
        change([](juce::var& e) {
            if (!e.getProperty("pins", {}).isArray())
                e.getDynamicObject()->setProperty("pins", juce::Array<juce::var>());
            auto* o = new juce::DynamicObject();
            o->setProperty("id", uniqueName(e.getProperty("pins", {}), "PIN"));
            o->setProperty("role", "input");
            e.getProperty("pins", {}).getArray()->add(juce::var(o));
        });
    }

    // Applies an edit to a copy of the definition and hands it to the owner;
    // the rows are rebuilt afterwards (asynchronously: the control whose
    // callback made the change is still on the stack).
    void change(const std::function<void(juce::var&)>& edit)
    {
        auto edited = juce::JSON::parse(juce::JSON::toString(owner.editedDefinition));
        edit(edited);
        juce::String message;
        owner.editComponent(edited, message);
        juce::Component::SafePointer<ComponentPanel> safe(this);
        juce::MessageManager::callAsync([safe] { if (safe != nullptr) safe->rebuild(); });
    }

    NodeDesignerPanel& owner;
    // Destroyed in reverse: the rows (children of content), the viewport
    // (which shows content), then content.
    juce::Component content;
    juce::Viewport viewport;
    std::vector<Row> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ComponentPanel)
};

class NodeDesignerPanel::VariablesPanel final : public juce::Component
{
public:
    explicit VariablesPanel(NodeDesignerPanel& ownerIn) : owner(ownerIn) {}

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff15181d));
        auto area = getLocalBounds().reduced(6);

        g.setFont(juce::Font(15.0f, juce::Font::bold));
        g.setColour(juce::Colours::white);
        g.drawText("Variables", area.removeFromTop(24), juce::Justification::centredLeft, true);

        auto header = area.removeFromTop(22);
        g.setColour(juce::Colour(0xff20262f));
        g.fillRect(header);
        g.setFont(juce::Font(11.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xff9aa8ba));
        g.drawText("Name", header.removeFromLeft(76).reduced(5, 0), juce::Justification::centredLeft, true);
        g.drawText("Type", header.removeFromLeft(48).reduced(5, 0), juce::Justification::centredLeft, true);
        g.drawText("Access", header.reduced(5, 0), juce::Justification::centredLeft, true);

        g.setFont(juce::Font(12.0f));
        int y = area.getY() + 4;
        for (const auto& v : owner.variables)
        {
            auto row = juce::Rectangle<int>(area.getX(), y, area.getWidth(), 24);
            g.setColour(juce::Colour(0xff1b232c));
            g.fillRoundedRectangle(row.toFloat(), 4.0f);
            auto cols = row.reduced(5, 0);
            g.setColour(juce::Colour(0xffdce9ee));
            g.drawText(v.name, cols.removeFromLeft(76), juce::Justification::centredLeft, true);
            g.setColour(juce::Colour(0xff7fffd4));
            g.drawText(v.type, cols.removeFromLeft(48), juce::Justification::centredLeft, true);
            g.setColour(v.access == "read_write" ? juce::Colour(0xfff5c15a)
                                                   : (v.access == "read_only" ? juce::Colour(0xff80bfff) : juce::Colour(0xff9aa8ba)));
            g.drawText(accessLabel(v.access), cols, juce::Justification::centredLeft, true);
            y += 28;
        }

        if (owner.variables.empty())
        {
            g.setColour(juce::Colour(0xff758294));
            g.drawText("No schematic variables yet.", area.reduced(4, 6), juce::Justification::centredLeft, true);
        }
    }

private:
    static juce::String accessLabel(const juce::String& access)
    {
        if (access == "read_only") return "read only";
        if (access == "read_write") return "read/write";
        return "local";
    }

    NodeDesignerPanel& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VariablesPanel)
};

class NodeDesignerPanel::TypesPanel final : public juce::Component
{
public:
    explicit TypesPanel(NodeDesignerPanel& ownerIn) : owner(ownerIn) {}

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff15181d));
        auto area = getLocalBounds().reduced(6);

        g.setFont(juce::Font(15.0f, juce::Font::bold));
        g.setColour(juce::Colours::white);
        g.drawText("Types", area.removeFromTop(24), juce::Justification::centredLeft, true);

        drawPrimitiveLegend(g, area.removeFromTop(48));
        area.removeFromTop(5);

        for (const auto& type : owner.types)
            drawType(g, area, type);

        if (owner.types.empty())
        {
            g.setColour(juce::Colour(0xff758294));
            g.setFont(juce::Font(12.0f));
            g.drawText("No user types yet.", area.reduced(4, 6), juce::Justification::centredLeft, true);
        }
    }

private:
    void drawPrimitiveLegend(juce::Graphics& g, juce::Rectangle<int> area)
    {
        g.setColour(juce::Colour(0xff1b232c));
        g.fillRoundedRectangle(area.toFloat(), 4.0f);

        g.setFont(juce::Font(11.0f));
        const juce::StringArray primitives { "i64", "f64", "bool", "string", "ascii" };
        auto x = area.getX() + 6;
        auto y = area.getY() + 7;
        for (const auto& primitive : primitives)
        {
            auto pill = juce::Rectangle<int>(x, y, 38, 16);
            g.setColour(typeColour(primitive));
            g.fillRoundedRectangle(pill.toFloat(), 8.0f);
            g.setColour(juce::Colours::white);
            g.drawText(primitive, pill, juce::Justification::centred, true);
            x += 42;
            if (x + 38 > area.getRight())
            {
                x = area.getX() + 6;
                y += 19;
            }
        }
    }

    void drawType(juce::Graphics& g, juce::Rectangle<int>& area, const TypeDef& type)
    {
        if (area.getHeight() < 42) return;

        int rows = juce::jmax((int)type.fields.size(), (int)type.variants.size());
        const int h = juce::jlimit(50, 116, 36 + rows * 17);
        auto box = area.removeFromTop(juce::jmin(h, area.getHeight()));
        area.removeFromTop(6);

        g.setColour(juce::Colour(0xff1b232c));
        g.fillRoundedRectangle(box.toFloat(), 5.0f);
        g.setColour(type.kind == "struct" ? juce::Colour(0xff4b8fc8) : juce::Colour(0xff9c5a9c));
        g.drawRoundedRectangle(box.toFloat().reduced(0.5f), 5.0f, 1.0f);

        auto inner = box.reduced(8, 5);
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.setColour(juce::Colour(0xffdce9ee));
        g.drawText(type.name, inner.removeFromTop(16), juce::Justification::centredLeft, true);

        g.setFont(juce::Font(10.5f));
        g.setColour(juce::Colour(0xff9aa8ba));
        g.drawText(type.kind + "  fallback: " + type.targetFallback, inner.removeFromTop(15), juce::Justification::centredLeft, true);

        g.setFont(juce::Font(11.0f));
        if (type.kind == "struct")
        {
            for (const auto& f : type.fields)
                drawMember(g, inner.removeFromTop(17), f.name, f.type);
        }
        else
        {
            for (const auto& v : type.variants)
            {
                juce::String shape = juce::String(v.intValue);
                if (!v.fields.empty())
                    shape << " / payload";
                drawMember(g, inner.removeFromTop(17), v.name, shape);
            }
        }
    }

    void drawMember(juce::Graphics& g, juce::Rectangle<int> row, const juce::String& name, const juce::String& type)
    {
        g.setColour(juce::Colour(0xffdce9ee));
        g.drawText(name, row.removeFromLeft(88), juce::Justification::centredLeft, true);
        g.setColour(typeColour(type));
        g.drawText(type, row, juce::Justification::centredLeft, true);
    }

    static juce::Colour typeColour(const juce::String& type)
    {
        if (type.startsWith("i") || type == "int") return juce::Colour(0xff80bfff);
        if (type.startsWith("f") || type == "float") return juce::Colour(0xff5ad8a0);
        if (type == "bool") return juce::Colour(0xffd85a5a);
        if (type == "string" || type == "ascii") return juce::Colour(0xffd85ad0);
        return juce::Colour(0xffc7d2df);
    }

    NodeDesignerPanel& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TypesPanel)
};

std::vector<NodeDesignerPanel::NodeTemplate> NodeDesignerPanel::buildTemplates()
{
    auto in = [](juce::String name, juce::String type = "i64") { return Pin { name, type, PinFlow::Data, true }; };
    auto out = [](juce::String name, juce::String type = "i64") { return Pin { name, type, PinFlow::Data, false }; };
    auto execIn = [](juce::String name = "in") { return Pin { name, "exec", PinFlow::Exec, true }; };
    auto execOut = [](juce::String name = "then") { return Pin { name, "exec", PinFlow::Exec, false }; };
    auto streamIn = [](juce::String name, juce::String type) { return Pin { name, type, PinFlow::Stream, true }; };
    auto streamOut = [](juce::String name, juce::String type) { return Pin { name, type, PinFlow::Stream, false }; };
    auto resourceIn = [](juce::String name, juce::String type) { return Pin { name, type, PinFlow::Resource, true }; };
    auto resourceOut = [](juce::String name, juce::String type) { return Pin { name, type, PinFlow::Resource, false }; };
    return {
        { "param_i64", "Parameter", "Sources", juce::Colour(0xff2f9c9c), 0, {}, { out("x") } },
        { "literal_i64", "Integer", "Sources", juce::Colour(0xff2f6fb8), 10, {}, { out("value") } },
        { "literal_string", "String", "Sources", juce::Colour(0xffd85ad0), 0, {}, { out("value", "string") } },
        { "const_bool", "Boolean", "Sources", juce::Colour(0xff2f6fb8), 0, {}, { out("value", "bool") } },
        { "literal_f64", "Number", "Sources", juce::Colour(0xff2f6fb8), 0, {}, { out("value", "any") } },
        { "add", "Add", "Math", juce::Colour(0xff5f9c5a), 0, { in("a"), in("b") }, { out("sum") } },
        { "sub", "Subtract", "Math", juce::Colour(0xff5f9c5a), 0, { in("a"), in("b") }, { out("difference") } },
        { "mul", "Multiply", "Math", juce::Colour(0xff5f9c5a), 0, { in("a"), in("b") }, { out("product") } },
        { "div", "Divide", "Math", juce::Colour(0xff5f9c5a), 0, { in("a"), in("b") }, { out("quotient") } },
        { "mod", "Modulo", "Math", juce::Colour(0xff5f9c5a), 0, { in("a"), in("b") }, { out("remainder") } },
        { "eq", "Equals", "Logic", juce::Colour(0xffb8722f), 0, { in("a"), in("b") }, { out("is equal", "bool") } },
        { "lt", "Less Than", "Logic", juce::Colour(0xffb8722f), 0, { in("a"), in("b") }, { out("is less", "bool") } },
        { "gt", "Greater Than", "Logic", juce::Colour(0xffb8722f), 0, { in("a"), in("b") }, { out("is greater", "bool") } },
        { "if", "If Select", "Logic", juce::Colour(0xff9c5a9c), 0, { in("cond", "bool"), in("then"), in("else") }, { out("value") } },
        { "event_start", "Event Start", "Execution", juce::Colour(0xffb8722f), 0, {}, { execOut("start") } },
        { "print", "Print", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("value", "any") }, { execOut("then") } },
        { "sequence", "Sequence", "Execution", juce::Colour(0xffb8722f), 0, { execIn() }, { execOut("A"), execOut("B") } },
        { "branch", "Branch", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("condition", "bool") }, { execOut("true"), execOut("false") } },
        { "while_loop", "While Loop", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("condition", "bool") }, { execOut("body"), execOut("done") } },
        { "for_loop", "For Loop", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("first"), in("last") }, { execOut("body"), execOut("done"), out("index") } },
        { "end", "End", "Execution", juce::Colour(0xffb8722f), 0, { execIn() }, {} },
        { "return", "Return", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("value") }, {} },
        { "get_var", "Get Variable", "Variables", juce::Colour(0xff2f9c9c), 0, {}, { out("value") } },
        { "set_var", "Set Variable", "Variables", juce::Colour(0xff2f9c9c), 0, { execIn(), in("value") }, { execOut("then") } },
        { "make_struct", "Make Struct", "Types", juce::Colour(0xff9c5a9c), 0, { in("field A", "any"), in("field B", "any") }, { out("struct", "any") } },
        { "break_struct", "Break Struct", "Types", juce::Colour(0xff9c5a9c), 0, { in("struct", "any") }, { out("field A", "any"), out("field B", "any") } },
        { "enum_value", "Enum Value", "Types", juce::Colour(0xff9c5a9c), 0, {}, { out("variant", "any") } },
        { "match_enum", "Match Enum", "Execution", juce::Colour(0xffb8722f), 0, { execIn(), in("variant", "any") }, { execOut("case A"), execOut("case B"), execOut("default") } },
        { "audio_buffer_in", "Audio Buffer In", "Streams", juce::Colour(0xff9c8a5a), 0, {}, { streamOut("buffer", "AudioBuffer") } },
        { "process_audio_block", "Process Audio Block", "Streams", juce::Colour(0xff9c8a5a), 0, { streamIn("in", "AudioBuffer") }, { streamOut("out", "AudioBuffer") } },
        { "audio_buffer_out", "Audio Buffer Out", "Streams", juce::Colour(0xff9c8a5a), 0, { streamIn("buffer", "AudioBuffer") }, {} },
        { "texture_resource", "Texture Resource", "Resources", juce::Colour(0xff9c5a9c), 0, {}, { resourceOut("texture", "Texture") } },
        { "sample_texture", "Sample Texture", "Resources", juce::Colour(0xff9c5a9c), 0, { resourceIn("texture", "Texture"), in("uv", "any") }, { out("color", "any") } }
        ,
        { "reroute", "Reroute", "Graph", juce::Colour(0xff7fffd4), 0, { in("in", "any") }, { out("out", "any") } },
        // A FRust programmable component's electrical interface (Workbench):
        // values are f64 volts, amps, seconds; each names a pin, parameter
        // or state variable of the component.
        { "pc_pin_voltage", "Pin Voltage", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("volts", "any") } },
        { "pc_pin_current", "Pin Current", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("amps", "any") } },
        { "pc_time", "Simulation Time", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("seconds", "any") } },
        { "pc_timestep", "Time Step", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("seconds", "any") } },
        { "pc_parameter", "Component Parameter", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("value", "any") } },
        { "pc_state_get", "Read State", "Component", juce::Colour(0xffc8a24b), 0, {}, { out("value", "any") } },
        { "pc_state_set", "Write State", "Component", juce::Colour(0xffc8a24b), 0, { in("value", "any") }, { out("value", "any") } },
        { "pc_drive", "Drive Pin", "Component", juce::Colour(0xffc8a24b), 0, { in("value", "any") }, { out("value", "any") } },
        { "pc_drive_current", "Drive Current", "Component", juce::Colour(0xffc8a24b), 0, { in("amps", "any") }, { out("value", "any") } },
        { "pc_set_resistance", "Pin Resistance", "Component", juce::Colour(0xffc8a24b), 0, { in("ohms", "any") }, { out("value", "any") } },
        { "state_machine_instance", "State Machine", "State Machine", juce::Colour(0xff4b8fc8), 0,
          { execIn("tick"), in("event", "string"), in("payload", "any") },
          { execOut("then"), out("state", "string"), out("transition", "string") } },
        { "sm_state", "State", "State Machine", juce::Colour(0xff4b8fc8), 0, { execIn("enter") }, { execOut("leave") } },
        { "sm_transition", "Transition", "State Machine", juce::Colour(0xff6cb6ff), 0, { execIn("from") }, { execOut("to") } },
        { "sm_event", "Event", "State Machine", juce::Colour(0xffb8722f), 0, {}, { out("event", "string") } }
    };
}

NodeDesignerPanel::NodeDesignerPanel()
    : templates(buildTemplates())
{
    palette = std::make_unique<PalettePanel>(*this);
    canvas = std::make_unique<GraphCanvas>(*this);
    inspector = std::make_unique<InspectorPanel>(*this);
    variablesPanel = std::make_unique<VariablesPanel>(*this);
    typesPanel = std::make_unique<TypesPanel>(*this);
    componentPanel = std::make_unique<ComponentPanel>(*this);
    addChildComponent(*componentPanel);

    titleLabel.setFont(juce::Font(16.0f, juce::Font::bold));
    titleLabel.setColour(juce::Label::textColourId, juce::Colour(0xff7fffd4));
    addAndMakeVisible(titleLabel);

    for (auto* button : { &newButton, &openButton, &saveButton, &validateButton, &compileButton, &saveSourceButton, &debugButton })
    {
        button->setColour(juce::TextButton::buttonColourId, juce::Colour(0xff263942));
        button->setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff0f6f8));
        addAndMakeVisible(*button);
    }
    newButton.onClick = [this] {
        newGraph(selectedDiagramType(), true);
        resized();
        repaint();
    };
    openButton.onClick = [this] { openGraph(); };
    saveButton.onClick = [this] { saveGraph(); };
    validateButton.onClick = [this] { showValidation(); };
    compileButton.onClick = [this] { compileGraph(); };
    saveSourceButton.onClick = [this] { saveGeneratedSource(); };
    debugButton.setTooltip("Start Debugging: run the program in the debugger (breakpoints, stepping, variables, watches)");
    debugButton.onClick = [this] { if (onDebugRequested != nullptr) onDebugRequested(); };

    diagramTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdce9ee));
    diagramTypeSelector.addItem("Node Graph", 1);
    diagramTypeSelector.addItem("State Machine", 2);
    diagramTypeSelector.setSelectedId(1, juce::dontSendNotification);
    diagramTypeSelector.onChange = [this] {
        const auto nextType = selectedDiagramType();
        if (nextType == diagramType) return;
        diagramType = nextType;
        if (diagramType == "state_machine")
            initializeStateMachine();
        else
            initializeNodeGraph();
        refreshProperties();
        resized();
        repaint();
    };
    addAndMakeVisible(diagramTypeLabel);
    addAndMakeVisible(diagramTypeSelector);

    targetLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdce9ee));
    targetSelector.addItem("Frust", 1);
    targetSelector.addItem("GLSL (production reference)", 2);
    targetSelector.setSelectedId(1, juce::dontSendNotification);
    targetSelector.onChange = [this] {
        const bool frust = selectedTarget() == "frust";
        frustProjectTypeLabel.setVisible(frust);
        frustProjectTypeSelector.setVisible(frust);
        if (selectedTarget() != "frust")
            setStatus("GLSL exists in production as reference. This research canvas is currently wired to the Frust backend.", true);
        refreshProperties();
        resized();
    };
    addAndMakeVisible(targetLabel);
    addAndMakeVisible(targetSelector);

    frustProjectTypeLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdce9ee));
    frustProjectTypeSelector.addItem("Executable pod (bin)", 1);
    frustProjectTypeSelector.addItem("Library pod (lib)", 2);
    frustProjectTypeSelector.setSelectedId(1, juce::dontSendNotification);
    frustProjectTypeSelector.onChange = [this] {
        frustProjectType = selectedFrustProjectType();
        setStatus("Frust output will be packaged as a Frate " + frustProjectType + " pod.");
    };
    addAndMakeVisible(frustProjectTypeLabel);
    addAndMakeVisible(frustProjectTypeSelector);

    propertiesTitle.setFont(juce::Font(15.0f, juce::Font::bold));
    propertiesTitle.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(propertiesTitle);
    addAndMakeVisible(*inspector);

    statusView.setMultiLine(false);
    statusView.setReadOnly(true);
    statusView.setScrollbarsShown(false);
    statusView.setFont(juce::Font(13.0f));
    statusView.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff182229));
    addAndMakeVisible(statusView);

    addAndMakeVisible(*palette);
    addAndMakeVisible(*variablesPanel);
    addAndMakeVisible(*typesPanel);
    addAndMakeVisible(*canvas);
    initializeUntitled();
}

NodeDesignerPanel::NodeDesignerPanel(const juce::File& fileToOpen)
    : NodeDesignerPanel()
{
    if (fileToOpen.existsAsFile())
        loadGraphFile(fileToOpen);
}

void NodeDesignerPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e2227));
}

void NodeDesignerPanel::resized()
{
    auto b = getLocalBounds().reduced(8);
    auto header = b.removeFromTop(30);
    titleLabel.setBounds(header.removeFromLeft(230));
    for (auto* button : { &newButton, &openButton, &saveButton, &validateButton, &compileButton, &saveSourceButton, &debugButton })
    {
        header.removeFromLeft(5);
        button->setBounds(header.removeFromLeft(button == &validateButton || button == &saveSourceButton ? 80 : (button == &compileButton || button == &debugButton ? 64 : 56)));
    }
    header.removeFromLeft(14);
    diagramTypeLabel.setBounds(header.removeFromLeft(58));
    diagramTypeSelector.setBounds(header.removeFromLeft(150));
    header.removeFromLeft(8);
    targetLabel.setBounds(header.removeFromLeft(48));
    targetSelector.setBounds(header.removeFromLeft(210));
    header.removeFromLeft(8);
    const bool showFrustKind = selectedTarget() == "frust";
    frustProjectTypeLabel.setVisible(showFrustKind);
    frustProjectTypeSelector.setVisible(showFrustKind);
    if (showFrustKind)
    {
        frustProjectTypeLabel.setBounds(header.removeFromLeft(45));
        frustProjectTypeSelector.setBounds(header.removeFromLeft(170));
    }
    b.removeFromTop(7);

    auto status = b.removeFromBottom(32);
    b.removeFromBottom(8);

    auto left = b.removeFromLeft(220);
    b.removeFromLeft(8);
    auto right = b.removeFromRight(270);
    b.removeFromRight(8);

    auto typesArea = left.removeFromBottom(230);
    left.removeFromBottom(8);
    auto variablesArea = left.removeFromBottom(160);
    left.removeFromBottom(8);
    palette->setBounds(left);
    variablesPanel->setBounds(variablesArea);
    typesPanel->setBounds(typesArea);
    if (componentPanel != nullptr && componentPanel->isVisible())
    {
        componentPanel->setBounds(right.removeFromTop(std::max(200, right.getHeight() * 11 / 20)));
        right.removeFromTop(8);
    }
    propertiesTitle.setBounds(right.removeFromTop(25).reduced(6, 0));
    inspector->setBounds(right);
    canvas->setBounds(b);
    statusView.setBounds(status);
}

NodeDesignerPanel::GraphNode* NodeDesignerPanel::findNode(int uid)
{
    auto it = std::find_if(nodes.begin(), nodes.end(), [uid](const GraphNode& n) { return n.uid == uid; });
    return it == nodes.end() ? nullptr : &*it;
}

const NodeDesignerPanel::GraphNode* NodeDesignerPanel::findNode(int uid) const
{
    auto it = std::find_if(nodes.begin(), nodes.end(), [uid](const GraphNode& n) { return n.uid == uid; });
    return it == nodes.end() ? nullptr : &*it;
}

const NodeDesignerPanel::NodeTemplate* NodeDesignerPanel::findTemplate(const juce::String& type) const
{
    auto it = std::find_if(templates.begin(), templates.end(), [&type](const NodeTemplate& t) { return t.type == type; });
    return it == templates.end() ? nullptr : &*it;
}

const NodeDesignerPanel::FunctionDef* NodeDesignerPanel::findFunction(const juce::String& id) const
{
    auto it = std::find_if(functions.begin(), functions.end(), [&id](const FunctionDef& f) {
        return f.id == id || f.name == id;
    });
    return it == functions.end() ? nullptr : &*it;
}

void NodeDesignerPanel::refreshTemplatesWithFunctions()
{
    templates = buildTemplates();

    for (const auto& fn : functions)
    {
        if (fn.id.isEmpty() || fn.name.isEmpty()) continue;
        if (fn.accessibility == "private") continue;

        NodeTemplate t;
        t.type = "call_function:" + fn.id;
        t.title = fn.namespaceName.isNotEmpty() ? fn.namespaceName + "." + fn.name : fn.name;
        t.category = "Functions";
        t.colour = juce::Colour(0xff4b8fc8);
        t.defaultLiteral = 0;
        t.functionRef = fn.id;

        if (fn.kind == "callable")
            t.inputs.push_back({ "in", "exec", PinFlow::Exec, true });
        for (const auto& input : fn.inputs)
            t.inputs.push_back({ input.name, input.type, PinFlow::Data, true });

        if (fn.kind == "callable")
            t.outputs.push_back({ "then", "exec", PinFlow::Exec, false });
        for (const auto& output : fn.outputs)
            t.outputs.push_back({ output.name, output.type, PinFlow::Data, false });

        templates.push_back(std::move(t));
    }

    if (palette != nullptr)
        palette->refreshRows();
}

int NodeDesignerPanel::nextUid()
{
    return nextNodeUid++;
}

juce::String NodeDesignerPanel::uniqueNodeId(const juce::String& base) const
{
    auto clean = base.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_").toLowerCase();
    if (clean.isEmpty()) clean = "node";
    auto candidate = clean;
    int suffix = 2;
    auto exists = [this](const juce::String& id) {
        return std::any_of(nodes.begin(), nodes.end(), [&id](const GraphNode& n) { return n.id == id; });
    };
    while (exists(candidate))
        candidate = clean + "_" + juce::String(suffix++);
    return candidate;
}

NodeDesignerPanel::GraphNode& NodeDesignerPanel::addNode(const juce::String& type, juce::Point<float> world)
{
    const auto* t = findTemplate(type);
    if (t == nullptr) t = findTemplate("literal_i64");

    GraphNode node;
    node.uid = nextUid();
    node.id = uniqueNodeId(baseIdForType(t->type));
    node.type = t->type;
    node.title = t->title;
    node.category = t->category;
    node.colour = t->colour;
    node.functionRef = t->functionRef;
    node.x = world.x;
    node.y = world.y;
    node.literalValue = t->defaultLiteral;
    node.inputs = t->inputs;
    node.outputs = t->outputs;
    if (node.type == "sm_state")
    {
        node.textValue = "State";
        node.accessibility = "package";
    }
    else if (node.type == "sm_transition")
    {
        node.textValue = "Transition";
        node.eventName = "event";
    }
    else if (node.type == "sm_event")
    {
        node.textValue = "Event";
        node.eventName = "event";
    }
    else if (node.type == "state_machine_instance")
    {
        node.textValue = "State Machine";
        node.machineRef = "";
    }
    for (const auto& input : node.inputs)
    {
        if (input.type == "bool") node.inputDefaults.push_back("false");
        else if (input.type == "string") node.inputDefaults.push_back("\"\"");
        else if (input.type == "any") node.inputDefaults.push_back("");
        else node.inputDefaults.push_back("0");
    }
    nodes.push_back(node);
    selectedNodeUid = node.uid;
    selectedConnectionIndex = -1;
    refreshProperties();
    setStatus("Added " + node.title + " node.");
    return nodes.back();
}

NodeDesignerPanel::GraphNode& NodeDesignerPanel::addRerouteNode(const Connection& connection, juce::Point<float> world)
{
    auto& node = addNode("reroute", world);
    const auto* source = findNode(connection.fromNode);
    if (source != nullptr && connection.fromPin >= 0 && connection.fromPin < (int)source->outputs.size())
    {
        const auto pin = source->outputs[(size_t)connection.fromPin];
        if (!node.inputs.empty())
        {
            node.inputs[0].type = pin.type;
            node.inputs[0].flow = pin.flow;
        }
        if (!node.outputs.empty())
        {
            node.outputs[0].type = pin.type;
            node.outputs[0].flow = pin.flow;
        }
    }
    node.title = "Reroute";
    node.textValue = "Reroute";
    setStatus("Inserted reroute node.");
    return node;
}

void NodeDesignerPanel::removeNode(int uid)
{
    connections.erase(std::remove_if(connections.begin(), connections.end(), [uid](const Connection& c) {
        return c.fromNode == uid || c.toNode == uid;
    }), connections.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [uid](const GraphNode& n) { return n.uid == uid; }), nodes.end());
}

const NodeDesignerPanel::Connection* NodeDesignerPanel::connectionToInput(int nodeUid, int inputPin) const
{
    auto it = std::find_if(connections.begin(), connections.end(), [nodeUid, inputPin](const Connection& c) {
        return c.toNode == nodeUid && c.toPin == inputPin;
    });
    return it == connections.end() ? nullptr : &*it;
}

const NodeDesignerPanel::GraphNode* NodeDesignerPanel::resolveRerouteUpstream(int nodeUid, int inputPin) const
{
    const auto* wire = connectionToInput(nodeUid, inputPin);
    auto* node = wire != nullptr ? findNode(wire->fromNode) : nullptr;
    std::set<int> visited;
    while (node != nullptr && node->type == "reroute" && visited.count(node->uid) == 0)
    {
        visited.insert(node->uid);
        wire = connectionToInput(node->uid, 0);
        node = wire != nullptr ? findNode(wire->fromNode) : nullptr;
    }
    return node;
}

const NodeDesignerPanel::GraphNode* NodeDesignerPanel::resolveRerouteDownstream(int nodeUid, int outputPin) const
{
    auto it = std::find_if(connections.begin(), connections.end(), [nodeUid, outputPin](const Connection& c) {
        return c.fromNode == nodeUid && c.fromPin == outputPin;
    });
    auto* node = it != connections.end() ? findNode(it->toNode) : nullptr;
    std::set<int> visited;
    while (node != nullptr && node->type == "reroute" && visited.count(node->uid) == 0)
    {
        visited.insert(node->uid);
        it = std::find_if(connections.begin(), connections.end(), [node](const Connection& c) {
            return c.fromNode == node->uid && c.fromPin == 0;
        });
        node = it != connections.end() ? findNode(it->toNode) : nullptr;
    }
    return node;
}

bool NodeDesignerPanel::pinsCompatible(const Pin& from, const Pin& to) const
{
    if (from.flow != to.flow) return false;
    return from.type == to.type || from.type == "any" || to.type == "any";
}

juce::String NodeDesignerPanel::validateStateMachine() const
{
    int stateCount = 0;
    int initialCount = 0;
    int transitionCount = 0;
    std::set<juce::String> stateIds;

    for (const auto& node : nodes)
    {
        if (node.type != "sm_state")
            continue;

        ++stateCount;
        if (node.id.trim().isEmpty())
            return "A state has no id.";
        if (stateIds.count(node.id) != 0)
            return "State id '" + node.id + "' is duplicated.";
        stateIds.insert(node.id);
        if (node.initial)
            ++initialCount;
    }

    if (stateCount == 0)
        return "A state machine needs at least one State node.";
    if (initialCount == 0)
        return "A state machine needs one initial State.";
    if (initialCount > 1)
        return "A state machine can only have one initial State.";

    for (const auto& node : nodes)
    {
        if (node.type != "sm_transition")
            continue;

        ++transitionCount;
        if (node.eventName.trim().isEmpty())
            return "Transition '" + node.id + "' needs an event name.";

        const auto* from = resolveRerouteUpstream(node.uid, 0);
        if (from == nullptr || from->type != "sm_state")
            return "Transition '" + node.id + "' needs a source State wired to its input.";

        const auto* to = resolveRerouteDownstream(node.uid, 0);
        if (to == nullptr || to->type != "sm_state")
            return "Transition '" + node.id + "' needs a destination State wired from its output.";
    }

    if (transitionCount == 0 && stateCount > 1)
        return "A multi-state machine needs at least one Transition.";

    return {};
}

void NodeDesignerPanel::connectPins(int fromNode, int fromPin, int toNode, int toPin)
{
    auto* a = findNode(fromNode);
    auto* b = findNode(toNode);
    if (a == nullptr || b == nullptr) return;
    if (fromPin < 0 || fromPin >= (int)a->outputs.size() || toPin < 0 || toPin >= (int)b->inputs.size()) return;
    if (!pinsCompatible(a->outputs[(size_t)fromPin], b->inputs[(size_t)toPin]))
    {
        setStatus("Those pins are not compatible.", true);
        return;
    }

    if (a->type == "sm_state" && b->type == "sm_state" && a->outputs[(size_t)fromPin].flow == PinFlow::Exec)
    {
        const auto fromStateUid = a->uid;
        const auto toStateUid = b->uid;
        const auto fromStateId = a->id;
        const auto toStateId = b->id;
        const auto fromStateName = a->textValue.isNotEmpty() ? a->textValue : a->id;
        const auto toStateName = b->textValue.isNotEmpty() ? b->textValue : b->id;
        const auto transitionId = uniqueNodeId(fromStateId + "_to_" + toStateId);
        const auto world = juce::Point<float> { (a->x + b->x) * 0.5f, (a->y + b->y) * 0.5f };

        auto& transition = addNode("sm_transition", world);
        transition.id = transitionId;
        transition.textValue = fromStateName + " to " + toStateName;
        transition.eventName = transitionId;
        const auto transitionUid = transition.uid;

        connections.push_back({ fromStateUid, fromPin, transitionUid, 0, connectionIdFor(fromStateUid, fromPin, transitionUid, 0) });
        connections.push_back({ transitionUid, 0, toStateUid, toPin, connectionIdFor(transitionUid, 0, toStateUid, toPin) });
        selectedNodeUid = transitionUid;
        refreshProperties();
        setStatus("Created transition " + transition.textValue + ". Edit its event and guard in Properties.");
        return;
    }

    const bool allowMultipleInput = b->type == "sm_state" && b->inputs[(size_t)toPin].flow == PinFlow::Exec;
    if (!allowMultipleInput)
    {
        connections.erase(std::remove_if(connections.begin(), connections.end(), [toNode, toPin](const Connection& c) {
            return c.toNode == toNode && c.toPin == toPin;
        }), connections.end());
    }
    connections.push_back({ fromNode, fromPin, toNode, toPin, connectionIdFor(fromNode, fromPin, toNode, toPin) });
    refreshProperties();
    setStatus("Connected " + a->title + " to " + b->title + ".");
}

juce::String NodeDesignerPanel::connectionIdFor(int fromNode, int fromPin, int toNode, int toPin) const
{
    // Stable, readable ids from the wire's ends: the same wiring gets the
    // same id, and a saved id is kept when the schematic is opened again.
    const auto* a = findNode(fromNode);
    const auto* b = findNode(toNode);
    auto pinName = [](const std::vector<Pin>& pins, int index) {
        return index >= 0 && index < (int)pins.size() ? pins[(size_t)index].name : juce::String(index);
    };
    const auto base = (a != nullptr ? a->id + "." + pinName(a->outputs, fromPin) : juce::String("?"))
        + "->" + (b != nullptr ? b->id + "." + pinName(b->inputs, toPin) : juce::String("?"));
    auto taken = [this](const juce::String& id) {
        return std::any_of(connections.begin(), connections.end(), [&id](const Connection& c) { return c.id == id; });
    };
    auto id = base;
    for (int n = 2; taken(id); ++n)
        id = base + "#" + juce::String(n);
    return id;
}

void NodeDesignerPanel::initializeUntitled()
{
    initializeNodeGraph();
}

void NodeDesignerPanel::initializeNodeGraph()
{
    currentGraphFile = {};
    loadedDocument = juce::var();
    generatedSource.clear();
    generatedFiles.clear();
    currentSourceMap.clear();
    diagramType = "node_graph";
    diagramTypeSelector.setSelectedId(1, juce::dontSendNotification);
    diagramName = "Untitled Node Schematic";
    titleLabel.setText(diagramName, juce::dontSendNotification);
    nodes.clear();
    connections.clear();
    variables.clear();
    types.clear();
    functions.clear();
    resources.clear();
    states.clear();
    events.clear();
    transitions.clear();
    nextNodeUid = 1;

    variables.push_back({ "scratch", "i64", "local", "0" });
    variables.push_back({ "inputGain", "f64", "read_only", "1.0" });
    variables.push_back({ "state", "i64", "read_write", "0" });
    types.push_back({ "Player",
                      "struct",
                      "struct",
                      { { "name", "string" }, { "health", "i64" }, { "isAwake", "bool" } },
                      {} });
    types.push_back({ "Command",
                      "algebraic_enum",
                      "tagged_int",
                      {},
                      { { "Move", { { "direction", "string" } }, 1 },
                        { "Look", {}, 2 },
                        { "Say", { { "text", "string" } }, 3 } } });
    refreshTemplatesWithFunctions();

    diagramName = "Hello Nodes";
    titleLabel.setText(diagramName, juce::dontSendNotification);

    // Keep uids, not references: each addNode may reallocate the node list
    // (the reference kept references here, so its starter wires went to
    // whatever the stale references held).
    const auto start = addNode("event_start", { 40.0f, 155.0f }).uid;
    auto& textNode = addNode("literal_string", { 40.0f, 260.0f });
    textNode.textValue = "Hello from nodes";
    const auto text = textNode.uid;
    const auto print = addNode("print", { 300.0f, 175.0f }).uid;
    const auto end = addNode("end", { 560.0f, 175.0f }).uid;

    connectPins(start, 0, print, 0);
    connectPins(text, 0, print, 1);
    connectPins(print, 0, end, 0);
    selectedNodeUid = print;
    refreshProperties();
    setStatus("Hello Nodes: execution starts at Start, Print writes a value, End terminates the diagram.");
}

void NodeDesignerPanel::initializeStateMachine()
{
    currentGraphFile = {};
    loadedDocument = juce::var();
    generatedSource.clear();
    generatedFiles.clear();
    currentSourceMap.clear();
    diagramType = "state_machine";
    diagramTypeSelector.setSelectedId(2, juce::dontSendNotification);
    diagramName = "Assistant State Machine";
    titleLabel.setText(diagramName, juce::dontSendNotification);

    nodes.clear();
    connections.clear();
    variables.clear();
    types.clear();
    functions.clear();
    resources.clear();
    states.clear();
    events.clear();
    transitions.clear();
    selectedNodeUid = 0;
    selectedConnectionIndex = -1;
    nextNodeUid = 1;

    variables.push_back({ "retryCount", "i64", "local", "0" });
    events.push_back({ "push_to_talk", "PushToTalkPressed", "" });
    events.push_back({ "speech_done", "UserFinishedSpeaking", "" });
    events.push_back({ "response_ready", "EngineerResponseReady", "" });
    events.push_back({ "cancel", "Cancel", "" });

    refreshTemplatesWithFunctions();

    auto& idle = addNode("sm_state", { 70.0f, 180.0f });
    idle.id = "idle";
    idle.textValue = "Idle";
    idle.entryAction = "restore_audio_routing";
    idle.initial = true;
    const auto idleUid = idle.uid;

    auto& listen = addNode("sm_state", { 390.0f, 95.0f });
    listen.id = "listening";
    listen.textValue = "Listening";
    listen.entryAction = "route_microphone_to_engineer";
    const auto listenUid = listen.uid;

    auto& think = addNode("sm_state", { 720.0f, 180.0f });
    think.id = "thinking";
    think.textValue = "Thinking";
    think.entryAction = "send_transcript_to_engineer";
    const auto thinkUid = think.uid;

    auto& speak = addNode("sm_state", { 390.0f, 330.0f });
    speak.id = "speaking";
    speak.textValue = "Speaking";
    speak.entryAction = "speak_response_to_headset";
    const auto speakUid = speak.uid;

    auto& t1 = addNode("sm_transition", { 285.0f, 180.0f });
    t1.id = "idle_to_listening";
    t1.textValue = "Idle to Listening";
    t1.eventName = "push_to_talk";
    const auto t1Uid = t1.uid;
    connectPins(idleUid, 0, t1Uid, 0);
    connectPins(t1Uid, 0, listenUid, 0);

    auto& t2 = addNode("sm_transition", { 600.0f, 95.0f });
    t2.id = "listening_to_thinking";
    t2.textValue = "Listening to Thinking";
    t2.eventName = "speech_done";
    const auto t2Uid = t2.uid;
    connectPins(listenUid, 0, t2Uid, 0);
    connectPins(t2Uid, 0, thinkUid, 0);

    auto& t3 = addNode("sm_transition", { 600.0f, 330.0f });
    t3.id = "thinking_to_speaking";
    t3.textValue = "Thinking to Speaking";
    t3.eventName = "response_ready";
    const auto t3Uid = t3.uid;
    connectPins(thinkUid, 0, t3Uid, 0);
    connectPins(t3Uid, 0, speakUid, 0);

    auto& t4 = addNode("sm_transition", { 180.0f, 330.0f });
    t4.id = "speaking_to_idle";
    t4.textValue = "Speaking to Idle";
    t4.eventName = "cancel";
    const auto t4Uid = t4.uid;
    connectPins(speakUid, 0, t4Uid, 0);
    connectPins(t4Uid, 0, idleUid, 0);

    selectedNodeUid = idleUid;
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    setStatus("State Machine: drag State and Transition nodes, wire exec pins to define the machine, and edit behavior in Properties.");
}

void NodeDesignerPanel::loadGraphFile(const juce::File& file)
{
    if (!file.existsAsFile())
    {
        setStatus("Node schematic does not exist: " + file.getFullPathName(), true);
        return;
    }
    juce::String problems;
    if (!loadFromJson(file.loadFileAsString(), problems))
        return;   // the status says why; the open graph is unchanged
    currentGraphFile = file;
    titleLabel.setText(file.getFileName(), juce::dontSendNotification);
    if (problems.isEmpty())
        setStatus("Opened " + file.getFullPathName());
    else
        setStatus("Opened " + file.getFileName() + " with problems: " + problems, true);
}

bool NodeDesignerPanel::loadFromJson(const juce::String& text, juce::String& problemSummary)
{
    // Documents that cannot be read are refused before anything changes.
    juce::var root;
    const auto parseResult = juce::JSON::parse(text, root);
    juce::String refusal;
    if (parseResult.failed())
        refusal = "the file is not valid JSON (" + parseResult.getErrorMessage() + ")";
    else if (root.getDynamicObject() == nullptr)
        refusal = "the file is not a JSON object";
    else if (root.hasProperty("schemaVersion") && (int)root.getProperty("schemaVersion", 0) > schematicFormatVersion)
        refusal = "schemaVersion " + root.getProperty("schemaVersion", {}).toString() + " is newer than this editor reads (up to "
            + juce::String(schematicFormatVersion) + ")";
    else if (root.hasProperty("nodes") && !root.getProperty("nodes", {}).isArray())
        refusal = "its nodes section is not a list";
    else if (root.hasProperty("connections") && !root.getProperty("connections", {}).isArray())
        refusal = "its connections section is not a list";
    if (refusal.isNotEmpty())
    {
        setStatus("Cannot open schematic: " + refusal + ".", true);
        return false;
    }
    juce::StringArray problems;
    loadedDocument = root;

    nodes.clear();
    connections.clear();
    variables.clear();
    types.clear();
    functions.clear();
    resources.clear();
    states.clear();
    events.clear();
    transitions.clear();
    nextNodeUid = 1;
    diagramName = propertyText(root, "name", "Untitled Node Schematic");
    titleLabel.setText(diagramName, juce::dontSendNotification);
    diagramType = propertyText(root, "diagramType", propertyText(root, "kind", "node_graph"));
    if (diagramType == "stateMachine") diagramType = "state_machine";
    diagramTypeSelector.setSelectedId(diagramType == "state_machine" ? 2 : 1, juce::dontSendNotification);
    frustProjectType = "bin";
    auto targetOptionsVar = root.getProperty("targetOptions", {});
    auto frustOptionsVar = targetOptionsVar.getProperty("frust", {});
    frustProjectType = propertyText(frustOptionsVar, "projectType", frustProjectType);
    frustProjectTypeSelector.setSelectedId(frustProjectType == "lib" ? 2 : 1, juce::dontSendNotification);
    std::map<juce::String, int> ids;

    auto resourcesVar = root.getProperty("resources", {});
    if (auto* arr = resourcesVar.getArray())
        for (const auto& r : *arr)
            resources.push_back({
                propertyText(r, "id", propertyText(r, "name", "resource")),
                propertyText(r, "name", "resource"),
                propertyText(r, "kind", "resource"),
                propertyText(r, "accessibility", "private")
            });

    auto eventsVar = root.getProperty("events", {});
    if (auto* arr = eventsVar.getArray())
        for (const auto& e : *arr)
            events.push_back({
                propertyText(e, "id", propertyText(e, "name", "event")),
                propertyText(e, "name", "event"),
                propertyText(e, "payloadType", propertyText(e, "message"))
            });

    auto statesVar = root.getProperty("states", {});
    if (auto* arr = statesVar.getArray())
        for (const auto& s : *arr)
            states.push_back({
                propertyText(s, "id", propertyText(s, "name", "state")),
                propertyText(s, "name", "State"),
                propertyText(s, "accessibility", "package"),
                propertyText(s, "entryAction", propertyText(s, "entry")),
                propertyText(s, "updateAction", propertyText(s, "update")),
                propertyText(s, "exitAction", propertyText(s, "exit")),
                propertyFloat(s, "x", 120.0f),
                propertyFloat(s, "y", 120.0f),
                (bool)s.getProperty("initial", false),
                (bool)s.getProperty("terminal", false)
            });

    auto transitionsVar = root.getProperty("transitions", {});
    if (auto* arr = transitionsVar.getArray())
        for (const auto& t : *arr)
            transitions.push_back({
                propertyText(t, "id", propertyText(t, "from") + "_to_" + propertyText(t, "to")),
                propertyText(t, "from"),
                propertyText(t, "to"),
                propertyText(t, "event"),
                propertyText(t, "guard"),
                propertyText(t, "action")
            });

    auto subgraphsVar = root.getProperty("subgraphs", {});
    if (auto* arr = subgraphsVar.getArray())
    {
        for (const auto& f : *arr)
        {
            FunctionDef def;
            def.id = propertyText(f, "id", propertyText(f, "name", "function"));
            def.name = propertyText(f, "name", propertyText(f, "title", def.id));
            def.namespaceName = propertyText(f, "namespace", propertyText(root, "namespace"));
            def.accessibility = propertyText(f, "accessibility", "package");
            def.kind = propertyText(f, "kind", "pure");
            auto ownerVar = f.getProperty("owner", {});
            def.owningPod = propertyText(ownerVar, "pod");
            def.owningVersion = propertyText(ownerVar, "version");

            auto inputsVar = f.getProperty("inputs", f.getProperty("params", {}));
            if (auto* inputs = inputsVar.getArray())
                for (const auto& p : *inputs)
                    def.inputs.push_back({ propertyText(p, "name", "value"), propertyText(p, "type", "i64") });

            auto outputsVar = f.getProperty("outputs", {});
            if (auto* outputs = outputsVar.getArray())
                for (const auto& p : *outputs)
                    def.outputs.push_back({ propertyText(p, "name", "result"), propertyText(p, "type", "i64") });
            else if (auto outputName = propertyText(f, "output"); outputName.isNotEmpty())
                def.outputs.push_back({ outputName, propertyText(f, "returnType", "i64") });

            functions.push_back(std::move(def));
        }
    }
    refreshTemplatesWithFunctions();

    auto paramsVar = root.getProperty("params", {});
    if (auto* params = paramsVar.getArray())
    {
        float y = 90.0f;
        for (const auto& p : *params)
        {
            auto& node = addNode("param_i64", { propertyFloat(p, "x", 20.0f), propertyFloat(p, "y", y) });
            node.id = propertyText(p, "name", "x");
            if (!node.outputs.empty()) node.outputs[0].name = node.id;
            ids[node.id] = node.uid;
            y += 88.0f;
        }
    }

    auto nodesVar = root.getProperty("nodes", {});
    if (auto* arr = nodesVar.getArray())
    {
        // Output pin names other nodes use on each node, for placeholders.
        std::map<juce::String, juce::StringArray> referencedOutputs, referencedInputs;
        if (auto* list = root.getProperty("connections", {}).getArray())
            for (const auto& c : *list)
            {
                referencedOutputs[propertyText(c.getProperty("from", {}), "node")].addIfNotAlreadyThere(propertyText(c.getProperty("from", {}), "pin"));
                referencedInputs[propertyText(c.getProperty("to", {}), "node")].addIfNotAlreadyThere(propertyText(c.getProperty("to", {}), "pin"));
            }
        for (const auto& n : *arr)
            if (auto* in = n.getProperty("inputs", {}).getArray())
                for (const auto& entry : *in)
                    if (entry.hasProperty("ref"))
                        referencedOutputs[propertyText(entry, "ref")].addIfNotAlreadyThere(propertyText(entry, "pin", "out"));

        for (int nodeIndex = 0; nodeIndex < arr->size(); ++nodeIndex)
        {
            const auto& n = arr->getReference(nodeIndex);
            if (!n.isObject())
            {
                problems.add("Node #" + juce::String(nodeIndex + 1) + " is not an object and was not loaded.");
                continue;
            }
            const auto type = propertyText(n, "type");
            const auto storedId = propertyText(n, "id");
            if (storedId.isEmpty() || type.isEmpty())
            {
                problems.add("Node #" + juce::String(nodeIndex + 1) + (storedId.isNotEmpty() ? " ('" + storedId + "')" : juce::String())
                    + (type.isNotEmpty() ? " (" + type + ")" : juce::String()) + " has no " + (storedId.isEmpty() ? "id" : "type")
                    + " and was not loaded.");
                continue;
            }
            if (ids.count(storedId) != 0)
                problems.add("Node id '" + storedId + "' is used by more than one node; wires naming it go to the last one.");
            juce::String templateType = type;
            if (type == "call_function")
            {
                auto fnVar = n.getProperty("function", {});
                const auto functionId = propertyText(fnVar, "id", propertyText(fnVar, "name", propertyText(n, "functionRef")));
                if (findFunction(functionId) == nullptr && functionId.isNotEmpty())
                {
                    FunctionDef external;
                    external.id = functionId;
                    external.name = propertyText(fnVar, "name", functionId);
                    external.namespaceName = propertyText(fnVar, "namespace");
                    external.accessibility = "public";
                    external.kind = propertyText(fnVar, "kind", "pure");
                    external.owningPod = propertyText(fnVar, "pod");
                    external.owningVersion = propertyText(fnVar, "version");

                    auto signatureInputs = fnVar.getProperty("inputs", {});
                    if (auto* inputs = signatureInputs.getArray())
                        for (const auto& p : *inputs)
                            external.inputs.push_back({ propertyText(p, "name", "value"), propertyText(p, "type", "any") });
                    else
                    {
                        auto callInputsVar = n.getProperty("inputs", {});
                        if (auto* callInputs = callInputsVar.getArray())
                            for (int i = 0; i < callInputs->size(); ++i)
                                external.inputs.push_back({ "arg" + juce::String(i + 1), "any" });
                    }

                    auto signatureOutputs = fnVar.getProperty("outputs", {});
                    if (auto* outputs = signatureOutputs.getArray())
                        for (const auto& p : *outputs)
                            external.outputs.push_back({ propertyText(p, "name", "result"), propertyText(p, "type", "any") });
                    else
                        external.outputs.push_back({ "result", propertyText(fnVar, "returnType", "any") });

                    functions.push_back(std::move(external));
                    refreshTemplatesWithFunctions();
                }
                templateType = "call_function:" + functionId;
            }
            const bool knownType = findTemplate(templateType) != nullptr;
            auto& node = addNode(templateType, { propertyFloat(n, "x", 220.0f), propertyFloat(n, "y", 120.0f) });
            node.id = storedId;
            node.type = type == "call_function" ? templateType : node.type;
            if (!knownType)
            {
                // No template for this type: a placeholder that keeps the type,
                // the pins its wires use, and everything stored on the node.
                node.type = type;
                node.title = type + " (unsupported)";
                node.category = "Unsupported";
                node.colour = juce::Colour(0xff6a6a6a);
                node.inputs.clear();
                node.outputs.clear();
                node.inputDefaults.clear();
                node.unsupportedSource = n;
                juce::StringArray inputNames;
                if (auto* in = n.getProperty("inputs", {}).getArray())
                    for (int i = 0; i < in->size(); ++i)
                        inputNames.add("in " + juce::String(i + 1));
                for (const auto& name : referencedInputs[storedId])
                    if (name.isNotEmpty()) inputNames.addIfNotAlreadyThere(name);
                for (const auto& name : inputNames)
                    node.inputs.push_back({ name, "any", PinFlow::Data, true });
                for (const auto& name : referencedOutputs[storedId])
                    if (name.isNotEmpty()) node.outputs.push_back({ name, "any", PinFlow::Data, false });
                node.inputDefaults.resize(node.inputs.size());
                problems.add("Node '" + storedId + "' has type '" + type + "', which this editor does not define; it is kept as it is.");
            }
            node.literalValue = propertyInt(n, "value", node.literalValue);
            node.textValue = propertyText(n, "text", node.textValue);
            node.accessibility = propertyText(n, "accessibility", node.accessibility);
            node.entryAction = propertyText(n, "entryAction", propertyText(n, "entry", node.entryAction));
            node.updateAction = propertyText(n, "updateAction", propertyText(n, "update", node.updateAction));
            node.exitAction = propertyText(n, "exitAction", propertyText(n, "exit", node.exitAction));
            node.eventName = propertyText(n, "event", node.eventName);
            node.payloadType = propertyText(n, "payloadType", node.payloadType);
            node.guardExpression = propertyText(n, "guard", node.guardExpression);
            node.transitionAction = propertyText(n, "action", node.transitionAction);
            node.machineRef = propertyText(n, "machineRef", node.machineRef);
            node.initial = (bool)n.getProperty("initial", node.initial);
            node.terminal = (bool)n.getProperty("terminal", node.terminal);
            if (node.type == "reroute")
            {
                const auto pinType = propertyText(n, "pinType", "any");
                const auto pinFlow = flowFromName(propertyText(n, "pinFlow", "data"));
                if (!node.inputs.empty())
                {
                    node.inputs[0].type = pinType;
                    node.inputs[0].flow = pinFlow;
                }
                if (!node.outputs.empty())
                {
                    node.outputs[0].type = pinType;
                    node.outputs[0].flow = pinFlow;
                }
            }
            node.functionRef = propertyText(n.getProperty("function", {}), "id", propertyText(n, "functionRef", node.functionRef));
            auto inputsVar = n.getProperty("inputs", {});
            if (auto* inputs = inputsVar.getArray())
            {
                for (int i = 0; i < inputs->size() && i < (int)node.inputDefaults.size(); ++i)
                {
                    const auto& input = inputs->getReference(i);
                    const auto value = propertyText(input, "default");
                    if (value.isNotEmpty())
                        node.inputDefaults[(size_t)i] = value;
                }
            }
            ids[node.id] = node.uid;
        }
    }

    // Wires. The connections section (written by this editor) names both
    // pins; older files only list each input's source node, optionally with
    // its output pin. A wire that cannot be restored is reported, not dropped.
    auto pinIndex = [](const std::vector<Pin>& pins, const juce::String& name) {
        for (int i = 0; i < (int)pins.size(); ++i)
            if (pins[(size_t)i].name == name) return i;
        return -1;
    };
    auto restore = [&](const juce::String& label, const juce::String& fromId, const juce::String& fromPinName, int fromFallback,
                       const juce::String& toId, const juce::String& toPinName, int toFallback, const juce::String& savedId) {
        const auto fromIt = ids.find(fromId);
        const auto toIt = ids.find(toId);
        if (fromIt == ids.end() || toIt == ids.end())
        {
            problems.add(label + " names node '" + (fromIt == ids.end() ? fromId : toId) + "', which is not in the file; not restored.");
            return;
        }
        const auto* a = findNode(fromIt->second);
        const auto* b = findNode(toIt->second);
        int fromPin = fromPinName.isNotEmpty() ? pinIndex(a->outputs, fromPinName) : fromFallback;
        int toPin = toPinName.isNotEmpty() ? pinIndex(b->inputs, toPinName) : toFallback;
        if (fromPin < 0 || fromPin >= (int)a->outputs.size())
        {
            problems.add(label + ": '" + fromId + "' has no output '" + (fromPinName.isNotEmpty() ? fromPinName : juce::String(fromFallback))
                + (pinIndex(a->inputs, fromPinName) >= 0 ? "' (that is an input)" : "'") + "; not restored.");
            return;
        }
        if (toPin < 0 || toPin >= (int)b->inputs.size())
        {
            problems.add(label + ": '" + toId + "' has no input '" + (toPinName.isNotEmpty() ? toPinName : juce::String(toFallback))
                + (pinIndex(b->outputs, toPinName) >= 0 ? "' (that is an output)" : "'") + "; not restored.");
            return;
        }
        const bool multiDrive = b->type == "sm_state" && b->inputs[(size_t)toPin].flow == PinFlow::Exec;
        if (!multiDrive && connectionToInput(b->uid, toPin) != nullptr)
        {
            problems.add(label + ": input '" + toId + "." + b->inputs[(size_t)toPin].name + "' is already wired; the first wire was kept.");
            return;
        }
        if (!pinsCompatible(a->outputs[(size_t)fromPin], b->inputs[(size_t)toPin]))
        {
            problems.add(label + " joins " + a->outputs[(size_t)fromPin].type + " to " + b->inputs[(size_t)toPin].type
                + " (incompatible pins); not restored.");
            return;
        }
        const int fromUid = a->uid, toUid = b->uid;
        connectPins(fromUid, fromPin, toUid, toPin);
        if (savedId.isNotEmpty())
            for (auto& c : connections)
                if (c.fromNode == fromUid && c.fromPin == fromPin && c.toNode == toUid && c.toPin == toPin)
                    c.id = savedId;
    };
    if (auto* list = root.getProperty("connections", {}).getArray())
    {
        for (int i = 0; i < list->size(); ++i)
        {
            const auto& c = list->getReference(i);
            const auto savedId = propertyText(c, "id");
            const auto label = "Connection " + (savedId.isNotEmpty() ? "'" + savedId + "'" : "#" + juce::String(i + 1));
            const auto from = c.getProperty("from", {});
            const auto to = c.getProperty("to", {});
            if (propertyText(from, "node").isEmpty() || propertyText(from, "pin").isEmpty()
                || propertyText(to, "node").isEmpty() || propertyText(to, "pin").isEmpty())
            {
                problems.add(label + " does not name both nodes and pins; not restored.");
                continue;
            }
            restore(label, propertyText(from, "node"), propertyText(from, "pin"), -1, propertyText(to, "node"), propertyText(to, "pin"), -1, savedId);
        }
    }
    else if (auto* arr = root.getProperty("nodes", {}).getArray())
    {
        for (const auto& n : *arr)
        {
            const auto toId = propertyText(n, "id");
            auto* inputs = n.getProperty("inputs", {}).getArray();
            if (toId.isEmpty() || inputs == nullptr) continue;
            for (int i = 0; i < inputs->size(); ++i)
            {
                const auto& input = inputs->getReference(i);
                auto sourceId = propertyText(input, "ref");
                if (sourceId.isEmpty()) sourceId = propertyText(input, "param");
                if (sourceId.isEmpty()) continue;
                restore("The wire into " + toId + " input " + juce::String(i + 1), sourceId, propertyText(input, "pin"), 0, toId, {}, i, {});
            }
        }
    }

    if (diagramType == "state_machine" && nodes.empty() && !states.empty())
    {
        std::map<juce::String, int> stateNodeUids;
        for (const auto& s : states)
        {
            auto& node = addNode("sm_state", { s.x, s.y });
            node.id = s.id;
            node.textValue = s.name;
            node.accessibility = s.accessibility;
            node.entryAction = s.entryAction;
            node.updateAction = s.updateAction;
            node.exitAction = s.exitAction;
            node.initial = s.initial;
            node.terminal = s.terminal;
            stateNodeUids[s.id] = node.uid;
        }

        for (const auto& t : transitions)
        {
            const auto fromIt = stateNodeUids.find(t.from);
            const auto toIt = stateNodeUids.find(t.to);
            if (fromIt == stateNodeUids.end() || toIt == stateNodeUids.end())
                continue;

            const auto* fromState = findNode(fromIt->second);
            const auto* toState = findNode(toIt->second);
            const auto x = fromState != nullptr && toState != nullptr ? (fromState->x + toState->x) * 0.5f : 260.0f;
            const auto y = fromState != nullptr && toState != nullptr ? (fromState->y + toState->y) * 0.5f : 160.0f;
            auto& node = addNode("sm_transition", { x, y });
            node.id = t.id;
            node.textValue = t.id;
            node.eventName = t.event;
            node.guardExpression = t.guard;
            node.transitionAction = t.action;
            const auto transitionUid = node.uid;
            connectPins(fromIt->second, 0, transitionUid, 0);
            connectPins(transitionUid, 0, toIt->second, 0);
        }
    }

    auto variablesVar = root.getProperty("variables", {});
    if (auto* arr = variablesVar.getArray())
    {
        for (const auto& v : *arr)
            variables.push_back({
                propertyText(v, "name", "value"),
                propertyText(v, "type", "i64"),
                propertyText(v, "access", "local"),
                propertyText(v, "default", "0")
            });
    }

    auto typesVar = root.getProperty("types", {});
    if (auto* arr = typesVar.getArray())
    {
        for (const auto& t : *arr)
        {
            TypeDef def;
            def.name = propertyText(t, "name", "Type");
            def.kind = propertyText(t, "kind", "struct");
            def.targetFallback = propertyText(t, "targetFallback", def.kind == "struct" ? "struct" : "tagged_int");

            auto fieldsVar = t.getProperty("fields", {});
            if (auto* fields = fieldsVar.getArray())
                for (const auto& f : *fields)
                    def.fields.push_back({ propertyText(f, "name", "field"), propertyText(f, "type", "i64") });

            auto variantsVar = t.getProperty("variants", {});
            if (auto* variants = variantsVar.getArray())
                for (const auto& v : *variants)
                {
                    TypeVariant variant;
                    variant.name = propertyText(v, "name", "Case");
                    variant.intValue = propertyInt(v, "intValue", (int)def.variants.size() + 1);
                    auto payloadVar = v.getProperty("fields", {});
                    if (auto* payload = payloadVar.getArray())
                        for (const auto& f : *payload)
                            variant.fields.push_back({ propertyText(f, "name", "value"), propertyText(f, "type", "i64") });
                    def.variants.push_back(variant);
                }

            types.push_back(def);
        }
    }

    auto debugVar = root.getProperty("debug", {});
    auto breakpointsVar = debugVar.getProperty("breakpoints", {});
    if (auto* breakpoints = breakpointsVar.getArray())
        for (const auto& bp : *breakpoints)
            if (auto* n = findNode(ids[propertyText(bp.getProperty("target", {}), "nodeId")]))
            {
                n->breakpoint = true;
                n->breakpointEnabled = (bool)bp.getProperty("enabled", true);
            }

    auto watchesVar = debugVar.getProperty("watches", {});
    if (auto* watches = watchesVar.getArray())
        for (const auto& watch : *watches)
            if (auto* n = findNode(ids[propertyText(watch.getProperty("target", {}), "nodeId")]))
                n->watched = (bool)watch.getProperty("enabled", true);

    selectedNodeUid = nodes.empty() ? 0 : nodes.back().uid;
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    if (variablesPanel != nullptr) variablesPanel->repaint();
    if (typesPanel != nullptr) typesPanel->repaint();

    for (const auto& p : validateGraph())
        problems.addIfNotAlreadyThere(p);
    problemSummary = problems.joinIntoString(" ");
    return true;
}

juce::String NodeDesignerPanel::quoted(const juce::String& text)
{
    return juce::JSON::toString(juce::var(text));
}

juce::String NodeDesignerPanel::outputNodeId() const
{
    if (const auto* selected = findNode(selectedNodeUid); selected != nullptr && selected->type != "param_i64" && selected->type != "reroute")
        return selected->id;
    for (const auto& n : nodes)
        if (n.id == "result" && n.type != "param_i64" && n.type != "reroute")
            return n.id;
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
        if (it->type != "param_i64" && it->type != "reroute")
            return it->id;
    return {};
}

juce::String NodeDesignerPanel::buildCompilerJson(bool includeRoutingNodes) const
{
    juce::String text;
    text << "{\n";
    text << "  \"functionName\": \"compute\",\n";
    text << "  \"params\": [\n";
    bool firstParam = true;
    for (const auto& n : nodes)
    {
        if (n.type != "param_i64") continue;
        if (!firstParam) text << ",\n";
        firstParam = false;
        text << "    { \"name\": " << quoted(n.id) << ", \"type\": \"i64\", \"x\": " << n.x << ", \"y\": " << n.y << " }";
    }
    text << "\n  ],\n";
    text << "  \"output\": " << quoted(outputNodeId()) << ",\n";
    text << "  \"nodes\": [\n";

    bool firstNode = true;
    for (const auto& n : nodes)
    {
        if (n.type == "param_i64") continue;
        if (!includeRoutingNodes && n.type == "reroute") continue;
        if (!firstNode) text << ",\n";
        firstNode = false;
        const bool functionCall = n.type.startsWith("call_function:");
        text << "    { \"id\": " << quoted(n.id) << ", \"type\": " << quoted(functionCall ? "call_function" : n.type)
             << ", \"x\": " << n.x << ", \"y\": " << n.y;
        if (functionCall)
        {
            const auto* fn = findFunction(n.functionRef);
            text << ", \"function\": { \"id\": " << quoted(n.functionRef);
            if (fn != nullptr)
            {
                text << ", \"namespace\": " << quoted(fn->namespaceName)
                     << ", \"name\": " << quoted(fn->name);
                if (fn->owningPod.isNotEmpty())
                    text << ", \"pod\": " << quoted(fn->owningPod);
                if (fn->owningVersion.isNotEmpty())
                    text << ", \"version\": " << quoted(fn->owningVersion);
            }
            text << " }";
        }
        if (n.type == "literal_i64")
            text << ", \"value\": " << n.literalValue;
        else if (n.type == "literal_f64")
            text << ", \"value\": " << juce::String(n.textValue.getDoubleValue(), 15);
        else if (n.type == "const_bool")
            text << ", \"value\": " << (n.literalValue != 0 ? "true" : "false");
        if (usesTextField(n.type) || n.type.startsWith("sm_") || n.type == "state_machine_instance")
            text << ", \"text\": " << quoted(n.textValue);
        if (n.type == "sm_state")
        {
            text << ", \"accessibility\": " << quoted(n.accessibility)
                 << ", \"initial\": " << (n.initial ? "true" : "false")
                 << ", \"terminal\": " << (n.terminal ? "true" : "false");
            if (n.entryAction.isNotEmpty()) text << ", \"entryAction\": " << quoted(n.entryAction);
            if (n.updateAction.isNotEmpty()) text << ", \"updateAction\": " << quoted(n.updateAction);
            if (n.exitAction.isNotEmpty()) text << ", \"exitAction\": " << quoted(n.exitAction);
        }
        else if (n.type == "sm_transition")
        {
            text << ", \"event\": " << quoted(n.eventName);
            if (n.guardExpression.isNotEmpty()) text << ", \"guard\": " << quoted(n.guardExpression);
            if (n.transitionAction.isNotEmpty()) text << ", \"action\": " << quoted(n.transitionAction);
        }
        else if (n.type == "sm_event")
        {
            text << ", \"event\": " << quoted(n.eventName);
            if (n.payloadType.isNotEmpty()) text << ", \"payloadType\": " << quoted(n.payloadType);
        }
        else if (n.type == "reroute")
        {
            const auto pinType = !n.outputs.empty() ? n.outputs[0].type : juce::String("any");
            const auto pinFlow = !n.outputs.empty() ? n.outputs[0].flow : PinFlow::Data;
            text << ", \"pinType\": " << quoted(pinType)
                 << ", \"pinFlow\": " << quoted(flowName(pinFlow));
        }
        else if (n.type == "state_machine_instance")
        {
            text << ", \"machineRef\": " << quoted(n.machineRef);
        }

        if (!n.inputs.empty())
        {
            text << ", \"inputs\": [ ";
            for (int i = 0; i < (int)n.inputs.size(); ++i)
            {
                if (i != 0) text << ", ";
                auto connIt = std::find_if(connections.begin(), connections.end(), [&n, i](const Connection& c) {
                    return c.toNode == n.uid && c.toPin == i;
                });
                if (connIt == connections.end())
                {
                    const auto value = i < (int)n.inputDefaults.size() ? n.inputDefaults[(size_t)i] : juce::String();
                    text << "{ \"default\": " << quoted(value) << " }";
                }
                else
                {
                    auto* source = findNode(connIt->fromNode);
                    int sourcePin = connIt->fromPin;
                    std::set<int> visited;
                    while (!includeRoutingNodes && source != nullptr && source->type == "reroute" && visited.count(source->uid) == 0)
                    {
                        visited.insert(source->uid);
                        const auto* upstream = connectionToInput(source->uid, 0);
                        source = upstream != nullptr ? findNode(upstream->fromNode) : nullptr;
                        sourcePin = upstream != nullptr ? upstream->fromPin : 0;
                    }
                    if (source != nullptr && source->type == "param_i64")
                        text << "{ \"param\": " << quoted(source->id) << " }";
                    else if (source != nullptr)
                    {
                        // The output pin, so a wire from a node's second or third
                        // output returns to it (the reference wrote only the node).
                        text << "{ \"ref\": " << quoted(source->id);
                        if (sourcePin >= 0 && sourcePin < (int)source->outputs.size())
                            text << ", \"pin\": " << quoted(source->outputs[(size_t)sourcePin].name);
                        text << " }";
                    }
                    else
                        text << "{ \"ref\": \"__missing_input__\" }";
                }
            }
            text << " ]";
        }
        text << " }";
    }
    text << "\n  ]";
    if (componentInfo.isObject())
        text << ",\n  \"component\": " << juce::JSON::toString(componentInfo, true);
    text << "\n}";
    return text;
}

juce::String NodeDesignerPanel::buildSchematicJson(bool includeRoutingNodes) const
{
    auto text = buildCompilerJson(includeRoutingNodes);
    auto insert = juce::String("  \"schemaVersion\": ") + juce::String(schematicFormatVersion) + ",\n"
        + "  \"name\": " + quoted(diagramName) + ",\n"
        + "  \"diagramType\": " + quoted(diagramType) + ",\n"
        + "  \"targets\": [ \"frust\" ],\n"
        + "  \"targetOptions\": { \"frust\": { \"functionName\": \"compute\", \"projectType\": " + quoted(selectedFrustProjectType()) + " }, \"glsl\": { \"referenceOnly\": true } },\n"
        + "  \"accessibility\": \"graph\",\n"
        + "  \"agentAccess\": { \"visible\": true, \"editable\": true, \"requiresApproval\": false },\n";
    const auto prefixLength = juce::String("{\n").length();
    if (text.startsWith("{\n"))
        text = "{\n" + insert + text.substring(prefixLength);

    juce::String vars;
    vars << ",\n  \"variables\": [";
    bool firstVar = true;
    for (const auto& v : variables)
    {
        if (!firstVar) vars << ", ";
        firstVar = false;
        vars << "{ \"name\": " << quoted(v.name)
             << ", \"type\": " << quoted(v.type)
             << ", \"access\": " << quoted(v.access)
             << ", \"default\": " << quoted(v.defaultValue) << " }";
    }
    vars << "]";

    juce::String typeJson;
    typeJson << ",\n  \"types\": [";
    bool firstType = true;
    for (const auto& t : types)
    {
        if (!firstType) typeJson << ", ";
        firstType = false;
        typeJson << "{ \"name\": " << quoted(t.name)
                 << ", \"kind\": " << quoted(t.kind)
                 << ", \"targetFallback\": " << quoted(t.targetFallback);

        if (!t.fields.empty())
        {
            typeJson << ", \"fields\": [";
            bool firstField = true;
            for (const auto& f : t.fields)
            {
                if (!firstField) typeJson << ", ";
                firstField = false;
                typeJson << "{ \"name\": " << quoted(f.name) << ", \"type\": " << quoted(f.type) << " }";
            }
            typeJson << "]";
        }

        if (!t.variants.empty())
        {
            typeJson << ", \"variants\": [";
            bool firstVariant = true;
            for (const auto& v : t.variants)
            {
                if (!firstVariant) typeJson << ", ";
                firstVariant = false;
                typeJson << "{ \"name\": " << quoted(v.name) << ", \"intValue\": " << v.intValue;
                if (!v.fields.empty())
                {
                    typeJson << ", \"fields\": [";
                    bool firstField = true;
                    for (const auto& f : v.fields)
                    {
                        if (!firstField) typeJson << ", ";
                        firstField = false;
                        typeJson << "{ \"name\": " << quoted(f.name) << ", \"type\": " << quoted(f.type) << " }";
                    }
                    typeJson << "]";
                }
                typeJson << " }";
            }
            typeJson << "]";
        }

        typeJson << " }";
    }
    typeJson << "]";

    juce::String functionJson;
    functionJson << ",\n  \"subgraphs\": [";
    bool firstFunction = true;
    for (const auto& f : functions)
    {
        if (!firstFunction) functionJson << ", ";
        firstFunction = false;
        functionJson << "{ \"id\": " << quoted(f.id)
                     << ", \"name\": " << quoted(f.name)
                     << ", \"namespace\": " << quoted(f.namespaceName)
                     << ", \"kind\": " << quoted(f.kind)
                     << ", \"accessibility\": " << quoted(f.accessibility);
        if (f.owningPod.isNotEmpty() || f.owningVersion.isNotEmpty())
            functionJson << ", \"owner\": { \"pod\": " << quoted(f.owningPod)
                         << ", \"version\": " << quoted(f.owningVersion) << " }";

        functionJson << ", \"inputs\": [";
        bool firstPort = true;
        for (const auto& p : f.inputs)
        {
            if (!firstPort) functionJson << ", ";
            firstPort = false;
            functionJson << "{ \"name\": " << quoted(p.name) << ", \"type\": " << quoted(p.type) << " }";
        }
        functionJson << "], \"outputs\": [";
        firstPort = true;
        for (const auto& p : f.outputs)
        {
            if (!firstPort) functionJson << ", ";
            firstPort = false;
            functionJson << "{ \"name\": " << quoted(p.name) << ", \"type\": " << quoted(p.type) << " }";
        }
        functionJson << "], \"nodes\": [] }";
    }
    functionJson << "]";

    juce::String resourceJson;
    resourceJson << ",\n  \"resources\": [";
    bool firstResource = true;
    for (const auto& r : resources)
    {
        if (!firstResource) resourceJson << ", ";
        firstResource = false;
        resourceJson << "{ \"id\": " << quoted(r.id)
                     << ", \"name\": " << quoted(r.name)
                     << ", \"kind\": " << quoted(r.kind)
                     << ", \"accessibility\": " << quoted(r.accessibility) << " }";
    }
    resourceJson << "]";

    juce::String eventJson;
    eventJson << ",\n  \"events\": [";
    bool firstEvent = true;
    std::set<juce::String> exportedEvents;
    auto writeEvent = [&](const juce::String& id, const juce::String& name, const juce::String& payload) {
        auto cleanId = id.isNotEmpty() ? id : name.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_").toLowerCase();
        if (cleanId.isEmpty() || exportedEvents.count(cleanId) != 0) return;
        exportedEvents.insert(cleanId);
        {
            if (!firstEvent) eventJson << ", ";
            firstEvent = false;
            eventJson << "{ \"id\": " << quoted(cleanId)
                      << ", \"name\": " << quoted(name.isNotEmpty() ? name : cleanId);
            if (payload.isNotEmpty())
                eventJson << ", \"payloadType\": " << quoted(payload);
            eventJson << " }";
        }
    };
    for (const auto& e : events)
        writeEvent(e.id, e.name, e.payloadType);
    for (const auto& n : nodes)
    {
        if (n.type == "sm_event")
            writeEvent(n.eventName.isNotEmpty() ? n.eventName : n.id, n.textValue.isNotEmpty() ? n.textValue : n.eventName, n.payloadType);
        else if (n.type == "sm_transition")
            writeEvent(n.eventName, n.eventName, {});
    }
    eventJson << "]";

    juce::String stateJson;
    stateJson << ",\n  \"states\": [";
    bool firstState = true;
    for (const auto& n : nodes)
    {
        if (n.type != "sm_state") continue;
        if (!firstState) stateJson << ", ";
        firstState = false;
        stateJson << "{ \"id\": " << quoted(n.id)
                  << ", \"name\": " << quoted(n.textValue.isNotEmpty() ? n.textValue : n.id)
                  << ", \"accessibility\": " << quoted(n.accessibility)
                  << ", \"x\": " << n.x
                  << ", \"y\": " << n.y
                  << ", \"initial\": " << (n.initial ? "true" : "false")
                  << ", \"terminal\": " << (n.terminal ? "true" : "false");
        if (n.entryAction.isNotEmpty()) stateJson << ", \"entryAction\": " << quoted(n.entryAction);
        if (n.updateAction.isNotEmpty()) stateJson << ", \"updateAction\": " << quoted(n.updateAction);
        if (n.exitAction.isNotEmpty()) stateJson << ", \"exitAction\": " << quoted(n.exitAction);
        stateJson << " }";
    }
    stateJson << "]";

    juce::String transitionJson;
    transitionJson << ",\n  \"transitions\": [";
    bool firstTransition = true;
    for (const auto& n : nodes)
    {
        if (n.type != "sm_transition") continue;
        const auto* from = [&]() -> const GraphNode* {
            auto it = std::find_if(connections.begin(), connections.end(), [&n](const Connection& c) {
                return c.toNode == n.uid && c.toPin == 0;
            });
            auto* node = it == connections.end() ? nullptr : findNode(it->fromNode);
            std::set<int> visited;
            while (node != nullptr && node->type == "reroute" && visited.count(node->uid) == 0)
            {
                visited.insert(node->uid);
                const auto* upstream = connectionToInput(node->uid, 0);
                node = upstream != nullptr ? findNode(upstream->fromNode) : nullptr;
            }
            return node;
        }();
        const auto* to = [&]() -> const GraphNode* {
            auto it = std::find_if(connections.begin(), connections.end(), [&n](const Connection& c) {
                return c.fromNode == n.uid && c.fromPin == 0;
            });
            auto* node = it == connections.end() ? nullptr : findNode(it->toNode);
            std::set<int> visited;
            while (node != nullptr && node->type == "reroute" && visited.count(node->uid) == 0)
            {
                visited.insert(node->uid);
                auto next = std::find_if(connections.begin(), connections.end(), [node](const Connection& c) {
                    return c.fromNode == node->uid && c.fromPin == 0;
                });
                node = next != connections.end() ? findNode(next->toNode) : nullptr;
            }
            return node;
        }();
        if (from == nullptr || to == nullptr || from->type != "sm_state" || to->type != "sm_state")
            continue;
        if (!firstTransition) transitionJson << ", ";
        firstTransition = false;
        transitionJson << "{ \"id\": " << quoted(n.id)
                       << ", \"from\": " << quoted(from->id)
                       << ", \"to\": " << quoted(to->id)
                       << ", \"event\": " << quoted(n.eventName);
        if (n.guardExpression.isNotEmpty()) transitionJson << ", \"guard\": " << quoted(n.guardExpression);
        if (n.transitionAction.isNotEmpty()) transitionJson << ", \"action\": " << quoted(n.transitionAction);
        transitionJson << " }";
    }
    transitionJson << "]";

    juce::String exports;
    exports << ",\n  \"exports\": { \"functions\": [";
    bool firstExport = true;
    for (const auto& f : functions)
    {
        if (f.accessibility != "public" && f.accessibility != "project" && f.accessibility != "package")
            continue;
        if (!firstExport) exports << ", ";
        firstExport = false;
        exports << quoted((f.namespaceName.isNotEmpty() ? f.namespaceName + "." : juce::String()) + f.name);
    }
    exports << "], \"types\": [";
    firstExport = true;
    for (const auto& t : types)
    {
        if (!firstExport) exports << ", ";
        firstExport = false;
        exports << quoted(t.name);
    }
    exports << "], \"resources\": [";
    firstExport = true;
    for (const auto& r : resources)
    {
        if (r.accessibility != "public" && r.accessibility != "project" && r.accessibility != "package")
            continue;
        if (!firstExport) exports << ", ";
        firstExport = false;
        exports << quoted(r.name);
    }
    exports << "] }";

    // Every wire with both of its pins: an input may take several wires (a
    // state entered by several transitions), which the inputs refs cannot say.
    juce::String connectionJson;
    connectionJson << ",\n  \"connections\": [";
    bool firstConnection = true;
    for (const auto& c : connections)
    {
        const auto* a = findNode(c.fromNode);
        const auto* b = findNode(c.toNode);
        if (a == nullptr || b == nullptr || c.fromPin < 0 || c.fromPin >= (int)a->outputs.size() || c.toPin < 0 || c.toPin >= (int)b->inputs.size())
            continue;
        if (!includeRoutingNodes && (a->type == "reroute" || b->type == "reroute"))
            continue;
        if (!firstConnection) connectionJson << ", ";
        firstConnection = false;
        connectionJson << "{ \"id\": " << quoted(c.id)
                       << ", \"from\": { \"node\": " << quoted(a->id) << ", \"pin\": " << quoted(a->outputs[(size_t)c.fromPin].name) << " }"
                       << ", \"to\": { \"node\": " << quoted(b->id) << ", \"pin\": " << quoted(b->inputs[(size_t)c.toPin].name) << " } }";
    }
    connectionJson << "]";

    juce::String debug;
    debug << ",\n  \"debug\": {\n";
    debug << "    \"breakpoints\": [";
    bool first = true;
    for (const auto& n : nodes)
    {
        if (!n.breakpoint) continue;
        if (!first) debug << ", ";
        first = false;
        debug << "{ \"id\": " << quoted("bp_" + n.id) << ", \"target\": { \"kind\": \"node\", \"nodeId\": "
              << quoted(n.id) << " }, \"enabled\": " << (n.breakpointEnabled ? "true" : "false") << ", \"condition\": \"\", \"hitCount\": 0 }";
    }
    debug << "],\n    \"watches\": [";
    first = true;
    for (const auto& n : nodes)
    {
        if (!n.watched) continue;
        if (!first) debug << ", ";
        first = false;
        debug << "{ \"id\": " << quoted("watch_" + n.id) << ", \"target\": { \"kind\": \"pin\", \"nodeId\": "
              << quoted(n.id) << ", \"pin\": \"output\" }, \"enabled\": true, \"label\": "
              << quoted(n.title + " output") << " }";
    }
    debug << "]\n  }";
    text = text.upToLastOccurrenceOf("\n}", false, false) + vars + typeJson + functionJson + resourceJson
        + eventJson + stateJson + transitionJson + connectionJson + exports + debug + "\n}";

    auto root = juce::JSON::parse(text);
    if (!root.isObject())
        return text;
    static const juce::StringArray clearable { "entryAction", "updateAction", "exitAction", "guard", "action", "payloadType" };
    carryForwardUnmanaged(root, loadedDocument, clearable);
    // Placeholders for unsupported types are saved with everything they had.
    if (auto* saved = root.getProperty("nodes", {}).getArray())
        for (auto& v : *saved)
            if (auto* o = v.getDynamicObject())
                for (const auto& n : nodes)
                    if (n.unsupportedSource.isObject() && n.id == o->getProperty("id").toString())
                        for (const auto& p : n.unsupportedSource.getDynamicObject()->getProperties())
                            if (!o->hasProperty(p.name))
                                o->setProperty(p.name, p.value.clone());
    return juce::JSON::toString(root, true);
}

juce::String NodeDesignerPanel::selectedDiagramType() const
{
    return diagramTypeSelector.getSelectedId() == 2 ? "state_machine" : "node_graph";
}

juce::String NodeDesignerPanel::selectedTarget() const
{
    return targetSelector.getSelectedId() == 2 ? "glsl" : "frust";
}

juce::String NodeDesignerPanel::selectedFrustProjectType() const
{
    return frustProjectTypeSelector.getSelectedId() == 2 ? "lib" : "bin";
}

void NodeDesignerPanel::refreshProperties()
{
    if (inspector != nullptr)
        inspector->refreshFromModel();
}

void NodeDesignerPanel::setStatus(const juce::String& text, bool isError)
{
    statusView.setColour(juce::TextEditor::textColourId, isError ? juce::Colour(0xffff8b8b) : juce::Colour(0xffdce9ee));
    statusView.setText(text, juce::dontSendNotification);
}

// ---------------------------------------------------------------------------
// Workbench integration: project folder, open/save/validate buttons, and the
// operations the Workbench agent (node_program_* tools) and tests use.
// ---------------------------------------------------------------------------

namespace
{
// The values each node type carries besides its wires, by the names the
// schematic stores them under.
juce::StringArray parameterNamesFor(const juce::String& type)
{
    juce::StringArray names;
    if (type == "literal_i64" || type == "const_bool") names.add("value");
    if (usesTextField(type) || type.startsWith("sm_") || type == "state_machine_instance") names.add("text");
    if (type == "sm_state") names.addArray(juce::StringArray { "accessibility", "initial", "terminal", "entryAction", "updateAction", "exitAction" });
    if (type == "sm_transition") names.addArray(juce::StringArray { "event", "guard", "action" });
    if (type == "sm_event") names.addArray(juce::StringArray { "event", "payloadType" });
    if (type == "state_machine_instance") names.add("machineRef");
    names.addArray(juce::StringArray { "breakpoint", "breakpointEnabled", "watched" });
    return names;
}

juce::var pinsVar(const std::vector<NodeDesignerPanel::Pin>& pins)
{
    juce::Array<juce::var> list;
    for (const auto& p : pins)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", p.name);
        o->setProperty("type", p.type);
        o->setProperty("flow", flowName(p.flow));
        list.add(juce::var(o));
    }
    return list;
}
}

NodeDesignerPanel::~NodeDesignerPanel() = default;

const NodeDesignerPanel::GraphNode* NodeDesignerPanel::findNodeById(const juce::String& id) const
{
    auto it = std::find_if(nodes.begin(), nodes.end(), [&id](const GraphNode& n) { return n.id == id; });
    return it == nodes.end() ? nullptr : &*it;
}

void NodeDesignerPanel::clearGraph()
{
    currentGraphFile = {};
    loadedDocument = juce::var();
    generatedSource.clear();
    generatedFiles.clear();
    currentSourceMap.clear();
    nodes.clear();
    connections.clear();
    variables.clear();
    types.clear();
    functions.clear();
    resources.clear();
    states.clear();
    events.clear();
    transitions.clear();
    nextNodeUid = 1;
    selectedNodeUid = 0;
    selectedConnectionIndex = -1;
    diagramName = "Untitled Node Schematic";
    titleLabel.setText(diagramName, juce::dontSendNotification);
    refreshTemplatesWithFunctions();
}

void NodeDesignerPanel::newGraph(const juce::String& type, bool withStarterNodes)
{
    componentInfo = juce::var();
    setComponentDefinition({});
    const bool stateMachine = type == "state_machine";
    if (withStarterNodes)
    {
        if (stateMachine) initializeStateMachine();
        else initializeNodeGraph();
    }
    else
    {
        clearGraph();
        diagramType = stateMachine ? "state_machine" : "node_graph";
        diagramTypeSelector.setSelectedId(stateMachine ? 2 : 1, juce::dontSendNotification);
        setStatus(stateMachine ? "New empty state machine." : "New empty node graph.");
    }
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    if (variablesPanel != nullptr) variablesPanel->repaint();
    if (typesPanel != nullptr) typesPanel->repaint();
}

void NodeDesignerPanel::openGraph()
{
    const auto folder = defaultFolder != nullptr ? defaultFolder() : juce::File();
    fileChooser = std::make_unique<juce::FileChooser>("Open Node Program", folder, "*.frnode.json;*.json");
    fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (file != juce::File()) loadGraphFile(file);
            fileChooser = nullptr;
        });
}

void NodeDesignerPanel::saveGraph()
{
    if (currentGraphFile == juce::File())
    {
        auto folder = defaultFolder != nullptr ? defaultFolder() : juce::File();
        if (folder != juce::File()) folder.createDirectory();
        const auto base = diagramName.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_- ").trim();
        fileChooser = std::make_unique<juce::FileChooser>("Save Node Program",
            folder.getChildFile((base.isNotEmpty() ? base : juce::String("program")) + ".frnode.json"), "*.frnode.json;*.json");
        fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& chooser) {
                const auto file = chooser.getResult();
                fileChooser = nullptr;
                juce::String error;
                if (file != juce::File() && !saveToFile(file, error))
                    setStatus(error, true);
            });
        return;
    }
    juce::String error;
    if (!saveToFile(currentGraphFile, error))
        setStatus(error, true);
}

bool NodeDesignerPanel::openFile(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "No such node program: " + file.getFullPathName();
        return false;
    }
    juce::String problems;
    if (!loadFromJson(file.loadFileAsString(), problems))
    {
        error = statusText();
        return false;
    }
    // Whose program it is, the caller says (setComponentContext/Definition).
    componentInfo = juce::var();
    setComponentDefinition({});
    currentGraphFile = file;
    titleLabel.setText(file.getFileName(), juce::dontSendNotification);
    setStatus(problems.isEmpty() ? "Opened " + file.getFullPathName() : "Opened " + file.getFileName() + " with problems: " + problems,
              problems.isNotEmpty());
    error = problems;   // empty when it opened cleanly
    return true;
}

bool NodeDesignerPanel::saveToFile(const juce::File& file, juce::String& error)
{
    const auto result = file.getParentDirectory().createDirectory();
    if (result.failed() || !file.replaceWithText(buildSavedDocument()))
    {
        error = "Could not save " + file.getFullPathName();
        return false;
    }
    currentGraphFile = file;
    loadedDocument = juce::JSON::parse(file.loadFileAsString());
    titleLabel.setText(file.getFileName(), juce::dontSendNotification);
    setStatus("Saved " + file.getFullPathName());
    return true;
}

juce::String NodeDesignerPanel::buildSavedDocument() const
{
    return buildSchematicJson();
}

void NodeDesignerPanel::showValidation()
{
    const auto problems = validateGraph();
    if (problems.isEmpty())
        setStatus("No structural problems.");
    else
        setStatus(juce::String(problems.size()) + " problem(s): " + problems.joinIntoString(" "), true);
}

juce::StringArray NodeDesignerPanel::validateGraph() const
{
    juce::StringArray problems;
    std::map<juce::String, int> idCount;
    for (const auto& n : nodes)
        if (++idCount[n.id] == 2)
            problems.add("Node id '" + n.id + "' is used by more than one node.");
    for (const auto& n : nodes)
    {
        if (n.unsupportedSource.isObject())
            problems.add("Node '" + n.id + "' has type '" + n.type + "', which this editor does not define; it is kept as it is.");
        if (n.type.startsWith("call_function:") && findFunction(n.functionRef) == nullptr)
            problems.add("Node '" + n.id + "' calls function '" + n.functionRef + "', which is not defined.");
    }
    std::map<std::pair<int, int>, int> drivers;
    for (const auto& c : connections)
    {
        const auto* a = findNode(c.fromNode);
        const auto* b = findNode(c.toNode);
        if (a == nullptr || b == nullptr)
        {
            problems.add("Connection '" + c.id + "' refers to a node that no longer exists.");
            continue;
        }
        if (c.fromPin < 0 || c.fromPin >= (int)a->outputs.size() || c.toPin < 0 || c.toPin >= (int)b->inputs.size())
        {
            problems.add("Connection '" + c.id + "' refers to a pin that does not exist.");
            continue;
        }
        const auto& from = a->outputs[(size_t)c.fromPin];
        const auto& to = b->inputs[(size_t)c.toPin];
        if (!pinsCompatible(from, to))
            problems.add("Connection '" + c.id + "' joins " + flowName(from.flow) + " " + from.type + " to " + flowName(to.flow) + " " + to.type + ".");
        const bool multiDrive = b->type == "sm_state" && to.flow == PinFlow::Exec;
        if (++drivers[{ c.toNode, c.toPin }] == 2 && !multiDrive)
            problems.add("Input '" + b->id + "." + to.name + "' is driven by more than one connection.");
    }
    if (diagramType == "state_machine")
    {
        if (const auto sm = validateStateMachine(); sm.isNotEmpty())
            problems.add(sm);
    }
    else if (componentInfo.isObject())
    {
        // A component program compiles as a pure function (compute()), whose
        // nodes take every input from a wire: an unwired one fails
        // compilation ("input ref must have 'ref' or 'param'"), so say which.
        for (const auto& n : nodes)
            for (int i = 0; i < (int)n.inputs.size(); ++i)
                if (drivers.count({ n.uid, i }) == 0)
                    problems.add("Input '" + n.id + "." + n.inputs[(size_t)i].name + "' is not wired: every input needs a wire "
                                 "(connect a value to it, or delete the node).");
        // A node whose result reaches no pin, resistance or state variable
        // changes nothing the circuit sees: usually a condition left out of
        // the logic by mistake.
        auto isSink = [](const juce::String& type) {
            return type == "pc_drive" || type == "pc_drive_current" || type == "pc_set_resistance" || type == "pc_state_set";
        };
        std::set<int> reaches;
        for (const auto& n : nodes)
            if (isSink(n.type))
                reaches.insert(n.uid);
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const auto& c : connections)
                if (reaches.count(c.toNode) != 0 && reaches.insert(c.fromNode).second)
                    grew = true;
        }
        for (const auto& n : nodes)
            if (reaches.count(n.uid) == 0)
                problems.add("Node '" + n.id + "' (" + n.type + ") does not reach any pin, resistance or state variable: its result "
                             "is unused (wire it into the logic that drives a pin, or delete it).");
    }
    return problems;
}

juce::String NodeDesignerPanel::addNodeOfType(const juce::String& type, float x, float y, const juce::String& requestedId, juce::String& error)
{
    if (findTemplate(type) == nullptr)
    {
        error = "Unknown node type '" + type + "'. node_program_node_types lists the types.";
        return {};
    }
    if (requestedId.isNotEmpty())
    {
        if (requestedId != requestedId.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"))
        {
            error = "Node ids use letters, digits and underscores only.";
            return {};
        }
        if (findNodeById(requestedId) != nullptr)
        {
            error = "A node with id '" + requestedId + "' already exists.";
            return {};
        }
    }
    auto& node = addNode(type, { x, y });
    if (requestedId.isNotEmpty())
        node.id = requestedId;
    const auto id = node.id;
    if (canvas != nullptr) canvas->repaint();
    return id;
}

bool NodeDesignerPanel::deleteNode(const juce::String& nodeId, juce::String& error)
{
    const auto* n = findNodeById(nodeId);
    if (n == nullptr)
    {
        error = "No node '" + nodeId + "'.";
        return false;
    }
    const auto uid = n->uid;
    removeNode(uid);
    if (selectedNodeUid == uid) selectedNodeUid = 0;
    selectedConnectionIndex = -1;
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    setStatus("Deleted node " + nodeId + ".");
    return true;
}

bool NodeDesignerPanel::moveNode(const juce::String& nodeId, float x, float y, juce::String& error)
{
    const auto* n = findNodeById(nodeId);
    if (n == nullptr)
    {
        error = "No node '" + nodeId + "'.";
        return false;
    }
    auto* node = findNode(n->uid);
    node->x = x;
    node->y = y;
    if (canvas != nullptr) canvas->repaint();
    return true;
}

juce::String NodeDesignerPanel::connect(const juce::String& fromNode, const juce::String& fromPin,
                                        const juce::String& toNode, const juce::String& toPin, juce::String& error)
{
    const auto* a = findNodeById(fromNode);
    const auto* b = findNodeById(toNode);
    if (a == nullptr || b == nullptr)
    {
        error = "No node '" + (a == nullptr ? fromNode : toNode) + "'.";
        return {};
    }
    if (a == b)
    {
        error = "A node cannot be wired to itself.";
        return {};
    }
    auto indexOf = [](const std::vector<Pin>& pins, const juce::String& name) {
        for (int i = 0; i < (int)pins.size(); ++i)
            if (pins[(size_t)i].name == name) return i;
        return -1;
    };
    auto names = [](const std::vector<Pin>& pins) {
        juce::StringArray s;
        for (const auto& p : pins) s.add(p.name);
        return s.joinIntoString(", ");
    };
    const int fp = indexOf(a->outputs, fromPin);
    const int tp = indexOf(b->inputs, toPin);
    if (fp < 0)
    {
        error = indexOf(a->inputs, fromPin) >= 0
            ? "'" + fromNode + "." + fromPin + "' is an input; wires go from an output to an input."
            : "'" + fromNode + "' has no output '" + fromPin + "' (outputs: " + names(a->outputs) + ").";
        return {};
    }
    if (tp < 0)
    {
        error = indexOf(b->outputs, toPin) >= 0
            ? "'" + toNode + "." + toPin + "' is an output; wires go from an output to an input."
            : "'" + toNode + "' has no input '" + toPin + "' (inputs: " + names(b->inputs) + ").";
        return {};
    }
    const auto out = a->outputs[(size_t)fp];
    const auto in = b->inputs[(size_t)tp];
    if (!pinsCompatible(out, in))
    {
        error = "Incompatible pins: " + flowName(out.flow) + " " + out.type + " cannot drive " + flowName(in.flow) + " " + in.type + ".";
        return {};
    }
    const bool stateToState = a->type == "sm_state" && b->type == "sm_state" && out.flow == PinFlow::Exec;
    const bool multiDrive = b->type == "sm_state" && in.flow == PinFlow::Exec;
    if (!multiDrive)
        if (const auto* existing = connectionToInput(b->uid, tp))
        {
            error = "'" + toNode + "." + toPin + "' is already wired by '" + existing->id + "'; disconnect it first.";
            return {};
        }
    const int fromUid = a->uid, toUid = b->uid;
    connectPins(fromUid, fp, toUid, tp);
    if (canvas != nullptr) canvas->repaint();
    if (stateToState)
        if (const auto* t = findNode(selectedNodeUid))
            return t->id;   // the Transition node the editor inserts between two states
    for (const auto& c : connections)
        if (c.fromNode == fromUid && c.fromPin == fp && c.toNode == toUid && c.toPin == tp)
            return c.id;
    error = "The editor did not make the connection.";
    return {};
}

bool NodeDesignerPanel::disconnect(const juce::String& connectionId, juce::String& error)
{
    auto it = std::find_if(connections.begin(), connections.end(), [&connectionId](const Connection& c) { return c.id == connectionId; });
    if (it == connections.end())
    {
        error = "No connection '" + connectionId + "'.";
        return false;
    }
    connections.erase(it);
    selectedConnectionIndex = -1;
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    return true;
}

bool NodeDesignerPanel::setNodeParameter(const juce::String& nodeId, const juce::String& name, const juce::var& value, juce::String& error)
{
    const auto* found = findNodeById(nodeId);
    if (found == nullptr)
    {
        error = "No node '" + nodeId + "'.";
        return false;
    }
    auto* n = findNode(found->uid);
    if (n->unsupportedSource.isObject())
    {
        error = "Node '" + nodeId + "' has an unsupported type and is kept unchanged.";
        return false;
    }
    if (name.startsWith("input."))
    {
        const auto pin = name.fromFirstOccurrenceOf(".", false, false);
        for (int i = 0; i < (int)n->inputs.size(); ++i)
            if (n->inputs[(size_t)i].name == pin)
            {
                if (n->inputs[(size_t)i].flow != PinFlow::Data)
                {
                    error = "Only data inputs have a default value.";
                    return false;
                }
                if (i >= (int)n->inputDefaults.size()) n->inputDefaults.resize((size_t)i + 1);
                n->inputDefaults[(size_t)i] = value.toString();
                refreshProperties();
                return true;
            }
        error = "'" + nodeId + "' has no input '" + pin + "'.";
        return false;
    }
    const auto allowed = parameterNamesFor(n->type);
    if (!allowed.contains(name))
    {
        error = "'" + name + "' is not a parameter of " + n->title + " nodes. Parameters: " + allowed.joinIntoString(", ")
            + ", or input.<pin> for a data input's default value.";
        return false;
    }
    if (name == "value") n->literalValue = n->type == "const_bool" ? ((bool)value ? 1 : 0) : (int)value;
    else if (name == "text") n->textValue = value.toString();
    else if (name == "accessibility") n->accessibility = value.toString();
    else if (name == "initial") n->initial = (bool)value;
    else if (name == "terminal") n->terminal = (bool)value;
    else if (name == "entryAction") n->entryAction = value.toString();
    else if (name == "updateAction") n->updateAction = value.toString();
    else if (name == "exitAction") n->exitAction = value.toString();
    else if (name == "event") n->eventName = value.toString();
    else if (name == "guard") n->guardExpression = value.toString();
    else if (name == "action") n->transitionAction = value.toString();
    else if (name == "payloadType") n->payloadType = value.toString();
    else if (name == "machineRef") n->machineRef = value.toString();
    else if (name == "breakpoint") n->breakpoint = (bool)value;
    else if (name == "breakpointEnabled") n->breakpointEnabled = (bool)value;
    else if (name == "watched") n->watched = (bool)value;
    if (name == "breakpoint" || name == "breakpointEnabled" || name == "watched")
        debugMarkersChanged();
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    return true;
}

juce::var NodeDesignerPanel::describeGraph() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("name", diagramName);
    root->setProperty("diagramType", diagramType);
    root->setProperty("file", currentGraphFile.getFullPathName());
    juce::Array<juce::var> nodeList;
    for (const auto& n : nodes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", n.id);
        o->setProperty("type", n.type);
        o->setProperty("title", n.title);
        o->setProperty("x", n.x);
        o->setProperty("y", n.y);
        if (n.unsupportedSource.isObject())
            o->setProperty("unsupported", true);
        auto* params = new juce::DynamicObject();
        for (const auto& p : parameterNamesFor(n.type))
        {
            if (p == "value") params->setProperty("value", n.type == "const_bool" ? juce::var(n.literalValue != 0) : juce::var(n.literalValue));
            else if (p == "text") params->setProperty("text", n.textValue);
            else if (p == "accessibility") params->setProperty("accessibility", n.accessibility);
            else if (p == "initial") params->setProperty("initial", n.initial);
            else if (p == "terminal") params->setProperty("terminal", n.terminal);
            else if (p == "entryAction") params->setProperty("entryAction", n.entryAction);
            else if (p == "updateAction") params->setProperty("updateAction", n.updateAction);
            else if (p == "exitAction") params->setProperty("exitAction", n.exitAction);
            else if (p == "event") params->setProperty("event", n.eventName);
            else if (p == "guard") params->setProperty("guard", n.guardExpression);
            else if (p == "action") params->setProperty("action", n.transitionAction);
            else if (p == "payloadType") params->setProperty("payloadType", n.payloadType);
            else if (p == "machineRef") params->setProperty("machineRef", n.machineRef);
            else if (p == "breakpoint") params->setProperty("breakpoint", n.breakpoint);
            else if (p == "breakpointEnabled") params->setProperty("breakpointEnabled", n.breakpointEnabled);
            else if (p == "watched") params->setProperty("watched", n.watched);
        }
        for (int i = 0; i < (int)n.inputs.size() && i < (int)n.inputDefaults.size(); ++i)
            if (n.inputs[(size_t)i].flow == PinFlow::Data && n.inputDefaults[(size_t)i].isNotEmpty() && connectionToInput(n.uid, i) == nullptr)
                params->setProperty("input." + n.inputs[(size_t)i].name, n.inputDefaults[(size_t)i]);
        o->setProperty("parameters", juce::var(params));
        o->setProperty("inputs", pinsVar(n.inputs));
        o->setProperty("outputs", pinsVar(n.outputs));
        nodeList.add(juce::var(o));
    }
    root->setProperty("nodes", nodeList);
    juce::Array<juce::var> wireList;
    for (const auto& c : connections)
    {
        const auto* a = findNode(c.fromNode);
        const auto* b = findNode(c.toNode);
        if (a == nullptr || b == nullptr) continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("id", c.id);
        o->setProperty("from", a->id);
        o->setProperty("fromPin", c.fromPin < (int)a->outputs.size() ? a->outputs[(size_t)c.fromPin].name : juce::String());
        o->setProperty("to", b->id);
        o->setProperty("toPin", c.toPin < (int)b->inputs.size() ? b->inputs[(size_t)c.toPin].name : juce::String());
        wireList.add(juce::var(o));
    }
    root->setProperty("connections", wireList);
    return juce::var(root);
}

juce::var NodeDesignerPanel::describeNodeTypes() const
{
    juce::Array<juce::var> list;
    for (const auto& t : templates)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("type", t.type);
        o->setProperty("title", t.title);
        o->setProperty("category", t.category);
        o->setProperty("inputs", pinsVar(t.inputs));
        o->setProperty("outputs", pinsVar(t.outputs));
        juce::Array<juce::var> params;
        for (const auto& p : parameterNamesFor(t.type)) params.add(p);
        o->setProperty("parameters", params);
        list.add(juce::var(o));
    }
    return list;
}

// ---------------------------------------------------------------------------
// Compile, Export and Compile & Run: the reference's compile path, with the
// node compiler copied into the Workbench (NodeCompiler.cpp). The IDE's
// debugger hook-up (breakpoints at generated lines) is not part of the
// Workbench; Compile & Run hands the program to the Workbench's Frust panel
// instead of the IDE REPL.
// ---------------------------------------------------------------------------

void NodeDesignerPanel::compileGraph()
{
    juce::String message;
    compileProgram(message);
}

bool NodeDesignerPanel::compileProgram(juce::String& message)
{
    generatedSource.clear();
    generatedFiles.clear();
    currentSourceMap.clear();
    auto fail = [this, &message](const juce::String& text) {
        message = text;
        // The status line is one line; the full text (with the generated
        // source the compiler rejected) is returned to the caller.
        setStatus(text.upToFirstOccurrenceOf("\n", false, false), true);
        return false;
    };

    if (diagramType == "state_machine")
    {
        const auto validation = validateStateMachine();
        if (validation.isNotEmpty())
            return fail("State-machine validation failed: " + validation);
    }
    else if (selectedTarget() != "frust")
        return fail("GLSL is a production reference target. This research canvas currently compiles Frust.");
    else
    {
        juce::StringArray unwired;
        for (const auto& p : validateGraph())
            if (p.contains("is not wired"))
                unwired.add(p);
        if (!unwired.isEmpty())
            return fail("Compilation failed: " + unwired.joinIntoString(" "));
    }

    const auto result = node_compiler::CompileSchematic(buildSchematicJson(false).toStdString());
    if (!result.ok)
        return fail("Compilation failed: " + juce::String(result.errorMessage));

    generatedSource = juce::String(result.source);
    generatedFiles = result.files;
    generatedKind = result.artifactKind;
    currentSourceMap = result.sourceMap;

    if (diagramType == "state_machine")
        message = "Compiled state-machine schematic to Frust. JSON remains authoritative; generated files are build artifacts.";
    else
    {
        juce::String artifact = "function source";
        if (result.artifactKind == node_compiler::ArtifactKind::FrustExecutablePod)
            artifact = "Frust executable pod";
        else if (result.artifactKind == node_compiler::ArtifactKind::FrustLibraryPod)
            artifact = "Frust library pod";
        message = "Compiled node schematic as " + artifact + ". JSON remains authoritative; generated files are build artifacts.";
    }
    setStatus(message);
    return true;
}

juce::String NodeDesignerPanel::generatedSourcePathLabel() const
{
    const auto base = currentGraphFile.existsAsFile()
        ? currentGraphFile.getFileNameWithoutExtension()
        : diagramName.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-").toLowerCase();
    const auto safe = base.isNotEmpty() ? base : juce::String("node_schematic");
    return ".frust/generated/nodes/" + safe + "/" + (selectedFrustProjectType() == "lib" ? "lib.fr" : "main.fr");
}

juce::File NodeDesignerPanel::generatedSourceCacheFile() const
{
    // Next to the program; a program not saved yet exports into the
    // project's programs folder (the reference used the working directory).
    juce::File root;
    if (currentGraphFile.existsAsFile())
        root = currentGraphFile.getParentDirectory();
    else if (defaultFolder != nullptr)
        root = defaultFolder();
    if (root == juce::File())
        root = juce::File::getCurrentWorkingDirectory();
    return root.getChildFile(generatedSourcePathLabel());
}

void NodeDesignerPanel::saveGeneratedSource()
{
    juce::File packageRoot;
    juce::String message;
    exportProgram(packageRoot, message);
}

bool NodeDesignerPanel::exportProgram(juce::File& packageRoot, juce::String& message)
{
    if (generatedSource.trim().isEmpty() && generatedFiles.empty())
    {
        message = "Compile the schematic before exporting generated Frust source.";
        setStatus(message, true);
        return false;
    }

    if (!generatedFiles.empty())
    {
        packageRoot = generatedSourceCacheFile().getParentDirectory();
        for (const auto& generated : generatedFiles)
        {
            auto file = packageRoot.getChildFile(juce::String(generated.path));
            file.getParentDirectory().createDirectory();
            if (!file.replaceWithText(juce::String(generated.content)))
            {
                message = "Could not save " + file.getFullPathName();
                setStatus(message, true);
                return false;
            }
        }
        message = "Exported generated artifact package to " + packageRoot.getFullPathName();
        setStatus(message);
        return true;
    }

    const auto sourceFile = generatedSourceCacheFile();
    sourceFile.getParentDirectory().createDirectory();
    packageRoot = sourceFile.getParentDirectory();
    if (!sourceFile.replaceWithText(generatedSource))
    {
        message = "Could not save " + sourceFile.getFullPathName();
        setStatus(message, true);
        return false;
    }
    message = "Exported generated source artifact to " + sourceFile.getFullPathName();
    setStatus(message);
    return true;
}

bool NodeDesignerPanel::buildRunScript(juce::String& script, juce::String& error) const
{
    if (generatedSource.trim().isEmpty())
    {
        error = "Compile the schematic before running it.";
        return false;
    }
    return wrapForRun(generatedSource, script, error);
}

bool NodeDesignerPanel::wrapForRun(const juce::String& src, juce::String& script, juce::String& error)
{
    // The reference sent the program to the IDE REPL (adding `compute();`
    // for function source). A Workbench script is a unit with
    // `pub fn run() -> String`, so the program gets that entry point after
    // its own lines: executable pods run main(); function source returns
    // compute()'s value; a state-machine library returns its initial
    // state's name.
    if (src.contains("fn main() -> i64"))
    {
        script = src + "\npub fn run() -> String = {\n    main();\n    \"\"\n}\n";
        return true;
    }
    if (src.contains("pub fn initial_state() -> i64"))
    {
        script = src + "\npub fn run() -> String = {\n    state_name(initial_state())\n}\n";
        return true;
    }
    const auto signatureStart = src.indexOf("pub fn compute(");
    if (signatureStart < 0)
    {
        error = "The generated program has no entry point to run (main, compute or a state machine).";
        return false;
    }
    const auto params = src.substring(signatureStart + 15).upToFirstOccurrenceOf(")", false, false).trim();
    if (params.isNotEmpty())
    {
        error = "Compile & Run calls compute() without arguments, and this program takes inputs (" + params
            + "). Run it from another program, or replace the inputs with literals to try it.";
        return false;
    }
    const auto returnType = src.substring(signatureStart).fromFirstOccurrenceOf("->", false, false)
                               .upToFirstOccurrenceOf("=", false, false).trim();
    juce::String externs, value;
    auto needExtern = [&src, &externs](const juce::String& decl, const juce::String& name) {
        if (!src.contains("extern fn " + name + "("))
            externs << decl << "\n";
    };
    if (returnType == "String")
        value = "compute()";
    else if (returnType == "i64" || returnType == "f64" || returnType == "bool")
    {
        needExtern("extern fn frust_format_" + returnType + "(val: " + returnType + ") -> String;", "frust_format_" + returnType);
        value = "frust_format_" + returnType + "(compute())";
    }
    else
    {
        error = "compute() returns " + returnType + ", which Compile & Run cannot show.";
        return false;
    }
    script = src + "\n" + externs + "pub fn run() -> String = {\n    " + value + "\n}\n";
    return true;
}

void NodeDesignerPanel::compileAndRun()
{
    juce::String message;
    if (!compileProgram(message))
        return;
    juce::String script, error;
    if (!buildRunScript(script, error))
    {
        setStatus(error, true);
        return;
    }
    if (onRunRequested != nullptr)
        onRunRequested(script, currentGraphFile.existsAsFile() ? currentGraphFile.getFileName() : diagramName);
    else
        setStatus("Compiled. Nothing is connected to run the program here.", true);
}

// ---------------------------------------------------------------------------
// Debugging: markers, the debug build, and the execution marker.
// ---------------------------------------------------------------------------

void NodeDesignerPanel::debugMarkersChanged()
{
    if (onDebugMarkersChanged != nullptr)
        onDebugMarkersChanged();
}

std::vector<NodeDesignerPanel::BreakpointMarker> NodeDesignerPanel::breakpointMarkers() const
{
    std::vector<BreakpointMarker> list;
    for (const auto& n : nodes)
        if (n.breakpoint)
            list.push_back({ n.id, n.breakpointEnabled });
    return list;
}

juce::StringArray NodeDesignerPanel::watchedNodes() const
{
    juce::StringArray list;
    for (const auto& n : nodes)
        if (n.watched)
            list.add(n.id);
    return list;
}

bool NodeDesignerPanel::setBreakpoint(const juce::String& nodeId, bool present, bool enabled, juce::String& error)
{
    auto* found = findNodeById(nodeId);
    if (found == nullptr)
    {
        error = "No node '" + nodeId + "'.";
        return false;
    }
    auto* n = findNode(found->uid);
    n->breakpoint = present;
    n->breakpointEnabled = enabled;
    debugMarkersChanged();
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    return true;
}

bool NodeDesignerPanel::setWatch(const juce::String& nodeId, bool watched, juce::String& error)
{
    auto* found = findNodeById(nodeId);
    if (found == nullptr)
    {
        error = "No node '" + nodeId + "'.";
        return false;
    }
    findNode(found->uid)->watched = watched;
    debugMarkersChanged();
    refreshProperties();
    if (canvas != nullptr) canvas->repaint();
    return true;
}

juce::String NodeDesignerPanel::programId() const
{
    return currentGraphFile != juce::File() ? currentGraphFile.getFullPathName() : "unsaved:" + diagramName;
}

juce::String NodeDesignerPanel::programLabel() const
{
    return currentGraphFile.existsAsFile() ? currentGraphFile.getFileName() : diagramName;
}

juce::String NodeDesignerPanel::panelNodeIdFor(const std::string& compilerId) const
{
    // The node compiler names a node by its id, and a state machine's event
    // by its event name.
    const auto id = juce::String(compilerId);
    if (findNodeById(id) != nullptr)
        return id;
    for (const auto& n : nodes)
        if (n.type == "sm_event" && n.eventName == id)
            return n.id;
    return {};
}

std::map<juce::String, int> NodeDesignerPanel::nodeLines(juce::String& error)
{
    std::map<juce::String, int> lines;
    if (diagramType != "state_machine" && selectedTarget() != "frust")
    {
        error = "Only the FRust target can be debugged.";
        return lines;
    }
    const auto result = node_compiler::CompileSchematic(buildSchematicJson(false).toStdString());
    if (!result.ok)
    {
        error = "Compilation failed: " + juce::String(result.errorMessage);
        return lines;
    }
    for (const auto& e : result.sourceMap)
    {
        const auto id = panelNodeIdFor(e.nodeId);
        if (id.isNotEmpty() && lines.count(id) == 0)
            lines[id] = e.line;
    }
    return lines;
}

bool NodeDesignerPanel::buildDebugProgram(frust_exec::Program& program, juce::String& error)
{
    if (diagramType == "state_machine")
    {
        const auto validation = validateStateMachine();
        if (validation.isNotEmpty())
        {
            error = "State-machine validation failed: " + validation;
            return false;
        }
    }
    else if (selectedTarget() != "frust")
    {
        error = "Only the FRust target can be debugged.";
        return false;
    }
    node_compiler::CompileOptions options;
    options.debugInstrumentation = true;
    const auto result = node_compiler::CompileSchematic(buildSchematicJson(false).toStdString(), options);
    if (!result.ok)
    {
        error = "Compilation failed: " + juce::String(result.errorMessage);
        return false;
    }
    juce::String script;
    if (!wrapForRun(juce::String(result.source), script, error))
        return false;

    program = {};
    program.label = programLabel().toStdString();
    program.programId = programId().toStdString();
    program.script = script.toStdString();
    program.programLines = (int)std::count(result.source.begin(), result.source.end(), '\n');
    program.debug = true;
    program.prelude = node_compiler::DebugPrelude();
    program.info = result.debug;
    // The compiler names nodes as the schematic does, except events (by
    // event name); everything the debugger reports uses the editor's ids.
    for (auto& slot : program.info.slots)
        for (auto& id : slot.nodeIds)
        {
            const auto panelId = panelNodeIdFor(id);
            if (panelId.isNotEmpty()) id = panelId.toStdString();
        }
    for (auto& fn : program.info.functions)
        if (!fn.nodeId.empty())
        {
            const auto panelId = panelNodeIdFor(fn.nodeId);
            if (panelId.isNotEmpty()) fn.nodeId = panelId.toStdString();
        }
    std::map<juce::String, int> lines;
    for (const auto& e : result.sourceMap)
    {
        const auto id = panelNodeIdFor(e.nodeId);
        if (id.isEmpty())
            continue;
        if (lines.count(id) == 0)
            lines[id] = e.line;
        program.lineNodes.emplace(e.line, id.toStdString());
    }
    for (const auto& marker : breakpointMarkers())
        program.breakpoints.push_back({ marker.nodeId.toStdString(), lines.count(marker.nodeId) != 0 ? lines[marker.nodeId] : 0,
                                        marker.enabled, 0 });
    for (const auto& id : watchedNodes())
        program.watches.push_back(id.toStdString());
    return true;
}

void NodeDesignerPanel::setExecutionMarker(const juce::String& nodeId)
{
    if (executionNodeId == nodeId)
        return;
    executionNodeId = nodeId;
    if (canvas != nullptr)
        canvas->repaint();
}

void NodeDesignerPanel::setCurrentFile(const juce::File& file)
{
    currentGraphFile = file;
    titleLabel.setText(file.getFileName(), juce::dontSendNotification);
}

void NodeDesignerPanel::setComponentDefinition(const juce::var& definition)
{
    editedDefinition = definition.isObject() ? juce::JSON::parse(juce::JSON::toString(definition)) : juce::var();
    const bool show = editedDefinition.isObject();
    if (componentPanel != nullptr)
    {
        componentPanel->setVisible(show);
        componentPanel->rebuild();
    }
    resized();
}

bool NodeDesignerPanel::editComponent(const juce::var& edited, juce::String& message)
{
    if (!editedDefinition.isObject())
    {
        message = "No programmable component's program is open.";
        return false;
    }
    // What the edit renames, so the nodes that name it can follow: pins by
    // their uid; parameters and state variables by position (the panel edits
    // them in place) when the old name is gone.
    struct Rename { juce::StringArray types; juce::String from, to; };
    std::vector<Rename> renames;
    const juce::StringArray pinTypes { "pc_pin_voltage", "pc_pin_current", "pc_drive", "pc_drive_current", "pc_set_resistance" };
    if (const auto* before = editedDefinition.getProperty("pins", {}).getArray())
        if (const auto* after = edited.getProperty("pins", {}).getArray())
            for (const auto& p : *before)
                for (const auto& q : *after)
                    if (p.getProperty("uid", {}).toString().isNotEmpty() && p.getProperty("uid", {}) == q.getProperty("uid", {})
                        && p.getProperty("id", {}).toString() != q.getProperty("id", {}).toString())
                        renames.push_back({ pinTypes, p.getProperty("id", {}).toString(), q.getProperty("id", {}).toString() });
    auto byPosition = [&](const char* key, const juce::StringArray& types) {
        const auto* before = editedDefinition.getProperty(key, {}).getArray();
        const auto* after = edited.getProperty(key, {}).getArray();
        if (before == nullptr || after == nullptr || before->size() != after->size())
            return;
        juce::StringArray newNames;
        for (const auto& q : *after) newNames.add(q.getProperty("name", {}).toString());
        for (int k = 0; k < before->size(); ++k)
        {
            const auto from = (*before)[k].getProperty("name", {}).toString();
            const auto to = (*after)[k].getProperty("name", {}).toString();
            if (from != to && !newNames.contains(from))
                renames.push_back({ types, from, to });
        }
    };
    byPosition("parameters", { "pc_parameter" });
    byPosition("state", { "pc_state_get", "pc_state_set" });

    if (onComponentEdited == nullptr)
    {
        message = "Component definitions cannot be edited here.";
        setStatus(message, true);
        return false;
    }
    if (!onComponentEdited(edited, message))
    {
        setStatus(message, true);
        return false;
    }
    int followed = 0;
    for (auto& node : nodes)
        for (const auto& r : renames)
            if (r.types.contains(node.type) && node.textValue == r.from)
            {
                node.textValue = r.to;
                ++followed;
            }
    if (currentGraphFile != juce::File())
    {
        juce::String saveError;
        if (!saveToFile(currentGraphFile, saveError))
            message << (message.isNotEmpty() ? "\n" : "") << "The program could not be saved: " << saveError;
    }
    if (followed > 0)
        message << (message.isNotEmpty() ? " " : "") << juce::String(followed) << (followed == 1 ? " node follows" : " nodes follow") << " the new name.";
    setStatus(message.isNotEmpty() ? message : juce::String("Component updated."), false);
    refreshProperties();
    if (canvas != nullptr)
        canvas->repaint();
    return true;
}

void NodeDesignerPanel::setComponentContext(const juce::var& context)
{
    componentInfo = context;
    if (diagramType != "state_machine" && context.isObject())
    {
        // A component's program is a function graph (compute()).
        frustProjectType = "lib";
        frustProjectTypeSelector.setSelectedId(2, juce::dontSendNotification);
    }
    refreshProperties();
}
