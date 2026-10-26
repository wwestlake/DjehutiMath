#pragma once

// The Debugger tab: FrustIDE's DebuggerPanel (commit 431543f; Resume, Step
// Over and a status line) adapted to the Workbench's FRust executor
// (FrustExecution). It shows the execution state, where the program is
// stopped, the call stack, the selected call's variables, the watches and
// the breakpoints, and drives Start Debugging, Continue, Pause, Stop and
// Step Into / Over / Out. It never waits: the executor tells it about every
// change on the message thread.

#include <JuceHeader.h>

#include "FrustExecution.h"

#include <functional>
#include <memory>
#include <vector>

class FrustDebuggerPanel final : public juce::Component
{
public:
    struct Actions
    {
        // Start Debugging the Node Designer's program; returns "" or why not.
        std::function<juce::String()> startDebugging;
        // The Node Designer's breakpoints (shown while no session runs).
        std::function<std::vector<frust_exec::Breakpoint>()> breakpoints;
        std::function<void(const juce::String& nodeId, bool enabled)> setBreakpointEnabled;
        std::function<void(const juce::String& nodeId)> removeBreakpoint;
        std::function<void(const juce::String& nodeId)> showNode;
    };

    explicit FrustDebuggerPanel(Actions actions);
    ~FrustDebuggerPanel() override;

    // The node program's breakpoints changed.
    void refreshBreakpoints();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class Rows;

    void update(const frust_exec::Snapshot& snapshot);
    void showFrame(int index);
    void report(const juce::String& error);

    Actions actions;
    int listener = 0;
    frust_exec::Snapshot shown;
    int selectedFrame = 0;

    juce::TextButton startButton { "Start Debugging" }, continueButton { "Continue" }, pauseButton { "Pause" },
                     stopButton { "Stop" }, intoButton { "Step Into" }, overButton { "Step Over" }, outButton { "Step Out" };
    juce::Label status;
    juce::Label stackTitle { {}, "Call stack" }, variablesTitle { {}, "Variables" }, watchesTitle { {}, "Watches" },
                breakpointsTitle { {}, "Breakpoints (click: enable/disable, right-click: remove)" }, outputTitle { {}, "Output" };
    std::unique_ptr<Rows> stackRows, variableRows, watchRows, breakpointRows;
    juce::ListBox stackList, variableList, watchList, breakpointList;
    juce::TextEditor output;
    std::vector<frust_exec::Breakpoint> breakpointsShown;
};
