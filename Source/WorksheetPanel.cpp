#include "WorksheetPanel.h"
#include "MathLinear.h"
#include "MathView.h"
#include <set>

namespace
{
constexpr auto starter = "pub fn run() -> String = {\n    let value: f64 = 6.0 * 7.0;\n    math_set_scalar(\"answer\", value);\n    print_line(\"Computed answer; inspect the Variables panel.\");\n    \"Completed\"\n}\n";
constexpr auto starterMath = "f(x) = sqrt(x^2+1)/(x+3)";
constexpr int maxSource = 100000;
const juce::Colour accent(0xff78dcca);

void style(juce::TextEditor& editor)
{
    editor.setMultiLine(true); editor.setReturnKeyStartsNewLine(true);
    editor.setFont(juce::Font("Consolas", 14.0f, juce::Font::plain));
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151c26));
    editor.setColour(juce::TextEditor::textColourId, juce::Colour(0xffe2e8f0));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
}
const std::initializer_list<math_doc::Role> allRoles { math_doc::Role::Expression, math_doc::Role::Definition, math_doc::Role::Equation,
                                                        math_doc::Role::Constraint, math_doc::Role::Assumption, math_doc::Role::System };
juce::String roleLabel(math_doc::Role role)
{
    switch (role)
    {
        case math_doc::Role::Expression: return "Expression";
        case math_doc::Role::Definition: return "Definition";
        case math_doc::Role::Equation: return "Equation";
        case math_doc::Role::Constraint: return "Constraint";
        case math_doc::Role::Assumption: return "Assumption";
        case math_doc::Role::System: return "System of equations";
    }
    return {};
}
}

// ---- the document ------------------------------------------------------------------------------------------------------------
void WorksheetDocument::reset()
{
    title = "Engineering worksheet"; cells.clear(); modified = false; nodeIds = {};
    cells.push_back({juce::Uuid().toString(), "text", juce::String::fromUTF8("Djehuti Math\n\nWrite engineering notes, typeset mathematics and run FRust calculations in the same worksheet. The example stores 6 × 7 in the shared scalar workspace. Use FRust for the full editor and Variables to inspect its result."), {}, {}});
    std::vector<std::string> notes;
    newMathCell(math_doc::Role::Definition, starterMath, notes);
    cells.push_back({juce::Uuid().toString(), "frust", starter, {}, {}});
    modified = false;
}
juce::var WorksheetDocument::toJson() const
{
    auto* root = new juce::DynamicObject(); root->setProperty("format", "djehuti-math-worksheet"); root->setProperty("version", 2); root->setProperty("title", title);
    root->setProperty("nextNodeId", (juce::int64) nodeIds.next);
    juce::Array<juce::var> values;
    for (const auto& cell : cells)
    {
        auto* item = new juce::DynamicObject(); item->setProperty("id", cell.id); item->setProperty("kind", cell.kind);
        if (cell.kind == "math")
        {
            item->setProperty("math", math_doc::toVar(cell.math));
            auto* origin = new juce::DynamicObject(); origin->setProperty("format", "linear"); origin->setProperty("text", cell.source);
            item->setProperty("origin", juce::var(origin));
        }
        else
            item->setProperty("source", cell.source);
        values.add(juce::var(item));
    }
    root->setProperty("cells", values); return juce::var(root);
}
int WorksheetDocument::indexOf(const juce::String& id) const { for (size_t i = 0; i < cells.size(); ++i) if (cells[i].id == id) return (int)i; return -1; }
bool WorksheetDocument::load(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile() || file.getSize() > 8 * 1024 * 1024) { error = "Worksheet is missing or exceeds the 8 MB foundation limit."; return false; }
    const auto parsed = juce::JSON::parse(file.loadFileAsString());
    const auto* array = parsed.getProperty("cells", {}).getArray();
    const int version = (int)parsed.getProperty("version", 0);
    if (parsed.getProperty("format", "") != juce::var("djehuti-math-worksheet") || (version != 1 && version != 2) || array == nullptr || array->size() > 200)
    { error = "Unsupported worksheet format or version."; return false; }
    std::vector<WorksheetCell> candidate; std::set<juce::String> ids;
    math_doc::IdAllocator allocator; std::set<math_doc::NodeId> seenNodes;
    for (const auto& value : *array)
    {
        WorksheetCell cell {value["id"].toString(), value["kind"].toString(), value["source"].toString(), {}, {}};
        const bool math = cell.kind == "math" && version == 2;
        if (cell.id.isEmpty() || !ids.insert(cell.id).second || (cell.kind != "text" && cell.kind != "frust" && !math))
        { error = "Invalid or duplicate worksheet cell."; return false; }
        if (math)
        {
            cell.source = value["origin"]["text"].toString();
            juce::String why;
            if (!math_doc::fromVar(value["math"], allocator, seenNodes, cell.math, why)) { error = "Invalid math cell: " + why; return false; }
            if (cell.source.isEmpty()) cell.source = juce::String::fromUTF8(math_linear::toLinear(cell.math.lines).c_str());
        }
        if (cell.source.length() > maxSource) { error = "Invalid or duplicate worksheet cell."; return false; }
        candidate.push_back(std::move(cell));
    }
    const auto stored = (juce::int64)parsed.getProperty("nextNodeId", 0);
    if (stored > 0) allocator.next = std::max<math_doc::NodeId>(allocator.next, (math_doc::NodeId) stored);
    cells = std::move(candidate); nodeIds = allocator; title = parsed.getProperty("title", "Worksheet").toString(); modified = false; return true;
}
bool WorksheetDocument::save(const juce::File& file, juce::String& error)
{
    juce::TemporaryFile temporary(file);
    if (!temporary.getFile().replaceWithText(juce::JSON::toString(toJson(), false)) || !temporary.overwriteTargetFileWithTemporary()) { error = "Could not save worksheet: " + file.getFullPathName(); return false; }
    modified = false; return true;
}

