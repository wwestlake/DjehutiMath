#include "FrustDebuggerPanel.h"

class FrustDebuggerPanel::Rows final : public juce::ListBoxModel
{
public:
    std::vector<juce::String> rows;
    int highlighted = -1;
    std::function<void(int row, const juce::MouseEvent&)> onClick;

    int getNumRows() override { return (int)rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int)rows.size())
            return;
        if (selected || row == highlighted)
            g.fillAll(juce::Colour(row == highlighted ? 0xff3a3413 : 0xff23394a));
        g.setColour(juce::Colour(0xffdce9ee));
        g.setFont(juce::Font("Consolas", 13.0f, juce::Font::plain));
        g.drawText(rows[(size_t)row], 6, 0, width - 8, height, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked(int row, const juce::MouseEvent& event) override
    {
        if (onClick != nullptr)
            onClick(row, event);
    }
};

namespace
{
void styleButton(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff263942));
    b.setColour(juce::TextButton::textColourOffId, juce::Colour(0xfff0f6f8));
}

void styleTitle(juce::Label& l)
{
    l.setFont(juce::Font(12.5f, juce::Font::bold));
    l.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
}

void styleList(juce::ListBox& list)
{
    list.setRowHeight(20);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff151a20));
    list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff33424d));
    list.setOutlineThickness(1);
}

juce::String where(int line, const std::string& nodeId)
{
    juce::String text = "line " + juce::String(line);
    if (!nodeId.empty())
        text << "  [node " << nodeId << "]";
    return text;
}
}

FrustDebuggerPanel::FrustDebuggerPanel(Actions a)
    : actions(std::move(a))
{
    for (auto* b : { &startButton, &continueButton, &pauseButton, &stopButton, &intoButton, &overButton, &outButton })
    {
        styleButton(*b);
        addAndMakeVisible(*b);
    }
    startButton.setTooltip("Compile the Node Designer's program with the debugger's instrumentation and run it");
    intoButton.setTooltip("Run to the next line, in this call or any call it makes");
    overButton.setTooltip("Run to the next line of this call (or of its caller, if it returns)");
    outButton.setTooltip("Run until this call returns to its caller");
    pauseButton.setTooltip("Stop at the next line the program reaches");
    stopButton.setTooltip("End the program (a paused program is released)");

    startButton.onClick = [this] {
        if (actions.startDebugging == nullptr)
            return;
        const auto why = actions.startDebugging();
        if (why.isNotEmpty())
            report(why);
    };
    auto command = [this](frust_exec::Command c) {
        std::string error;
        if (!frust_exec::Executor::instance().command(c, error))
            report(error);
    };
    continueButton.onClick = [command] { command(frust_exec::Command::Continue); };
    intoButton.onClick = [command] { command(frust_exec::Command::StepInto); };
    overButton.onClick = [command] { command(frust_exec::Command::StepOver); };
    outButton.onClick = [command] { command(frust_exec::Command::StepOut); };
    pauseButton.onClick = [this] {
        std::string error;
        if (!frust_exec::Executor::instance().pause(error))
            report(error);
    };
    stopButton.onClick = [this] {
        std::string error;
        if (!frust_exec::Executor::instance().stop(error))
            report(error);
    };

    status.setColour(juce::Label::textColourId, juce::Colour(0xffdce9ee));
    status.setFont(juce::Font(13.0f, juce::Font::bold));
    addAndMakeVisible(status);

    for (auto* t : { &stackTitle, &variablesTitle, &watchesTitle, &breakpointsTitle, &outputTitle })
    {
        styleTitle(*t);
        addAndMakeVisible(*t);
    }

    stackRows = std::make_unique<Rows>();
    variableRows = std::make_unique<Rows>();
    watchRows = std::make_unique<Rows>();
    breakpointRows = std::make_unique<Rows>();
    stackList.setModel(stackRows.get());
    variableList.setModel(variableRows.get());
    watchList.setModel(watchRows.get());
    breakpointList.setModel(breakpointRows.get());
    for (auto* l : { &stackList, &variableList, &watchList, &breakpointList })
    {
        styleList(*l);
        addAndMakeVisible(*l);
    }

    stackRows->onClick = [this](int row, const juce::MouseEvent&) {
        showFrame(row);
        if (row >= 0 && row < (int)shown.stack.size() && actions.showNode != nullptr && !shown.stack[(size_t)row].lineNodeId.empty())
            actions.showNode(shown.stack[(size_t)row].lineNodeId);
    };
    breakpointRows->onClick = [this](int row, const juce::MouseEvent& event) {
        if (row < 0 || row >= (int)breakpointsShown.size())
            return;
        const auto bp = breakpointsShown[(size_t)row];
        if (event.mods.isPopupMenu())
        {
            if (actions.removeBreakpoint != nullptr)
                actions.removeBreakpoint(bp.nodeId);
        }
        else if (actions.setBreakpointEnabled != nullptr)
            actions.setBreakpointEnabled(bp.nodeId, !bp.enabled);
    };

    output.setMultiLine(true);
    output.setReadOnly(true);
    output.setScrollbarsShown(true);
    output.setFont(juce::Font("Consolas", 13.0f, juce::Font::plain));
    output.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151a20));
    output.setColour(juce::TextEditor::textColourId, juce::Colour(0xffdce9ee));
    output.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff33424d));
    addAndMakeVisible(output);

    auto& executor = frust_exec::Executor::instance();
    listener = executor.addListener([this](const frust_exec::Snapshot& s) { update(s); });
    update(executor.snapshot());
}

