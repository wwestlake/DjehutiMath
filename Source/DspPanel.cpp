#include "DspPanel.h"

#include <algorithm>
#include <cmath>

namespace
{
juce::File sourceFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("DjehutiMath").getChildFile("dsp_effect.frust");
}

void styleButton(juce::TextButton& b, bool primary)
{
    b.setColour(juce::TextButton::buttonColourId, primary ? juce::Colour(0xff2f7f73) : juce::Colour(0xff1d2731));
    b.setColour(juce::TextButton::textColourOffId, primary ? juce::Colours::white : juce::Colour(0xffdce9ee));
}

void styleLabel(juce::Label& l)
{
    l.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
    l.setFont(juce::Font(12.5f));
}

dsp_effect::Signal signalFrom(const juce::String& name, bool& ok)
{
    ok = true;
    if (name.equalsIgnoreCase("sine")) return dsp_effect::Signal::Sine;
    if (name.equalsIgnoreCase("noise")) return dsp_effect::Signal::Noise;
    if (name.equalsIgnoreCase("impulse")) return dsp_effect::Signal::Impulse;
    if (name.equalsIgnoreCase("sweep")) return dsp_effect::Signal::Sweep;
    if (name.equalsIgnoreCase("silence")) return dsp_effect::Signal::Silence;
    ok = false;
    return dsp_effect::Signal::Sine;
}

juce::var statsVar(const dsp_effect::Measurement& m)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("peak", m.peak);
    o->setProperty("peakDbfs", m.peak > 0.0 ? 20.0 * std::log10(m.peak) : -200.0);
    o->setProperty("rms", m.rms);
    o->setProperty("dc", m.dc);
    o->setProperty("clippedSamples", m.clipped);
    o->setProperty("nonFiniteSamples", m.nonFinite);
    return juce::var(o);
}
}

DspPanel::DspPanel(AudioPipeline& audio) : pipeline(audio)
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
    document.replaceAllContent(saved.trim().isNotEmpty() ? saved : juce::String(dsp_effect::starterSource("softclip")));
    document.clearUndoHistory();

    starterBox.addItem("Load template...", 1);
    int id = 2;
    for (const auto& kind : dsp_effect::starterKinds())
        starterBox.addItem(juce::String(kind), id++);
    starterBox.setSelectedId(1, juce::dontSendNotification);
    starterBox.onChange = [this] {
        const auto index = starterBox.getSelectedId() - 2;
        const auto kinds = dsp_effect::starterKinds();
        if (index < 0 || index >= (int) kinds.size())
            return;
        const auto kind = juce::String(kinds[(size_t) index]);
        starterBox.setSelectedId(1, juce::dontSendNotification);
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Load template",
            "Replace the effect source with the " + kind + " template?", "Replace", "Cancel", this,
            juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<DspPanel>(this), kind](int ok) {
                juce::String error;
                if (safe != nullptr && ok != 0 && !safe->loadStarter(kind, error))
                    safe->setStatus(error, true);
            }));
    };
    addAndMakeVisible(starterBox);

    for (const char* name : { "Sine", "Noise", "Impulse", "Sweep" })
        signalBox.addItem(name, signalBox.getNumItems() + 1);
    signalBox.setSelectedId(1, juce::dontSendNotification);
    addAndMakeVisible(signalBox);
    inputBox.addItem("Hardware input", 1);
    inputBox.addItem("File (looping)", 2);
    inputBox.setSelectedId(1, juce::dontSendNotification);
    inputBox.onChange = [this] {
        if (inputBox.getSelectedId() == 2 && inputFilePath.isEmpty())
            chooseInputFile();
        else
        {
            juce::String error;
            setLiveInput(inputBox.getSelectedId() == 2 ? "File" : "Hardware", inputFilePath, error);
            if (error.isNotEmpty()) setStatus(error, true);
        }
    };
    addAndMakeVisible(inputBox);

    for (auto* l : { &starterLabel, &signalLabel, &inputLabel, &meterLabel })
    {
        styleLabel(*l);
        addAndMakeVisible(*l);
    }
    styleButton(compileButton, true);
    styleButton(liveButton, false);
    for (auto* b : { &audioButton, &fileButton, &renderButton, &playButton, &wavButton, &vstButton })
        styleButton(*b, false);
    compileButton.setTooltip("Compile the effect to native code and link it");
    liveButton.setTooltip("Run the compiled effect on the audio device (input to output)");
    renderButton.setTooltip("Render the selected test signal through the effect and measure it");
    vstButton.setTooltip("Package the compiled effect as a VST3 plugin bundle");
    compileButton.onClick = [this] { compile(); };
    liveButton.onClick = [this] { toggleLive(); };
    audioButton.onClick = [this] { pipeline.showAudioSettings(this); };
    fileButton.onClick = [this] { chooseInputFile(); };
    renderButton.onClick = [this] {
        const auto result = render(signalBox.getText(), 1000.0, 2.0, 0.5, 48000.0, {});
        showRenderStats(result);
    };
    playButton.onClick = [this] {
        juce::String error;
        if (!playLastRender(error)) setStatus(error, true);
    };
    wavButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Save the processed render", juce::File(), "*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<DspPanel>(this)](const juce::FileChooser& fc) {
                if (safe == nullptr || fc.getResult() == juce::File()) return;
                juce::String error;
                if (!safe->exportLastRender(fc.getResult().withFileExtension("wav"), true, error))
                    safe->setStatus(error, true);
            });
    };
    vstButton.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser>("Choose the folder for the VST3 package", juce::File(), "*");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [safe = juce::Component::SafePointer<DspPanel>(this)](const juce::FileChooser& fc) {
                if (safe == nullptr || fc.getResult() == juce::File()) return;
                const auto result = safe->exportVst3(fc.getResult());
                safe->setStatus(result.ok ? "Exported " + result.bundleDirectory.getFullPathName() : juce::String(result.error), !result.ok);
            });
    };
    for (auto* b : { &compileButton, &liveButton, &audioButton, &fileButton, &renderButton, &playButton, &wavButton, &vstButton })
        addAndMakeVisible(*b);

    status.setColour(juce::Label::textColourId, juce::Colour(0xff93a7b0));
    status.setFont(juce::Font(12.5f));
    addAndMakeVisible(status);
    report.setMultiLine(true);
    report.setReadOnly(true);
    report.setScrollbarsShown(true);
    report.setFont(juce::Font("Consolas", 13.0f, juce::Font::plain));
    report.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff151a20));
    report.setColour(juce::TextEditor::textColourId, juce::Colour(0xffdce9ee));
    report.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff33424d));
    addAndMakeVisible(report);
    sliderView.setViewedComponent(&sliderHolder, false);
    sliderView.setScrollBarsShown(true, false);
    addAndMakeVisible(sliderView);
    setStatus("Compile the effect to hear it, render it, or export it as a VST3.");
    startTimerHz(10);
}