juce::String WorksheetDocument::newMathCell(math_doc::Role role, const juce::String& linear, std::vector<std::string>& notes)
{
    if (cells.size() >= 200 || linear.length() > maxSource) return {};
    WorksheetCell cell { juce::Uuid().toString(), "math", linear, {}, {} };
    cell.math.role = role;
    cells.push_back(std::move(cell));
    const auto id = cells.back().id;
    if (!setMathSource(id, linear, notes)) { cells.pop_back(); return {}; }
    cells.back().math.revision = 1;
    modified = true;
    return id;
}

bool WorksheetDocument::setMathSource(const juce::String& id, const juce::String& linear, std::vector<std::string>& notes)
{
    const int index = indexOf(id);
    if (index < 0 || cells[(size_t) index].kind != "math" || linear.length() > maxSource) return false;
    auto& cell = cells[(size_t) index];
    auto allocator = nodeIds;                                   // committed only if the whole input is representable
    auto lines = math_linear::parseCell(linear, cell.math.role, allocator, notes);
    int total = 0;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i < cell.math.lines.size()) lines[i] = math_doc::adoptIds(cell.math.lines[i], lines[i]);
        juce::String why;
        if (!math_doc::validate(lines[i], why)) { notes.push_back("Not applied: " + why.toStdString()); return false; }
        total += math_doc::countNodes(lines[i]);
    }
    if (total > math_doc::kMaxNodesPerCell) { notes.push_back("Not applied: the expression has too many parts."); return false; }
    nodeIds = allocator;
    cell.math.lines = std::move(lines);
    cell.source = linear;
    ++cell.math.revision;
    modified = true;
    return true;
}

bool WorksheetDocument::setMathRole(const juce::String& id, math_doc::Role role, std::vector<std::string>& notes)
{
    const int index = indexOf(id);
    if (index < 0 || cells[(size_t) index].kind != "math") return false;
    auto& cell = cells[(size_t) index];
    if (cell.math.role == role) return true;
    const auto before = cell.math.role;
    cell.math.role = role;
    // moving into or out of a System changes how the text is divided into lines
    if (!setMathSource(id, cell.source, notes)) { cell.math.role = before; return false; }
    return true;
}

