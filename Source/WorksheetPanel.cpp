#include "WorksheetPanel.h"
#include <set>

namespace
{
constexpr auto starter = "pub fn run() -> String = {\n    let value: f64 = 6.0 * 7.0;\n    math_set_scalar(\"answer\", value);\n    print_line(\"Computed answer; inspect the Variables panel.\");\n    \"Completed\"\n}\n";
void style(juce::TextEditor& editor)
{
    editor.setMultiLine(true); editor.setReturnKeyStartsNewLine(true);
    editor.setFont(juce::Font("Consolas", 14.0f, juce::Font::plain));
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151c26));
    editor.setColour(juce::TextEditor::textColourId, juce::Colour(0xffe2e8f0));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
}
}
void WorksheetDocument::reset()
{
    title = "Engineering worksheet"; cells.clear(); modified = false;
    cells.push_back({juce::Uuid().toString(), "text", juce::String::fromUTF8("Djehuti Math\n\nWrite engineering notes and run FRust calculations in the same worksheet. The example stores 6 × 7 in the shared scalar workspace. Use FRust for the full editor and Variables to inspect its result."), {}});
    cells.push_back({juce::Uuid().toString(), "frust", starter, {}});
}
juce::var WorksheetDocument::toJson() const
{
    auto* root = new juce::DynamicObject(); root->setProperty("format", "djehuti-math-worksheet"); root->setProperty("version", 1); root->setProperty("title", title);
    juce::Array<juce::var> values;
    for (const auto& cell : cells)
    {
        auto* item = new juce::DynamicObject(); item->setProperty("id", cell.id); item->setProperty("kind", cell.kind); item->setProperty("source", cell.source); values.add(juce::var(item));
    }
    root->setProperty("cells", values); return juce::var(root);
}
int WorksheetDocument::indexOf(const juce::String& id) const { for (size_t i = 0; i < cells.size(); ++i) if (cells[i].id == id) return (int)i; return -1; }
bool WorksheetDocument::load(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile() || file.getSize() > 8 * 1024 * 1024) { error = "Worksheet is missing or exceeds the 8 MB foundation limit."; return false; }
    const auto parsed = juce::JSON::parse(file.loadFileAsString());
    const auto* array = parsed.getProperty("cells", {}).getArray();
    if (parsed.getProperty("format", "") != juce::var("djehuti-math-worksheet") || (int)parsed.getProperty("version", 0) != 1 || array == nullptr || array->size() > 200)
    { error = "Unsupported worksheet format or version."; return false; }
    std::vector<WorksheetCell> candidate; std::set<juce::String> ids;
    for (const auto& value : *array)
    {
        WorksheetCell cell {value["id"].toString(), value["kind"].toString(), value["source"].toString(), {}};
        if (cell.id.isEmpty() || !ids.insert(cell.id).second || (cell.kind != "text" && cell.kind != "frust") || cell.source.length() > 100000)
        { error = "Invalid or duplicate worksheet cell."; return false; }
        candidate.push_back(std::move(cell));
    }
    cells = std::move(candidate); title = parsed.getProperty("title", "Worksheet").toString(); modified = false; return true;
}
bool WorksheetDocument::save(const juce::File& file, juce::String& error)
{
    juce::TemporaryFile temporary(file);
    if (!temporary.getFile().replaceWithText(juce::JSON::toString(toJson(), false)) || !temporary.overwriteTargetFileWithTemporary()) { error = "Could not save worksheet: " + file.getFullPathName(); return false; }
    modified = false; return true;
}

