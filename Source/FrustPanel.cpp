#include <algorithm>
#include "FrustPanel.h"

#include "FrustExecution.h"

namespace
{
juce::File sourceFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("DjehutiMath").getChildFile("frust_panel.frust");
}

const char* starterSource =
    "// Frust, compiled in memory by the embedded compiler and run in this app.\n"
    "// run() is called; print_line adds a line of output; what run returns is shown.\n"
    "\n"
    "pub fn run() -> String = {\n"
    "    print_line(\"Frust is running inside Djehuti Math\");\n"
    "    \"ok\"\n"
    "}\n";

void styleButton(juce::TextButton& b, bool primary)
{
    b.setColour(juce::TextButton::buttonColourId, primary ? juce::Colour(0xff2f7f73) : juce::Colour(0xff1d2731));
    b.setColour(juce::TextButton::textColourOffId, primary ? juce::Colours::white : juce::Colour(0xffdce9ee));
}
}

FrustPanel::FrustPanel()
{
    editor = std::make_unique<juce::CodeEditorComponent>(document, &tokeniser);
    editor->setFont(juce::Font("Consolas", 14.0f, juce::Font::plain));
    editor->setTabSize(4, true);
    editor->setColour(juce::CodeEditorComponent::backgroundColourId, juce::Colour(0xff10161d));
    editor->setColour(juce::CodeEditorComponent::defaultTextColourId, juce::Colour(0xffdce9ee));
    editor->setColour(juce::CodeEditorComponent::lineNumberBackgroundId, juce::Colour(0xff151a20));
    editor->setColour(juce::CodeEditorComponent::lineNumberTextId, juce::Colour(0xff71808c));
    editor->setColour(juce::CodeEditorComponent::highlightColourId, juce::Colour(0xff23394a));
    editor->setColour(juce::CaretComponent::caretColourId, juce::Colour(0xff78dcca));
    // The JUCE C++ tokeniser defaults to colours intended for a light editor.
    // Keep DjehutiMath's dark canvas while replacing every token colour with a
    // readable dark-background palette.
    juce::CodeEditorComponent::ColourScheme scheme;
    scheme.set("Error", juce::Colour(0xffff6b6b));
    scheme.set("Comment", juce::Colour(0xff79c98b));
    scheme.set("Keyword", juce::Colour(0xff78dcca));
    scheme.set("Operator", juce::Colour(0xffdce9ee));
    scheme.set("Identifier", juce::Colour(0xffdce9ee));
    scheme.set("Integer", juce::Colour(0xffffc777));
    scheme.set("Float", juce::Colour(0xffffc777));
    scheme.set("String", juce::Colour(0xffffa7c4));
    scheme.set("Bracket", juce::Colour(0xff9cdcfe));
    scheme.set("Punctuation", juce::Colour(0xffb8c9d1));
    scheme.set("Preprocessor Text", juce::Colour(0xffc792ea));
    editor->setColourScheme(scheme);
    addAndMakeVisible(*editor);

    const auto saved = sourceFile().loadFileAsString();
    document.replaceAllContent(saved.trim().isNotEmpty() ? saved : juce::String(starterSource));
    document.clearUndoHistory();

    styleButton(runButton, true);
    styleButton(checkButton, false);
    runButton.setTooltip("Compile with the embedded Frust compiler and run run()");
    checkButton.setTooltip("Compile only and list any errors");
    runButton.onClick = [this] { runFromEditor(); };
    checkButton.onClick = [this] { show(checkNow(getSource()), false); };
    styleButton(stopButton, false);
    stopButton.setTooltip("End the running FRust program");
    stopButton.onClick = [] {
        std::string ignored;
        frust_exec::Executor::instance().stop(ignored);
    };
    addAndMakeVisible(runButton);
    addAndMakeVisible(checkButton);
    addAndMakeVisible(stopButton);

    status.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
    status.setFont(juce::Font(12.5f));
    status.setText("Embedded Frust compiler ready.", juce::dontSendNotification);
    addAndMakeVisible(status);

    output.setMultiLine(true);
    output.setReadOnly(true);
    output.setScrollbarsShown(true);
    output.setFont(juce::Font("Consolas", 13.5f, juce::Font::plain));
    output.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151a20));
    output.setColour(juce::TextEditor::textColourId, juce::Colour(0xffdce9ee));
    output.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff33424d));
    addAndMakeVisible(output);

    listener = frust_exec::Executor::instance().addListener([this](const frust_exec::Snapshot&) { executionChanged(); });
    executionChanged();
}

FrustPanel::~FrustPanel()
{
    // A program still running goes on without the panel (Stop ends it).
    frust_exec::Executor::instance().removeListener(listener);
    saveSource();
}

void FrustPanel::setSource(const juce::String& source)
{
    document.replaceAllContent(source);
    saveSource();
}

juce::String FrustPanel::getSource() const
{
    return document.getAllContent();
}