DspPanel::~DspPanel()
{
    stopTimer();
    stopLive();
    pipeline.stopRenderedAudio();
    sourceFile().getParentDirectory().createDirectory();
    sourceFile().replaceWithText(document.getAllContent());
    sliderView.setViewedComponent(nullptr, false);
}

void DspPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff101720));
    juce::Rectangle<int> meters(sliderView.getX(), sliderView.getY() - 42, sliderView.getWidth(), 40);
    g.setColour(juce::Colour(0xff1d2731));
    auto in = meters.removeFromTop(16).reduced(2, 3), out = meters.reduced(2, 3);
    g.fillRect(in);
    g.fillRect(out);
    const auto bar = [&](juce::Rectangle<int> r, float level) {
        const float db = level > 0.0f ? juce::Decibels::gainToDecibels(level, -60.0f) : -60.0f;
        const float fraction = juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f);
        g.setColour(level >= 1.0f ? juce::Colour(0xffff6b6b) : juce::Colour(0xff78dcca));
        g.fillRect(r.withWidth((int) (r.getWidth() * fraction)));
    };
    bar(in, inLevel);
    bar(out, outLevel);
}

void DspPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto row1 = area.removeFromTop(28);
    starterLabel.setBounds(row1.removeFromLeft(62));
    starterBox.setBounds(row1.removeFromLeft(150));
    row1.removeFromLeft(10);
    compileButton.setBounds(row1.removeFromLeft(86));
    row1.removeFromLeft(6);
    liveButton.setBounds(row1.removeFromLeft(96));
    row1.removeFromLeft(6);
    audioButton.setBounds(row1.removeFromLeft(130));
    area.removeFromTop(6);
    auto row2 = area.removeFromTop(28);
    inputLabel.setBounds(row2.removeFromLeft(62));
    inputBox.setBounds(row2.removeFromLeft(130));
    row2.removeFromLeft(4);
    fileButton.setBounds(row2.removeFromLeft(96));
    row2.removeFromLeft(12);
    signalLabel.setBounds(row2.removeFromLeft(72));
    signalBox.setBounds(row2.removeFromLeft(90));
    row2.removeFromLeft(6);
    renderButton.setBounds(row2.removeFromLeft(100));
    row2.removeFromLeft(6);
    playButton.setBounds(row2.removeFromLeft(100));
    row2.removeFromLeft(6);
    wavButton.setBounds(row2.removeFromLeft(96));
    row2.removeFromLeft(6);
    vstButton.setBounds(row2.removeFromLeft(110));
    area.removeFromTop(6);
    status.setBounds(area.removeFromBottom(22));
    report.setBounds(area.removeFromBottom(150));
    area.removeFromBottom(6);
    auto right = area.removeFromRight(280);
    area.removeFromRight(8);
    meterLabel.setBounds(right.removeFromTop(18));
    right.removeFromTop(42);   // the meter bars paint here
    sliderView.setBounds(right);
    editor->setBounds(area);
    int y = 0;
    const int width = std::max(120, sliderView.getWidth() - 14);
    for (auto& s : sliders)
    {
        s.label->setBounds(0, y, width, 18);
        s.slider->setBounds(0, y + 18, width, 24);
        y += 50;
    }
    sliderHolder.setSize(width, std::max(y, sliderView.getHeight()));
}

