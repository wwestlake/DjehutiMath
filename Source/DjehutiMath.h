#pragma once
#include <JuceHeader.h>
#include <CreationDock/DockManager.h>
#include "Auth/DesktopAuthSession.h"
#include "Auth/ProfileAvatarComponent.h"
#include "DjehutiMenuBarComponent.h"
#include "WorksheetPanel.h"
#include "DjehutiAgent.h"
#include "FrustPanel.h"
#include "AudioPipeline.h"
#include "DspPanel.h"
#include "NodeDesignerPanel.h"
#include "FrustDebuggerPanel.h"
#include "LocalAgentApi.h"

class DjehutiMath final : public juce::Component, private juce::MenuBarModel, private juce::Timer
{
public:
    DjehutiMath();
    ~DjehutiMath() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void confirmCloseThen(std::function<void()>);
    // The approved Djehuti Math emblem at (about) the requested size, from Resources/branding; empty if the file is missing.
    static juce::Image brandImage(int size);
    // The one command entry point (message thread): the agent, the local API and the checks all
    // run tools through it. `done` receives the tool's JSON result, possibly later (a compile, a run).
    void executeCommand(const std::string& name, const std::string& argumentsJson,
                        std::shared_ptr<DjehutiAgentRequestState> state, std::function<void(juce::String)> done);
private:
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int, const juce::String&) override;
    void menuItemSelected(int, int) override;
    void timerCallback() override;
    void chooseFile(bool saveAs);
    void newWorksheet();
    void ifDiscardAllowed(std::function<void()>);
    void showAgentSettings();
    bool applyLayout(juce::var);
    void saveUserLayout();
    juce::var layoutCatalog() const;
    juce::String dispatch(const std::string&, const std::string&, std::shared_ptr<DjehutiAgentRequestState>);
    // Tools whose result arrives later (a program running, a compile) answer through `done`
    // on the message thread; false when `name` is not one of them.
    bool dispatchAsync(const std::string& name, const std::string& arguments, std::function<void(juce::String)> done);
    // Programming tools: FRust checking, node programs, debugging, DSP effects, audio, VST3.
    // Sets `handled` false when `name` is not one of them.
    juce::String dispatchProgramming(const juce::String& name, const juce::var& args, bool& handled);
    void appendLog(const juce::String& text);
    juce::String startNodeDebugging();
    void syncDebugMarkers();
    void executionChanged();
    void refreshProject();
    juce::File stateDirectory() const;
    juce::ApplicationProperties properties;
    juce::Image logo;
    juce::Rectangle<int> logoBounds;
    AudioPipeline audio;
    DesktopAuthSession auth { "djehuti-math" };
    ProfileAvatarComponent avatar { auth };
    DjehutiMenuBarComponent menu { this };
    std::unique_ptr<CreationDock::DockManager> dock;
    std::shared_ptr<WorksheetDocument> document = std::make_shared<WorksheetDocument>();
    WorksheetPanel* worksheet = nullptr;
    FrustPanel* frust = nullptr;
    NodeDesignerPanel* nodes = nullptr;
    FrustDebuggerPanel* debugger = nullptr;
    DspPanel* dsp = nullptr;
    int executionListener = 0;
    std::unique_ptr<LocalAgentApi> localApi;
    std::uint64_t lastReportedSession = 0;
    juce::TextEditor* project = nullptr;
    juce::TextEditor* variables = nullptr;
    juce::TextEditor* diagnostics = nullptr;
    juce::File worksheetFile;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastLayout, persistedLayout, lastVariables;
    std::int64_t changedAt = 0;
};
