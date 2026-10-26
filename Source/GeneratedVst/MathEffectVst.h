#pragma once

// The VST3 runtime shared by every Djehuti Math effect export. The exporter
// copies this plugin's binary and supplies the effect in
// <bundle>/Contents/Resources/effect.frust-il; the plugin links that native
// object through the FRust plugin host and runs it per sample and channel.

#include <JuceHeader.h>

#include "../DspEffect.h"

#include <atomic>
#include <memory>
#include <vector>

namespace math_effect_vst
{
// <bundle>/Contents/Resources/effect.frust-il, or effect.frust-il beside the binary.
juce::File findBundleArtifactFile();

class Processor final : public juce::AudioProcessor
{
public:
    Processor();                                         // from the bundle
    explicit Processor(const dsp_effect::Artifact& art); // from a loaded artifact (tests)
    ~Processor() override = default;

    const juce::String getName() const override { return name; }
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return name; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool isValid() const noexcept { return valid; }
    juce::String status() const { return statusText; }
    juce::AudioProcessorValueTreeState& state() { return parameters; }
    int divergenceResets() const noexcept { return runtime.resets(); }

private:
    struct Loaded
    {
        bool ok = false;
        std::string error;
        dsp_effect::Artifact artifact;
    };
    static Loaded fromBundle();
    static Loaded fromArtifact(const dsp_effect::Artifact& art);
    explicit Processor(Loaded loaded);
    static juce::AudioProcessorValueTreeState::ParameterLayout layoutFor(const Loaded& loaded);

    juce::String name;
    bool valid = false;
    juce::String statusText;
    juce::AudioProcessorValueTreeState parameters;
    dsp_effect::Runtime runtime;
    std::vector<std::atomic<float>*> effectParams;
    std::atomic<float>* bypass = nullptr;
    std::atomic<float>* inputGainDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* mix = nullptr;
    juce::AudioBuffer<float> dry;
    float lastInputGain = 1.0f, lastOutputGain = 1.0f, lastMix = 1.0f;
    bool wasBypassed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Processor)
};
}