bool WorksheetDocument::snapshot(const juce::String& id, math_doc::MathSnapshot& out) const
{
    const int index = indexOf(id);
    if (index < 0 || cells[(size_t) index].kind != "math") return false;
    const auto& cell = cells[(size_t) index];
    out.cellId = id.toStdString(); out.cellRevision = cell.math.revision; out.role = cell.math.role; out.lines = cell.math.lines;   // nodes are immutable
    return true;
}

// ---- one cell on screen ------------------------------------------------------------------------------------------------------
class WorksheetPanel::CellComponent final : public juce::Component
{
public:
    CellComponent(WorksheetPanel& ownerIn, const WorksheetCell& cell, int ordinalIn)
        : owner(ownerIn), id(cell.id), kind(cell.kind), isCode(cell.kind == "frust"), isMath(cell.kind == "math"), ordinal(ordinalIn)
    {
        title.setColour(juce::Label::textColourId, juce::Colour(0xff95a8bd)); addAndMakeVisible(title);
        style(source); addAndMakeVisible(source);
        if (isMath)
        {
            source.setMultiLine(false);
            source.setTextToShowWhenEmpty("Type an expression, e.g.  f(x) = sqrt(x^2+1)/(x+3)", juce::Colour(0xff64748b));
            source.onTextChange = [this] { owner.mathTextChanged(id, source.getText()); };
            for (auto r : allRoles) role.addItem(roleLabel(r), (int) r + 1);
            role.onChange = [this] { owner.setMathRole(id, (math_doc::Role) (role.getSelectedId() - 1)); };
            addAndMakeVisible(role);
            notes.setColour(juce::Label::textColourId, juce::Colour(0xffe0b070)); notes.setFont(juce::Font(12.5f)); addAndMakeVisible(notes);
            info.setColour(juce::Label::textColourId, juce::Colour(0xff7f93a8)); info.setFont(juce::Font("Consolas", 12.0f, juce::Font::plain)); info.setJustificationType(juce::Justification::centredRight); addAndMakeVisible(info);
            view.onHover = [this](const juce::String& text) { info.setText(text, juce::dontSendNotification); };
            addAndMakeVisible(view);
        }
        else
        {
            source.onTextChange = [this] { const int i = owner.document->indexOf(id); if (i >= 0) { owner.document->cells[(size_t)i].source = source.getText(); owner.document->modified = true; if (owner.onChanged) owner.onChanged(); } };
            style(output); output.setReadOnly(true); addAndMakeVisible(output); output.setVisible(isCode);
            run.setButtonText("Run cell"); run.onClick = [this] { juce::String error; if (!owner.runCell(id, error)) output.setText(error, false); }; if (isCode) addAndMakeVisible(run);
        }
        refresh(cell, ordinal);
    }

    void refresh(const WorksheetCell& cell, int ordinalIn)
    {
        ordinal = ordinalIn;
        title.setText(juce::String(ordinal) + (isMath ? "   Math" : isCode ? "   FRust calculation" : "   Notes"), juce::dontSendNotification);
        if (source.getText() != cell.source) source.setText(cell.source, false);
        if (isMath)
        {
            if (shownRevision != cell.math.revision || shownRole != cell.math.role)
            {
                view.setContent(cell.math.lines, cell.math.role);
                shownRevision = cell.math.revision; shownRole = cell.math.role;
                source.setMultiLine(cell.math.role == math_doc::Role::System, false);
                source.setReturnKeyStartsNewLine(cell.math.role == math_doc::Role::System);
                if (cell.math.role == math_doc::Role::System) source.setTextToShowWhenEmpty("One equation per line; mark the alignment point with &", juce::Colour(0xff64748b));
            }
            role.setSelectedId((int) cell.math.role + 1, juce::dontSendNotification);
        }
        else if (output.getText() != cell.output)
            output.setText(cell.output, false);
    }