class WorksheetPanel::CellComponent final : public juce::Component
{
public:
    CellComponent(WorksheetPanel& ownerIn, const WorksheetCell& cell, int ordinal) : owner(ownerIn), id(cell.id), isCode(cell.kind == "frust")
    {
        title.setText(juce::String(ordinal) + (isCode ? "   FRust calculation" : "   Notes"), juce::dontSendNotification);
        title.setColour(juce::Label::textColourId, juce::Colour(0xff95a8bd)); addAndMakeVisible(title);
        style(source); source.setText(cell.source, false); source.onTextChange = [this] { const int i = owner.document->indexOf(id); if (i >= 0) { owner.document->cells[(size_t)i].source = source.getText(); owner.document->modified = true; if (owner.onChanged) owner.onChanged(); } }; addAndMakeVisible(source);
        style(output); output.setReadOnly(true); output.setText(cell.output, false); addAndMakeVisible(output); output.setVisible(isCode);
        run.setButtonText("Run cell"); run.onClick = [this] { juce::String error; if (!owner.runCell(id, error)) output.setText(error, false); }; if (isCode) addAndMakeVisible(run);
    }
    void paint(juce::Graphics& g) override { g.setColour(juce::Colour(0xff1b2430)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f); }
    void resized() override { auto a = getLocalBounds().reduced(14); auto bar = a.removeFromTop(30); run.setBounds(bar.removeFromRight(88)); title.setBounds(bar); a.removeFromTop(6); if (isCode) { output.setBounds(a.removeFromBottom(60)); a.removeFromBottom(8); } source.setBounds(a); }
    void setOutput(const juce::String& text) { output.setText(text, false); }
    WorksheetPanel& owner; juce::String id; bool isCode; juce::Label title; juce::TextEditor source, output; juce::TextButton run;
};
WorksheetPanel::WorksheetPanel(std::shared_ptr<WorksheetDocument> value) : document(std::move(value))
{
    heading.setFont(juce::Font(24.0f, juce::Font::bold)); heading.setColour(juce::Label::textColourId, juce::Colour(0xffe2e8f0)); addAndMakeVisible(heading);
    subtitle.setText(juce::String::fromUTF8("Notes · FRust calculations · Inspectable results"), juce::dontSendNotification); subtitle.setColour(juce::Label::textColourId, juce::Colour(0xff95a8bd)); addAndMakeVisible(subtitle);
    addCode.onClick = [this] { addCell("frust", starter); }; addNote.onClick = [this] { addCell("text", "Engineering notes"); };
    stop.onClick = [] { std::string ignored; frust_exec::Executor::instance().stop(ignored); };
    addAndMakeVisible(addCode); addAndMakeVisible(addNote); addAndMakeVisible(stop);
    viewport.setViewedComponent(&canvas, false); viewport.setScrollBarsShown(true, false); addAndMakeVisible(viewport);
    listener = frust_exec::Executor::instance().addListener([this](const frust_exec::Snapshot&) { executionChanged(); }); rebuild();
}
WorksheetPanel::~WorksheetPanel() { frust_exec::Executor::instance().removeListener(listener); viewport.setViewedComponent(nullptr, false); }
void WorksheetPanel::paint(juce::Graphics& g) { g.fillAll(juce::Colour(0xff101720)); }
void WorksheetPanel::resized()
{
    auto a = getLocalBounds().reduced(22); auto bar = a.removeFromTop(34); stop.setBounds(bar.removeFromRight(64)); bar.removeFromRight(8); addNote.setBounds(bar.removeFromRight(88)); bar.removeFromRight(8); addCode.setBounds(bar.removeFromRight(112)); heading.setBounds(bar); subtitle.setBounds(a.removeFromTop(27)); a.removeFromTop(12); viewport.setBounds(a);
    int y = 0; const int width = juce::jmax(200, viewport.getWidth() - 16);
    for (auto& cell : components) { const int height = cell->isCode ? 300 : 165; cell->setBounds(0, y, width, height); y += height + 18; }
    canvas.setSize(width, juce::jmax(y, viewport.getHeight()));
}
void WorksheetPanel::rebuild()
{
    components.clear(); heading.setText(document->title, juce::dontSendNotification);
    for (size_t i = 0; i < document->cells.size(); ++i) { auto cell = std::make_unique<CellComponent>(*this, document->cells[i], (int)i + 1); canvas.addAndMakeVisible(*cell); components.push_back(std::move(cell)); }
    resized();
}
juce::String WorksheetPanel::addCell(const juce::String& kind, const juce::String& source)
{
    if ((kind != "text" && kind != "frust") || document->cells.size() >= 200 || source.length() > 100000) return {};
    const auto id = juce::Uuid().toString(); document->cells.push_back({id, kind, source, {}}); document->modified = true; rebuild(); if (onChanged) onChanged(); return id;
}
bool WorksheetPanel::setCell(const juce::String& id, const juce::String& source)
{
    int i = document->indexOf(id); if (i < 0 || source.length() > 100000) return false;
    document->cells[(size_t)i].source = source; document->cells[(size_t)i].output.clear(); document->modified = true; rebuild(); if (onChanged) onChanged(); return true;
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
    if (i >= 0 && document->cells[(size_t)i].source == runningSource) { document->cells[(size_t)i].output = text; for (auto& cell : components) if (cell->id == runningCell) cell->setOutput(text); }
    if (!frust_exec::isActive(snap.state)) runningSession = 0;
}
