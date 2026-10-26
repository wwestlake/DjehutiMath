#pragma once
#include <JuceHeader.h>
#include "FrustExecution.h"
#include "MathDoc.h"

namespace math_workspace { class Workspace; }

struct WorksheetCell
{
    juce::String id, kind, source, output;
    // kind == "math": the authoritative structured mathematics. `source` is the linear text it was entered as,
    // kept for the author's convenience; it is never read back as the document.
    math_doc::CellMath math;
    // How the cell's calculated result is shown (a view preference, not a result): the unit and the number of
    // significant digits. Saved with the cell when they differ from the defaults.
    juce::String displayUnit;
    int displayDigits = 15;
};
class WorksheetDocument
{
public:
    juce::String title = "Untitled worksheet";
    std::vector<WorksheetCell> cells;
    bool modified = false;
    math_doc::IdAllocator nodeIds;   // the next node identifier, saved with the worksheet
    void reset();
    juce::var toJson() const;
    bool load(const juce::File&, juce::String& error);   // format versions 1 and 2
    bool save(const juce::File&, juce::String& error);   // always version 2
    int indexOf(const juce::String& id) const;

    // ---- mathematical cells ----------------------------------------------------------------------------------------
    // Appends a math cell entered as linear text. Returns its id, or "" if the text cannot be represented.
    juce::String newMathCell(math_doc::Role role, const juce::String& linear, std::vector<std::string>& notes);
    // Re-parses a math cell from linear text. Nodes that are still the same keep their identifiers; the cell's
    // revision advances. False (and nothing changes) for an unknown cell or an expression that is too large.
    bool setMathSource(const juce::String& id, const juce::String& linear, std::vector<std::string>& notes);
    bool setMathRole(const juce::String& id, math_doc::Role role, std::vector<std::string>& notes);
    // The immutable view of a math cell that the mathematical compiler receives.
    bool snapshot(const juce::String& id, math_doc::MathSnapshot& out) const;
};
class WorksheetPanel final : public juce::Component, private juce::FocusChangeListener, private juce::Timer
{
public:
    explicit WorksheetPanel(std::shared_ptr<WorksheetDocument>);
    ~WorksheetPanel() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    // Brings the components in line with the document. Components of cells that still exist keep their
    // state (focus, caret, scroll); only added and removed cells are created and destroyed.
    void rebuild();
    juce::String addCell(const juce::String& kind, const juce::String& source);   // text, frust or math
    juce::String addMathCell(math_doc::Role role, const juce::String& linear);
    bool setCell(const juce::String& id, const juce::String& source);
    bool setMathRole(const juce::String& id, math_doc::Role role);
    bool runCell(const juce::String& id, juce::String& error);
    juce::String selectedCell() const { return selectedId; }
    void selectCell(const juce::String& id);
    void revealCell(const juce::String& id);   // scrolls the worksheet so the cell is in view
    // The notes the linear parser made about the last input of a math cell (empty when there were none).
    juce::StringArray notesFor(const juce::String& id) const;
    // Verification support: how many cell components exist, and which object shows a cell.
    int componentCount() const { return (int) components.size(); }
    const void* componentFor(const juce::String& id) const;
    int cellHeight(const juce::String& id) const;   // the laid-out height of a cell
    std::function<void()> onChanged;

    // ---- calculated results (S1) ------------------------------------------------------------------------------------------------
    // The panel shows what a math_workspace::Workspace has computed for each math cell. It never computes anything
    // itself: evaluation goes through `runCommand`, the same command service the agent tools use.
    void setMathWorkspace(math_workspace::Workspace* workspace);
    std::function<juce::String(const std::string& tool, const juce::var& arguments)> runCommand;
    void evaluateCell(const juce::String& id);                  // evaluate (and show) one cell
    void evaluateWorksheet();                                    // evaluate every cell whose result is not current
    void refreshResults();                                       // re-render results from the workspace
    void setAutoEvaluate(bool shouldEvaluate);                   // recalculate after edits, after a short pause
    bool autoEvaluate() const { return autoEvaluating; }
    void setDisplay(const juce::String& id, const juce::String& unit, int digits);
    bool recalculationPending() const { return isTimerRunning(); }   // an edit is waiting for the pause before recalculation
    void flushRecalculation() { timerCallback(); }                      // run that recalculation now (the timer calls the same function)
    // Verification support: what a cell's result area currently shows.
    struct ResultView { bool visible = false, isValue = false, stale = false; juce::String text, status, unit; int digits = 15; int height = 0; };
    ResultView resultOf(const juce::String& id) const;
private:
    class CellComponent;
    void executionChanged();
    void globalFocusChanged(juce::Component* focused) override;
    void layoutCells();
    void timerCallback() override;
    void scheduleRecalculation();
    void mathTextChanged(const juce::String& id, const juce::String& text);
    CellComponent* find(const juce::String& id) const;
    std::shared_ptr<WorksheetDocument> document;
    juce::Label heading, subtitle;
    juce::TextButton addCode { "+ FRust cell" }, addNote { "+ Note" }, addMath { "+ Math" }, stop { "Stop" };
    juce::Viewport viewport;
    juce::Component canvas;
    std::vector<std::unique_ptr<CellComponent>> components;
    juce::String runningCell, runningSource, selectedId;
    std::uint64_t runningSession = 0;
    int listener = 0;
    math_workspace::Workspace* workspace = nullptr;
    juce::TextButton evaluateAll { "Evaluate all" };
    juce::ToggleButton autoButton { "Auto" };
    bool autoEvaluating = true;
};