    void setMathNotes(const juce::StringArray& list)
    {
        notesList = list;
        notes.setText(list.joinIntoString("  ·  "), juce::dontSendNotification);
    }

    int preferredHeight() const
    {
        if (!isMath) return isCode ? 300 : 165;
        const bool system = shownRole == math_doc::Role::System;
        const int input = system ? juce::jlimit(60, 200, 22 + 20 * std::max(2, source.getText().isEmpty() ? 2 : juce::StringArray::fromLines(source.getText()).size())) : 34;
        return 14 + 30 + 6 + input + 6 + std::max(64, view.preferredHeight()) + (notesList.isEmpty() ? 0 : 22) + 14;
    }

    void setOutput(const juce::String& text) { output.setText(text, false); }
    void paint(juce::Graphics& g) override
    {
        g.setColour(juce::Colour(0xff1b2430)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
        if (owner.selectedId == id) { g.setColour(accent.withAlpha(0.85f)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 8.0f, 1.6f); }
    }
    void mouseDown(const juce::MouseEvent&) override { owner.selectCell(id); }
    void resized() override
    {
        auto a = getLocalBounds().reduced(14);
        auto bar = a.removeFromTop(30);
        if (isMath)
        {
            role.setBounds(bar.removeFromRight(180).reduced(0, 2));
            info.setBounds(bar.removeFromRight(260));
            title.setBounds(bar);
            a.removeFromTop(6);
            const bool system = shownRole == math_doc::Role::System;
            source.setBounds(a.removeFromTop(system ? juce::jlimit(60, 200, 22 + 20 * std::max(2, juce::StringArray::fromLines(source.getText()).size())) : 34));
            a.removeFromTop(6);
            if (!notesList.isEmpty()) notes.setBounds(a.removeFromBottom(22)); else notes.setBounds({});
            view.setBounds(a);
            return;
        }
        run.setBounds(bar.removeFromRight(88)); title.setBounds(bar); a.removeFromTop(6);
        if (isCode) { output.setBounds(a.removeFromBottom(60)); a.removeFromBottom(8); }
        source.setBounds(a);
    }

    WorksheetPanel& owner; juce::String id, kind; bool isCode, isMath; int ordinal;
    juce::Label title, notes, info; juce::TextEditor source, output; juce::TextButton run;
    juce::ComboBox role; MathView view;
    juce::StringArray notesList;
    std::uint64_t shownRevision = 0; math_doc::Role shownRole = math_doc::Role::Expression;
};

// ---- the panel -------------------------------------------------------------------------------------------------------------------
WorksheetPanel::WorksheetPanel(std::shared_ptr<WorksheetDocument> value) : document(std::move(value))
{
    heading.setFont(juce::Font(24.0f, juce::Font::bold)); heading.setColour(juce::Label::textColourId, juce::Colour(0xffe2e8f0)); addAndMakeVisible(heading);
    subtitle.setText(juce::String::fromUTF8("Notes · Typeset mathematics · FRust calculations · Inspectable results"), juce::dontSendNotification); subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff95a8bd)); addAndMakeVisible(subtitle);
    addCode.onClick = [this] { addCell("frust", starter); }; addNote.onClick = [this] { addCell("text", "Engineering notes"); };
    addMath.onClick = [this] { addMathCell(math_doc::Role::Expression, {}); };
    stop.onClick = [] { std::string ignored; frust_exec::Executor::instance().stop(ignored); };
    addAndMakeVisible(addMath); addAndMakeVisible(addCode); addAndMakeVisible(addNote); addAndMakeVisible(stop);
    viewport.setViewedComponent(&canvas, false); viewport.setScrollBarsShown(true, false); addAndMakeVisible(viewport);
    listener = frust_exec::Executor::instance().addListener([this](const frust_exec::Snapshot&) { executionChanged(); });
    juce::Desktop::getInstance().addFocusChangeListener(this);
    rebuild();
}
WorksheetPanel::~WorksheetPanel()
{
    juce::Desktop::getInstance().removeFocusChangeListener(this);
    frust_exec::Executor::instance().removeListener(listener); viewport.setViewedComponent(nullptr, false);
}
void WorksheetPanel::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff101720)); }
void WorksheetPanel::resized()
{
    auto a = getLocalBounds().reduced(22); auto bar = a.removeFromTop(34); stop.setBounds(bar.removeFromRight(64)); bar.removeFromRight(8); addNote.setBounds(bar.removeFromRight(88)); bar.removeFromRight(8); addCode.setBounds(bar.removeFromRight(112)); bar.removeFromRight(8); addMath.setBounds(bar.removeFromRight(96)); heading.setBounds(bar); subtitle.setBounds(a.removeFromTop(27)); a.removeFromTop(12); viewport.setBounds(a);
    layoutCells();
}
void WorksheetPanel::layoutCells()
{
    int y = 0; const int width = juce::jmax(200, viewport.getWidth() - 16);
    for (auto& cell : components) { const int height = cell->preferredHeight(); cell->setBounds(0, y, width, height); y += height + 18; }
    canvas.setSize(width, juce::jmax(y, viewport.getHeight()));
}
WorksheetPanel::CellComponent* WorksheetPanel::find(const juce::String& id) const
{
    for (auto& cell : components) if (cell->id == id) return cell.get();
    return nullptr;
}
const void* WorksheetPanel::componentFor(const juce::String& id) const { return find(id); }
int WorksheetPanel::cellHeight(const juce::String& id) const { if (auto* cell = find(id)) return cell->getHeight(); return 0; }
juce::StringArray WorksheetPanel::notesFor(const juce::String& id) const { if (auto* cell = find(id)) return cell->notesList; return {}; }