void DspPanel::log(const juce::String& text)
{
    if (onLog) onLog(text);
}

void DspPanel::setStatus(const juce::String& text, bool error)
{
    statusText = text;
    status.setColour(juce::Label::textColourId, error ? juce::Colour(0xffffb36b) : juce::Colour(0xff93a7b0));
    status.setText(text, juce::dontSendNotification);
}

void DspPanel::setSource(const juce::String& text)
{
    document.replaceAllContent(text);
}

juce::String DspPanel::source() const { return document.getAllContent(); }

bool DspPanel::loadStarter(const juce::String& kind, juce::String& error)
{
    const auto kinds = dsp_effect::starterKinds();
    if (std::find(kinds.begin(), kinds.end(), kind.toStdString()) == kinds.end())
    {
        error = "Unknown template '" + kind + "'. Use gain, softclip, lowpass or tremolo.";
        return false;
    }
    setSource(juce::String(dsp_effect::starterSource(kind.toStdString())));
    setStatus("Loaded the " + kind + " template. Compile to use it.");
    return true;
}

void DspPanel::rebuildSliders()
{
    sliders.clear();
    sliderHolder.removeAllChildren();
    if (!hasEffect())
    {
        resized();
        return;
    }
    const auto& params = runtime->artifact().spec.params;
    for (size_t i = 0; i < params.size(); ++i)
    {
        Slider s;
        s.id = params[i].id;
        s.label = std::make_unique<juce::Label>();
        s.label->setText(juce::String(params[i].name) + (params[i].unit.empty() ? "" : " (" + juce::String(params[i].unit) + ")"), juce::dontSendNotification);
        s.label->setColour(juce::Label::textColourId, juce::Colour(0xffdce9ee));
        s.slider = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s.slider->setRange(params[i].minValue, params[i].maxValue, 0.0);
        s.slider->setSkewFactorFromMidPoint(params[i].minValue > 0.0 && params[i].maxValue / params[i].minValue > 50.0
                                                ? std::sqrt(params[i].minValue * params[i].maxValue)
                                                : 0.5 * (params[i].minValue + params[i].maxValue));
        s.slider->setValue(runtime->param((int) i), juce::dontSendNotification);
        s.slider->onValueChange = [this, i, p = s.slider.get()] {
            if (hasEffect()) runtime->setParam((int) i, p->getValue());
        };
        sliderHolder.addAndMakeVisible(*s.label);
        sliderHolder.addAndMakeVisible(*s.slider);
        sliders.push_back(std::move(s));
    }
    resized();
}

