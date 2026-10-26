#pragma once
#include <JuceHeader.h>
#include "FrustExecution.h"
#include "MathDoc.h"

struct WorksheetCell
{
    juce::String id, kind, source, output;
    // kind == "math": the authoritative structured mathematics. `source` is the linear text it was entered as,
    // kept for the author's convenience; it is never read back as the document.
    math_doc::CellMath math;
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
class WorksheetPanel final : public juce::Component, private juce::FocusChangeListener
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
private:
    class CellComponent;
    void executionChanged();
    void globalFocusChanged(juce::Component* focused) override;
    void layoutCells();
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
};
