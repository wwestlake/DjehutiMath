#pragma once

#include <JuceHeader.h>

#include "FrustEngine.h"

#include <cstdint>
#include <string>

// Write Frust and run it inside the app. The source is compiled in memory by
// the embedded Frust compiler; `run()` is called and what it returns (after
// any print_line output) is shown, or the compile diagnostics with their lines.
// The agent's frust_check / frust_run tools go through the same panel. A run
// is on the app's FRust worker (frust_exec::Executor), never the message
// thread; Stop ends it.
class FrustPanel final : public juce::Component
{
public:
    FrustPanel();
    ~FrustPanel() override;

    void setSource(const juce::String& source);
    juce::String getSource() const;

    // Shows the script and starts it on the FRust worker; its result is shown
    // when it ends. False (with error) while another program runs.
    bool start(const juce::String& script, const juce::String& label, std::string& error);
    // The session start() began (0 before any).
    std::uint64_t lastSession() const { return session; }
    // Compile only, on the calling thread.
    frust_engine::Result checkNow(const juce::String& script);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void runFromEditor();
    void executionChanged();
    void show(const frust_engine::Result& result, bool ran);
    void saveSource() const;

    juce::CodeDocument document;
    juce::CPlusPlusCodeTokeniser tokeniser;
    std::unique_ptr<juce::CodeEditorComponent> editor;
    juce::TextButton runButton { "Run" }, checkButton { "Check" }, stopButton { "Stop" };
    juce::Label status;
    juce::TextEditor output;
    std::uint64_t session = 0;
    int listener = 0;
};