void FrustPanel::saveSource() const
{
    sourceFile().getParentDirectory().createDirectory();
    sourceFile().replaceWithText(document.getAllContent());
}

bool FrustPanel::start(const juce::String& script, const juce::String& label, std::string& error)
{
    frust_exec::Program program;
    program.label = label.toStdString();
    program.programId = label.toStdString();
    program.script = script.toStdString();
    program.programLines = juce::StringArray::fromLines(script).size();
    program.prelude = frust_engine::mathPrelude();
    program.hostFunctions = frust_engine::mathHostFunctions();
    auto& executor = frust_exec::Executor::instance();
    if (!executor.start(std::move(program), error))
    {
        status.setColour(juce::Label::textColourId, juce::Colour(0xffffb36b));
        status.setText(juce::String(error), juce::dontSendNotification);
        return false;
    }
    setSource(script);
    session = executor.snapshot().session;
    executionChanged();
    return true;
}

frust_engine::Result FrustPanel::checkNow(const juce::String& script)
{
    auto hosts = std::vector<std::string> { "djehuti_frust_log" };
    hosts.insert(hosts.end(), frust_engine::mathHostFunctions().begin(), frust_engine::mathHostFunctions().end());
    const auto header = frust_engine::manifestLine("djehuti_math_script", "check", hosts)
                      + "extern fn djehuti_frust_log(text: String) -> i64;\n"
                        "pub fn print_line(text: String) -> i64 = { djehuti_frust_log(text) }\n"
                      + frust_engine::mathPrelude();
    auto result = frust_engine::check("script.frust", header + script.toStdString());
    for (auto& d : result.diagnostics)
        if (d.file == "script.frust" && d.line > (int) std::count(header.begin(), header.end(), '\n'))
            d.line -= (int) std::count(header.begin(), header.end(), '\n');
    return result;
}

void FrustPanel::runFromEditor()
{
    saveSource();
    std::string error;
    start(getSource(), "Frust panel", error);
}

void FrustPanel::executionChanged()
{
    const auto snap = frust_exec::Executor::instance().snapshot();
    const bool active = frust_exec::isActive(snap.state);
    runButton.setEnabled(!active);
    stopButton.setEnabled(active);
    if (session == 0 || snap.session != session)
    {
        if (active)
        {
            status.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
            status.setText(juce::String(snap.label) + " is " + frust_exec::stateName(snap.state) + ".", juce::dontSendNotification);
        }
        return;
    }
    if (active)
    {
        status.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
        status.setText(snap.state == frust_exec::State::Compiling ? "Compiling..." : "Running...", juce::dontSendNotification);
        return;
    }
    if (snap.state == frust_exec::State::Cancelled)
    {
        output.setText(juce::String(snap.result.output) + (snap.result.output.empty() ? "" : "\n") + "(stopped)", false);
        status.setColour(juce::Label::textColourId, juce::Colour(0xffffb36b));
        status.setText("Stopped.", juce::dontSendNotification);
        session = 0;
        return;
    }
    show(snap.result, true);
    session = 0;
}

void FrustPanel::show(const frust_engine::Result& result, bool ran)
{
    juce::String text;
    if (result.ok)
    {
        text << (ran ? juce::String(result.output) : juce::String("No errors."));
        status.setColour(juce::Label::textColourId, juce::Colour(0xff8fd16b));
        status.setText(ran ? "Ran: compile " + juce::String(result.compileMs, 1) + " ms, run " + juce::String(result.runMs, 3) + " ms"
                           : "Compiles cleanly (" + juce::String(result.compileMs, 1) + " ms).",
                       juce::dontSendNotification);
    }
    else
    {
        text << juce::String(result.report());
        status.setColour(juce::Label::textColourId, juce::Colour(0xffffb36b));
        status.setText(result.diagnostics.empty() ? "Failed." : juce::String((int)result.diagnostics.size()) + " diagnostic(s).",
                       juce::dontSendNotification);
        // Put the caret on the first error's line.
        for (const auto& d : result.diagnostics)
            if (d.line > 0)
            {
                editor->moveCaretTo(juce::CodeDocument::Position(document, d.line - 1, juce::jmax(0, d.column - 1)), false);
                break;
            }
    }
    output.setText(text, false);
}

void FrustPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff10161d));
}

void FrustPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    auto bar = area.removeFromTop(32);
    runButton.setBounds(bar.removeFromLeft(90).reduced(0, 2));
    bar.removeFromLeft(6);
    checkButton.setBounds(bar.removeFromLeft(90).reduced(0, 2));
    bar.removeFromLeft(6);
    stopButton.setBounds(bar.removeFromLeft(70).reduced(0, 2));
    bar.removeFromLeft(10);
    status.setBounds(bar);
    area.removeFromTop(4);
    output.setBounds(area.removeFromBottom(juce::jlimit(80, 260, area.getHeight() / 3)));
    area.removeFromBottom(4);
    editor->setBounds(area);
}