void DspPanel::compile(std::function<void(bool, const juce::String&)> done)
{
    if (compiling)
    {
        if (done) done(false, "A compile is already running.");
        return;
    }
    dsp_effect::Spec spec;
    std::string error;
    if (!dsp_effect::parseSpec(source().toStdString(), spec, error))
    {
        compileMessage = juce::String(error);
        setStatus(compileMessage, true);
        report.setText(compileMessage, false);
        log("DSP effect: " + compileMessage);
        if (done) done(false, compileMessage);
        return;
    }
    compiling = true;
    compileButton.setEnabled(false);
    const auto generation = ++compileGeneration;
    setStatus("Compiling " + juce::String(spec.name) + "...");
    juce::Thread::launch([safe = juce::Component::SafePointer<DspPanel>(this), spec, generation, done = std::move(done)]() mutable {
        auto compiled = dsp_effect::compile(spec);
        juce::MessageManager::callAsync([safe, spec = std::move(spec), compiled = std::move(compiled), generation, done = std::move(done)]() mutable {
            if (safe == nullptr || generation != safe->compileGeneration)
                return;
            safe->applyCompiled(spec, std::move(compiled), std::move(done));
        });
    });
}

void DspPanel::applyCompiled(const dsp_effect::Spec& spec, dsp_effect::Compiled compiled,
                             std::function<void(bool, const juce::String&)> done)
{
    compiling = false;
    compileButton.setEnabled(true);
    const auto finish = [&](bool ok, const juce::String& message) {
        compileMessage = message;
        setStatus(message, !ok);
        report.setText(message, false);
        log("DSP effect: " + message);
        if (done) done(ok, message);
    };
    if (!compiled.ok)
        return finish(false, "Compile failed:\n" + juce::String(compiled.error).trim());
    const auto artifact = dsp_effect::makeArtifact(spec, compiled);
    auto fresh = std::make_shared<dsp_effect::Runtime>();
    std::string error;
    if (!fresh->load(artifact, error))
        return finish(false, "Linking failed: " + juce::String(error));
    const bool resume = live;
    if (resume)
        pipeline.setProcessCallback(nullptr);
    fresh->prepare(pipeline.isOpen() ? pipeline.sampleRate() : 48000.0, 1);
    runtime = fresh;
    rebuildSliders();
    if (resume)
        pipeline.setProcessCallback([rt = runtime](const float* in, float* out, int n) { rt->process(in, out, n, 0); });
    sourceFile().getParentDirectory().createDirectory();
    sourceFile().replaceWithText(document.getAllContent());
    finish(true, "Compiled " + juce::String(spec.name) + ": " + juce::String((int) compiled.object.size()) + " bytes of native code, "
                 + juce::String((int) spec.params.size()) + " parameter(s), " + juce::String(compiled.compileMs, 0) + " ms.");
}

bool DspPanel::setParameter(const juce::String& id, double value, juce::String& error)
{
    if (!hasEffect()) { error = "Compile an effect first."; return false; }
    const auto& params = runtime->artifact().spec.params;
    for (size_t i = 0; i < params.size(); ++i)
        if (params[i].id == id.toStdString())
        {
            runtime->setParam((int) i, value);
            if (i < sliders.size())
                sliders[i].slider->setValue(runtime->param((int) i), juce::dontSendNotification);
            return true;
        }
    error = "The effect has no parameter '" + id + "'.";
    return false;
}

bool DspPanel::startLive(juce::String& message)
{
    if (!hasEffect()) { message = "Compile an effect first."; return false; }
    if (live) { message = "Already live."; return true; }
    if (const auto error = pipeline.open(); error.isNotEmpty())
    {
        message = "No audio device could be opened: " + error;
        return false;
    }
    pipeline.setProcessCallback(nullptr);
    liveRate = pipeline.sampleRate();
    runtime->prepare(liveRate, 1);
    pipeline.setProcessCallback([rt = runtime](const float* in, float* out, int n) { rt->process(in, out, n, 0); });
    live = true;
    liveButton.setButtonText("Stop Live");
    message = "Live on " + pipeline.describeDevice() + ".";
    setStatus(message);
    log("DSP effect: " + message);
    return true;
}

void DspPanel::stopLive()
{
    if (!live)
        return;
    pipeline.setProcessCallback(nullptr);
    pipeline.close(); // release the device; it reopens when asked for
    live = false;
    liveButton.setButtonText("Start Live");
    setStatus("Live audio stopped; the audio device was released.");
}

void DspPanel::toggleLive()
{
    juce::String message;
    if (live) stopLive();
    else if (!startLive(message)) setStatus(message, true);
}

bool DspPanel::setLiveInput(const juce::String& type, const juce::String& file, juce::String& error)
{
    const bool useFile = type.equalsIgnoreCase("file");
    if (useFile && !juce::File(file).existsAsFile())
    {
        error = "Input file not found: " + file;
        return false;
    }
    inputType = useFile ? "File" : "Hardware";
    inputFilePath = file;
    pipeline.setRouting(inputType, file, "Hardware", {});
    error = pipeline.routingError();
    inputBox.setSelectedId(useFile ? 2 : 1, juce::dontSendNotification);
    return error.isEmpty();
}