FrustDebuggerPanel::~FrustDebuggerPanel()
{
    frust_exec::Executor::instance().removeListener(listener);
    for (auto* l : { &stackList, &variableList, &watchList, &breakpointList })
        l->setModel(nullptr);
}

void FrustDebuggerPanel::report(const juce::String& error)
{
    status.setColour(juce::Label::textColourId, juce::Colour(0xffffb36b));
    status.setText(error, juce::dontSendNotification);
}

void FrustDebuggerPanel::update(const frust_exec::Snapshot& s)
{
    using frust_exec::State;
    // Snapshots can arrive after a newer state was read; show the current one.
    const auto now = frust_exec::Executor::instance().snapshot();
    const auto& snap = now.session >= s.session ? now : s;
    const bool sameStop = shown.state == State::Paused && snap.state == State::Paused && shown.session == snap.session
                          && shown.line == snap.line && shown.stack.size() == snap.stack.size();
    shown = snap;
    if (!sameStop)
        selectedFrame = 0;

    const bool active = frust_exec::isActive(snap.state);
    const bool paused = snap.state == State::Paused;
    startButton.setEnabled(!active);
    continueButton.setEnabled(paused);
    intoButton.setEnabled(paused);
    overButton.setEnabled(paused);
    outButton.setEnabled(paused);
    pauseButton.setEnabled(snap.state == State::Running && snap.debug);
    stopButton.setEnabled(active);

    juce::String text;
    if (snap.session == 0)
        text = "Idle. Start Debugging runs the Node Designer's program.";
    else
    {
        text << snap.label << ": " << frust_exec::stateName(snap.state);
        if (paused)
            text << " (" << snap.pauseReason << ") at " << where(snap.line, snap.nodeId);
        if (!snap.debug && active)
            text << " (Run, not debugging)";
    }
    status.setColour(juce::Label::textColourId, paused ? juce::Colour(0xffffd60a)
                                                       : snap.state == State::Failed ? juce::Colour(0xffffb36b)
                                                                                     : juce::Colour(0xffdce9ee));
    status.setText(text, juce::dontSendNotification);

    stackRows->rows.clear();
    for (const auto& f : snap.stack)
        stackRows->rows.push_back(juce::String(f.function) + "  " + where(f.line, f.lineNodeId));
    stackRows->highlighted = paused ? selectedFrame : -1;
    stackList.updateContent();
    stackList.repaint();
    showFrame(selectedFrame);

    watchRows->rows.clear();
    for (const auto& w : snap.watches)
    {
        juce::String row = juce::String(w.nodeId) + " = ";
        if (w.available)
            row << w.value << "   (" << w.type << ", " << w.function << " line " << w.line << ")";
        else
            row << (w.hasValue ? "not computed yet" : "no value in the generated program");
        watchRows->rows.push_back(row);
    }
    if (snap.watches.empty() && snap.debug)
        watchRows->rows.push_back("(no watches: Set Watch on a node)");
    watchList.updateContent();
    watchList.repaint();

    refreshBreakpoints();

    if (!active && snap.session != 0)
    {
        juce::String out = juce::String(snap.result.output);
        if (!snap.result.ok)
            out << (out.isEmpty() ? "" : "\n") << juce::String(snap.result.report());
        output.setText(out, false);
    }
    else if (active && snap.state == State::Compiling)
        output.setText({}, false);
}