void WorksheetPanel::rebuild()
{
    heading.setText(document->title, juce::dontSendNotification);
    std::vector<std::unique_ptr<CellComponent>> next;
    for (size_t i = 0; i < document->cells.size(); ++i)
    {
        const auto& cell = document->cells[i];
        std::unique_ptr<CellComponent> keep;
        for (auto& existing : components)
            if (existing != nullptr && existing->id == cell.id && existing->kind == cell.kind) { keep = std::move(existing); break; }
        if (keep != nullptr) keep->refresh(cell, (int) i + 1);
        else { keep = std::make_unique<CellComponent>(*this, cell, (int) i + 1); canvas.addAndMakeVisible(*keep); }
        next.push_back(std::move(keep));
    }
    components = std::move(next);          // cells that are gone are destroyed here
    if (find(selectedId) == nullptr) selectedId.clear();
    layoutCells();
}
juce::String WorksheetPanel::addCell(const juce::String& kind, const juce::String& source)
{
    if (kind == "math") return addMathCell(math_doc::Role::Expression, source);
    if ((kind != "text" && kind != "frust") || document->cells.size() >= 200 || source.length() > maxSource) return {};
    const auto id = juce::Uuid().toString(); document->cells.push_back({id, kind, source, {}, {}}); document->modified = true; rebuild(); revealCell(id); if (onChanged) onChanged(); return id;
}
juce::String WorksheetPanel::addMathCell(math_doc::Role role, const juce::String& linear)
{
    std::vector<std::string> notes;
    const auto id = document->newMathCell(role, linear, notes);
    if (id.isEmpty()) return {};
    rebuild();
    if (auto* cell = find(id))
    {
        juce::StringArray list; for (const auto& n : notes) list.add(juce::String::fromUTF8(n.c_str()));
        cell->setMathNotes(list); layoutCells();
        if (linear.isEmpty()) cell->source.grabKeyboardFocus();
    }
    selectCell(id);
    revealCell(id);
    if (onChanged) onChanged();
    return id;
}
bool WorksheetPanel::setCell(const juce::String& id, const juce::String& source)
{
    int i = document->indexOf(id); if (i < 0 || source.length() > maxSource) return false;
    if (document->cells[(size_t)i].kind == "math")
    {
        mathTextChanged(id, source);
        if (auto* cell = find(id)) { cell->refresh(document->cells[(size_t) i], i + 1); }
        return document->cells[(size_t) i].source == source;
    }
    document->cells[(size_t)i].source = source; document->cells[(size_t)i].output.clear(); document->modified = true; rebuild(); if (onChanged) onChanged(); return true;
}
bool WorksheetPanel::setMathRole(const juce::String& id, math_doc::Role role)
{
    std::vector<std::string> notes;
    if (!document->setMathRole(id, role, notes)) return false;
    rebuild();
    if (auto* cell = find(id)) { juce::StringArray list; for (const auto& n : notes) list.add(juce::String::fromUTF8(n.c_str())); cell->setMathNotes(list); layoutCells(); }
    if (onChanged) onChanged();
    return true;
}
void WorksheetPanel::mathTextChanged(const juce::String& id, const juce::String& text)
{
    std::vector<std::string> notes;
    const bool applied = document->setMathSource(id, text, notes);
    juce::StringArray list; for (const auto& n : notes) list.add(juce::String::fromUTF8(n.c_str()));
    if (auto* cell = find(id))
    {
        const int i = document->indexOf(id);
        if (applied && i >= 0) cell->refresh(document->cells[(size_t) i], i + 1);
        cell->setMathNotes(list);
    }
    layoutCells();
    if (applied && onChanged) onChanged();
}
void WorksheetPanel::revealCell(const juce::String& id)
{
    auto* cell = find(id);
    if (cell == nullptr) return;
    const int top = cell->getY(), bottom = cell->getBottom(), viewTop = viewport.getViewPositionY(), viewHeight = viewport.getViewHeight();
    if (top >= viewTop && bottom <= viewTop + viewHeight) return;
    viewport.setViewPosition(0, std::max(0, cell->getHeight() <= viewHeight ? bottom - viewHeight + 12 : top));
}
void WorksheetPanel::selectCell(const juce::String& id)
{
    if (selectedId == id) return;
    const auto old = selectedId; selectedId = id;
    if (auto* c = find(old)) c->repaint();
    if (auto* c = find(id)) c->repaint();
}
void WorksheetPanel::globalFocusChanged(juce::Component* focused)
{
    for (auto* c = focused; c != nullptr; c = c->getParentComponent())
        if (auto* cell = dynamic_cast<CellComponent*>(c)) { if (cell->owner.document == document) selectCell(cell->id); return; }
}
bool WorksheetPanel::runCell(const juce::String& id, juce::String& error)
{
    const int index = document->indexOf(id);
    if (index < 0 || document->cells[(size_t)index].kind != "frust") { error = "Select a FRust cell to run."; return false; }
    const auto& cell = document->cells[(size_t)index]; frust_exec::Program program;
    program.label = "Worksheet calculation"; program.programId = cell.id.toStdString(); program.script = cell.source.toStdString(); program.programLines = juce::StringArray::fromLines(cell.source).size(); program.prelude = frust_engine::mathPrelude(); program.hostFunctions = frust_engine::mathHostFunctions();
    std::string why; if (!frust_exec::Executor::instance().start(std::move(program), why)) { error = why; return false; }
    runningCell = id; runningSource = cell.source; runningSession = frust_exec::Executor::instance().snapshot().session; executionChanged(); return true;
}
void WorksheetPanel::executionChanged()
{
    const auto snap = frust_exec::Executor::instance().snapshot(); stop.setEnabled(frust_exec::isActive(snap.state));
    if (runningSession == 0 || runningSession != snap.session) return;
    const auto text = frust_exec::isActive(snap.state) ? juce::String(frust_exec::stateName(snap.state)) + juce::String::fromUTF8("…") : snap.state == frust_exec::State::Cancelled ? juce::String("Stopped") : snap.result.ok ? juce::String(snap.result.output) : juce::String(snap.result.report());
    const int i = document->indexOf(runningCell);
    if (i >= 0 && document->cells[(size_t)i].source == runningSource) { document->cells[(size_t)i].output = text; if (auto* cell = find(runningCell)) cell->setOutput(text); }
    if (!frust_exec::isActive(snap.state)) runningSession = 0;
}