void DspPanel::chooseInputFile()
{
    chooser = std::make_unique<juce::FileChooser>("Choose an audio file for the live input", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [safe = juce::Component::SafePointer<DspPanel>(this)](const juce::FileChooser& fc) {
            if (safe == nullptr) return;
            if (fc.getResult() == juce::File())
            {
                safe->inputBox.setSelectedId(safe->inputType == "File" ? 2 : 1, juce::dontSendNotification);
                return;
            }
            juce::String error;
            safe->setLiveInput("File", fc.getResult().getFullPathName(), error);
            if (error.isNotEmpty()) safe->setStatus(error, true);
        });
}

DspPanel::RenderResult DspPanel::render(const juce::String& signal, double frequency, double seconds, double amplitude,
                                        double sampleRate, const juce::File& inputFile)
{
    RenderResult result;
    if (!hasEffect()) { result.error = "Compile an effect first."; return result; }
    if (!(sampleRate >= 8000.0 && sampleRate <= 192000.0)) { result.error = "Sample rate must be 8000 to 192000 Hz."; return result; }
    if (!(seconds > 0.0 && seconds <= 30.0)) { result.error = "Duration must be above 0 and at most 30 seconds."; return result; }
    if (!(amplitude >= 0.0 && amplitude <= 4.0)) { result.error = "Amplitude must be 0 to 4 (1.0 is full scale)."; return result; }
    result.sampleRate = sampleRate;
    if (signal.equalsIgnoreCase("file"))
    {
        juce::String error;
        if (!pipeline.readAudioFile(inputFile, result.input, result.sampleRate, error)) { result.error = error; return result; }
        const auto cap = (size_t) (result.sampleRate * 30.0);
        if (result.input.size() > cap) result.input.resize(cap);
    }
    else
    {
        bool known = false;
        const auto kind = signalFrom(signal, known);
        if (!known) { result.error = "Unknown signal '" + signal + "'. Use sine, noise, impulse, sweep, silence or file."; return result; }
        if (!(frequency > 0.0 && frequency < sampleRate * 0.5)) { result.error = "Frequency must be above 0 and below half the sample rate."; return result; }
        result.input = dsp_effect::makeSignal(kind, sampleRate, seconds, frequency, amplitude);
    }
    std::vector<double> values;
    for (int i = 0; i < runtime->paramCount(); ++i)
        values.push_back(runtime->param(i));
    std::string error;
    if (!dsp_effect::renderThrough(runtime->artifact(), result.input, result.sampleRate, values, result.output, error))
    {
        result.error = juce::String(error);
        return result;
    }
    result.inputStats = dsp_effect::measure(result.input);
    result.outputStats = dsp_effect::measure(result.output);
    if (signal.equalsIgnoreCase("sine"))
        result.harmonicDb = dsp_effect::harmonicLevelsDb(result.output, result.sampleRate, frequency, 6);
    result.ok = true;
    lastRenderResult = result;
    return result;
}

void DspPanel::showRenderStats(const RenderResult& result)
{
    if (!result.ok)
    {
        setStatus(result.error, true);
        return;
    }
    auto text = "Rendered " + juce::String((int) result.output.size()) + " samples at " + juce::String(result.sampleRate, 0)
              + " Hz\nin : " + juce::String(result.inputStats.summary()) + "\nout: " + juce::String(result.outputStats.summary());
    if (!result.harmonicDb.empty())
    {
        juce::String line = "\nharmonics (dBFS):";
        for (size_t i = 0; i < result.harmonicDb.size(); ++i)
            line << " " << juce::String((int) i + 1) << "x " << juce::String(result.harmonicDb[i], 1);
        text += line;
    }
    report.setText(text, false);
    setStatus("Render complete.");
    log("DSP effect: " + text.replace("\n", " | "));
}

bool DspPanel::playLastRender(juce::String& error)
{
    if (!lastRenderResult.ok || lastRenderResult.output.empty()) { error = "Render a test signal first."; return false; }
    if (const auto openError = pipeline.open(); openError.isNotEmpty())
    {
        error = "No audio device could be opened: " + openError;
        return false;
    }
    stopLive();
    pipeline.playRenderedAudio(lastRenderResult.output, lastRenderResult.sampleRate, 0.5f, false);
    setStatus("Playing the processed render.");
    return true;
}

