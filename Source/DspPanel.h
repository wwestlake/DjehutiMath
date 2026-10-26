#pragma once

// The DSP workbench: author an audio effect in FRust, compile it to native
// code, hear it live through the audio pipeline, render and measure test
// signals offline, and export it as a VST3. Every operation is public so the
// agent tools (the same command service) drive exactly what the buttons do.
// Compilation runs on a background thread; nothing here blocks the UI thread
// for longer than a block of audio.

#include <JuceHeader.h>

#include "AudioPipeline.h"
#include "DspEffect.h"
#include "VstExporter.h"

#include <functional>
#include <memory>
#include <vector>

class DspPanel final : public juce::Component, private juce::Timer
{
public:
    explicit DspPanel(AudioPipeline& pipeline);
    ~DspPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // ---- source ---------------------------------------------------------
    void setSource(const juce::String& source);
    juce::String source() const;
    // Replaces the source with a starter ("gain", "softclip", "lowpass", "tremolo").
    bool loadStarter(const juce::String& kind, juce::String& error);

    // ---- compile --------------------------------------------------------
    // Parses and compiles on a worker, then links the native code. `done`
    // runs on the message thread with the outcome.
    void compile(std::function<void(bool ok, const juce::String& message)> done = {});
    bool isCompiling() const { return compiling; }
    bool hasEffect() const { return runtime != nullptr && runtime->isLoaded(); }
    const dsp_effect::Artifact* artifact() const { return hasEffect() ? &runtime->artifact() : nullptr; }
    juce::String lastCompileMessage() const { return compileMessage; }

    // ---- parameters -----------------------------------------------------
    bool setParameter(const juce::String& id, double value, juce::String& error);

    // ---- live audio -----------------------------------------------------
    bool startLive(juce::String& message);
    void stopLive();
    bool isLive() const { return live; }
    // Input: hardware or a looping audio file. Output is the device.
    bool setLiveInput(const juce::String& type, const juce::String& file, juce::String& error);

    // ---- offline render and measurement ------------------------------------
    struct RenderResult
    {
        bool ok = false;
        juce::String error;
        double sampleRate = 48000.0;
        std::vector<float> input, output;
        dsp_effect::Measurement inputStats, outputStats;
        std::vector<double> harmonicDb;   // output level at 1x, 2x, ... the test frequency (sine tests)
    };
    // signal: sine, noise, impulse, sweep, silence, or file (inputFile).
    RenderResult render(const juce::String& signal, double frequency, double seconds, double amplitude,
                        double sampleRate, const juce::File& inputFile = {});
    const RenderResult& lastRender() const { return lastRenderResult; }
    bool playLastRender(juce::String& error);
    bool exportLastRender(const juce::File& wav, bool processed, juce::String& error);

    // ---- VST3 -----------------------------------------------------------
    vst_exporter::ExportResult exportVst3(const juce::File& outputFolder);

    // ---- inspection -----------------------------------------------------
    juce::var describe() const;
    juce::String statusLine() const { return statusText; }
    std::function<void(const juce::String&)> onLog;   // one-line events for the Diagnostics panel

private:
    void timerCallback() override;
    void log(const juce::String& text);
    void setStatus(const juce::String& text, bool error = false);
    void rebuildSliders();
    void applyCompiled(const dsp_effect::Spec& spec, dsp_effect::Compiled compiled,
                       std::function<void(bool, const juce::String&)> done);
    void toggleLive();
    void chooseInputFile();
    void showRenderStats(const RenderResult& result);

    AudioPipeline& pipeline;
    juce::CodeDocument document;
    juce::CPlusPlusCodeTokeniser tokeniser;
    std::unique_ptr<juce::CodeEditorComponent> editor;
    juce::ComboBox starterBox, signalBox, inputBox;
    juce::TextButton compileButton { "Compile" }, liveButton { "Start Live" }, audioButton { "Audio Settings..." },
        fileButton { "Input File..." }, renderButton { "Render Test" }, playButton { "Play Render" },
        wavButton { "Save WAV..." }, vstButton { "Export VST3..." };
    juce::Label starterLabel { {}, "Template" }, signalLabel { {}, "Test signal" }, inputLabel { {}, "Live input" },
        status, meterLabel { {}, "Levels" };
    juce::TextEditor report;
    struct Slider
    {
        std::string id;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::Slider> slider;
    };
    std::vector<Slider> sliders;
    juce::Component sliderHolder;
    juce::Viewport sliderView;
    float inLevel = 0.0f, outLevel = 0.0f;

    std::shared_ptr<dsp_effect::Runtime> runtime;
    std::uint64_t compileGeneration = 0;
    bool compiling = false;
    bool live = false;
    double liveRate = 48000.0;
    juce::String compileMessage, statusText;
    juce::String inputType = "Hardware", inputFilePath;
    RenderResult lastRenderResult;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DspPanel)
};