void FrustDebuggerPanel::showFrame(int index)
{
    variableRows->rows.clear();
    if (index >= 0 && index < (int)shown.stack.size())
    {
        selectedFrame = index;
        stackRows->highlighted = index;
        const auto& f = shown.stack[(size_t)index];
        for (const auto& v : f.variables)
            variableRows->rows.push_back(juce::String(v.name) + ": " + juce::String(v.type) + " = " + juce::String(v.value));
        if (f.variables.empty())
            variableRows->rows.push_back("(no values recorded in this call yet)");
    }
    stackList.repaint();
    variableList.updateContent();
    variableList.repaint();
}

void FrustDebuggerPanel::refreshBreakpoints()
{
    // A debug session's own list (lines, hits) while one runs; otherwise
    // the program's markers.
    if (frust_exec::isActive(shown.state) && shown.debug)
        breakpointsShown = shown.breakpoints;
    else
        breakpointsShown = actions.breakpoints != nullptr ? actions.breakpoints() : std::vector<frust_exec::Breakpoint> {};
    breakpointRows->rows.clear();
    for (const auto& bp : breakpointsShown)
    {
        juce::String row;
        row << (bp.enabled ? "[x] " : "[ ] ") << bp.nodeId;
        if (bp.line > 0)
            row << "  line " << bp.line;
        else if (frust_exec::isActive(shown.state))
            row << "  (no code of its own: cannot stop here)";
        if (bp.hits > 0)
            row << "  hits " << bp.hits;
        breakpointRows->rows.push_back(row);
    }
    breakpointList.updateContent();
    breakpointList.repaint();
}

void FrustDebuggerPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff10161d));
}

void FrustDebuggerPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    auto row1 = area.removeFromTop(28);
    startButton.setBounds(row1.removeFromLeft(118).reduced(0, 1));
    row1.removeFromLeft(6);
    stopButton.setBounds(row1.removeFromLeft(60).reduced(0, 1));
    area.removeFromTop(4);
    auto row2 = area.removeFromTop(28);
    for (auto* b : { &continueButton, &pauseButton, &intoButton, &overButton, &outButton })
    {
        b->setBounds(row2.removeFromLeft(b == &pauseButton ? 56 : 78).reduced(0, 1));
        row2.removeFromLeft(4);
    }
    area.removeFromTop(4);
    status.setBounds(area.removeFromTop(22));

    const int sections = 5;
    const int titleHeight = 18;
    const int each = juce::jmax(40, (area.getHeight() - sections * titleHeight) / sections);
    auto section = [&](juce::Label& title, juce::Component& body) {
        title.setBounds(area.removeFromTop(titleHeight));
        body.setBounds(area.removeFromTop(each));
    };
    section(stackTitle, stackList);
    section(variablesTitle, variableList);
    section(watchesTitle, watchList);
    section(breakpointsTitle, breakpointList);
    outputTitle.setBounds(area.removeFromTop(titleHeight));
    output.setBounds(area);
}
