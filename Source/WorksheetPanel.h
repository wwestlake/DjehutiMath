#pragma once
#include <JuceHeader.h>
#include "FrustExecution.h"

struct WorksheetCell
{
    juce::String id, kind, source, output;
};
class WorksheetDocument
{
public:
    juce::String title = "Untitled worksheet";
    std::vector<WorksheetCell> cells;
    bool modified = false;
    void reset();
    juce::var toJson() const;
    bool load(const juce::File&, juce::String& error);
    bool save(const juce::File&, juce::String& error);
    int indexOf(const juce::String& id) const;
};
class WorksheetPanel final : public juce::Component
{
public:
    explicit WorksheetPanel(std::shared_ptr<WorksheetDocument>);
    ~WorksheetPanel() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void rebuild();
    juce::String addCell(const juce::String& kind, const juce::String& source);
    bool setCell(const juce::String& id, const juce::String& source);
    bool runCell(const juce::String& id, juce::String& error);
    std::function<void()> onChanged;
private:
    class CellComponent;
    void executionChanged();
    std::shared_ptr<WorksheetDocument> document;
    juce::Label heading, subtitle;
    juce::TextButton addCode { "+ FRust cell" }, addNote { "+ Note" }, stop { "Stop" };
    juce::Viewport viewport;
    juce::Component canvas;
    std::vector<std::unique_ptr<CellComponent>> components;
    juce::String runningCell, runningSource;
    std::uint64_t runningSession = 0;
    int listener = 0;
};