bool DspPanel::exportLastRender(const juce::File& wav, bool processed, juce::String& error)
{
    if (!lastRenderResult.ok) { error = "Render a test signal first."; return false; }
    if (!AudioPipeline::exportToWav(wav, processed ? lastRenderResult.output : lastRenderResult.input, lastRenderResult.sampleRate))
    {
        error = "Could not write " + wav.getFullPathName();
        return false;
    }
    setStatus("Saved " + wav.getFullPathName());
    return true;
}

vst_exporter::ExportResult DspPanel::exportVst3(const juce::File& outputFolder)
{
    vst_exporter::ExportResult result;
    if (!hasEffect())
    {
        result.error = "Compile an effect first.";
        return result;
    }
    vst_exporter::ExportRequest request;
    request.artifact = runtime->artifact();
    request.outputDirectory = outputFolder;
    result = vst_exporter::runExport(request);
    log(result.ok ? "VST3 exported: " + result.bundleDirectory.getFullPathName() : "VST3 export failed: " + juce::String(result.error));
    if (result.ok)
        report.setText("Exported " + result.bundleDirectory.getFullPathName() + "\n\n" + juce::String(result.identityLimitation), false);
    return result;
}

juce::var DspPanel::describe() const
{
    auto* root = new juce::DynamicObject();
    root->setProperty("compiling", compiling);
    root->setProperty("compiled", hasEffect());
    root->setProperty("live", live);
    root->setProperty("status", statusText);
    root->setProperty("lastCompileMessage", compileMessage);
    root->setProperty("audioDevice", pipeline.describeDevice());
    root->setProperty("liveInput", inputType + (inputType == "File" ? " " + inputFilePath : juce::String()));
    root->setProperty("vstTemplateAvailable", vst_exporter::templateBundle() != juce::File());
    if (hasEffect())
    {
        const auto& art = runtime->artifact();
        auto* effect = new juce::DynamicObject();
        effect->setProperty("id", juce::String(art.spec.id));
        effect->setProperty("name", juce::String(art.spec.name));
        effect->setProperty("version", juce::String(art.spec.version));
        effect->setProperty("workspaceSize", art.spec.workspaceSize);
        effect->setProperty("nativeCodeBytes", (int) art.object.size());
        effect->setProperty("pluginCode4CC", juce::String(art.pluginCode4CC));
        juce::Array<juce::var> params;
        for (int i = 0; i < runtime->paramCount(); ++i)
        {
            const auto& p = art.spec.params[(size_t) i];
            auto* o = new juce::DynamicObject();
            o->setProperty("id", juce::String(p.id));
            o->setProperty("name", juce::String(p.name));
            o->setProperty("unit", juce::String(p.unit));
            o->setProperty("min", p.minValue);
            o->setProperty("max", p.maxValue);
            o->setProperty("value", runtime->param(i));
            params.add(juce::var(o));
        }
        effect->setProperty("parameters", params);
        root->setProperty("effect", juce::var(effect));
        root->setProperty("divergenceResets", runtime->resets());
    }
    if (lastRenderResult.ok)
    {
        auto* r = new juce::DynamicObject();
        r->setProperty("samples", (int) lastRenderResult.output.size());
        r->setProperty("sampleRate", lastRenderResult.sampleRate);
        r->setProperty("input", statsVar(lastRenderResult.inputStats));
        r->setProperty("output", statsVar(lastRenderResult.outputStats));
        root->setProperty("lastRender", juce::var(r));
    }
    return juce::var(root);
}

void DspPanel::timerCallback()
{
    const float in = pipeline.takeInputPeak(), out = pipeline.takeOutputPeak();
    inLevel = std::max(in, inLevel * 0.85f);
    outLevel = std::max(out, outLevel * 0.85f);
    if (live && hasEffect() && pipeline.isOpen() && std::abs(pipeline.sampleRate() - liveRate) > 1.0)
    {
        // The device was reconfigured: prepare again at its rate with no callback in flight.
        pipeline.setProcessCallback(nullptr);
        liveRate = pipeline.sampleRate();
        runtime->prepare(liveRate, 1);
        pipeline.setProcessCallback([rt = runtime](const float* in, float* out, int n) { rt->process(in, out, n, 0); });
    }
    repaint(sliderView.getX() - 4, sliderView.getY() - 46, sliderView.getWidth() + 8, 46);
}
