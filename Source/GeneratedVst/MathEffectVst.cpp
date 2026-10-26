#include "MathEffectVst.h"

#include <algorithm>
#include <cmath>

namespace math_effect_vst
{
namespace
{
constexpr int kMaxBlock = 8192;
}

juce::File findBundleArtifactFile()
{
    const auto module = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    // <Name>.vst3/Contents/x86_64-win/<Name>.vst3  ->  <Name>.vst3/Contents/Resources/effect.frust-il
    const auto resource = module.getParentDirectory().getParentDirectory().getChildFile("Resources").getChildFile("effect.frust-il");
    if (resource.existsAsFile())
        return resource;
    const auto beside = module.getSiblingFile("effect.frust-il");
    return beside.existsAsFile() ? beside : juce::File();
}

Processor::Loaded Processor::fromBundle()
{
    Loaded out;
    const auto file = findBundleArtifactFile();
    if (file == juce::File())
    {
        out.error = "No effect is packaged with this plugin (Contents/Resources/effect.frust-il is missing).";
        return out;
    }
    out.ok = dsp_effect::loadArtifactFile(file, out.artifact, out.error);
    return out;
}

Processor::Loaded Processor::fromArtifact(const dsp_effect::Artifact& art)
{
    Loaded out;
    // Round-trip through the file format so a test exercises the same validation a bundle gets.
    out.ok = dsp_effect::parseArtifact(dsp_effect::serialize(art), out.artifact, out.error);
    return out;
}

juce::AudioProcessorValueTreeState::ParameterLayout Processor::layoutFor(const Loaded& loaded)
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;
    params.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID("bypass", 1), "Bypass", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("input_gain_db", 1), "Input Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("output_level_db", 1), "Output Level",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.01f), 0.0f, juce::AudioParameterFloatAttributes().withLabel("dB")));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID("mix", 1), "Dry/Wet Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
    if (loaded.ok)
        for (const auto& p : loaded.artifact.spec.params)
        {
            const float lo = (float) p.minValue, hi = (float) p.maxValue;
            params.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID(juce::String(p.id), 1), juce::String(p.name),
                juce::NormalisableRange<float>(lo, hi, 0.0f), juce::jlimit(lo, hi, (float) p.defaultValue),
                juce::AudioParameterFloatAttributes().withLabel(juce::String(p.unit))));
        }
    return { params.begin(), params.end() };
}

Processor::Processor() : Processor(fromBundle()) {}
Processor::Processor(const dsp_effect::Artifact& art) : Processor(fromArtifact(art)) {}

Processor::Processor(Loaded loaded)
    : AudioProcessor(BusesProperties().withInput("Input", juce::AudioChannelSet::stereo(), true)
                                      .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "DJEHUTI_MATH_EFFECT", layoutFor(loaded))
{
    name = loaded.ok ? juce::String(loaded.artifact.spec.name) : juce::String("Djehuti Math Effect");
    bypass = parameters.getRawParameterValue("bypass");
    inputGainDb = parameters.getRawParameterValue("input_gain_db");
    outputGainDb = parameters.getRawParameterValue("output_level_db");
    mix = parameters.getRawParameterValue("mix");
    if (!loaded.ok)
    {
        statusText = "Effect not loaded: " + juce::String(loaded.error);
        return;
    }
    std::string error;
    if (!runtime.load(loaded.artifact, error))
    {
        statusText = "Effect not loaded: " + juce::String(error);
        return;
    }
    for (const auto& p : loaded.artifact.spec.params)
        effectParams.push_back(parameters.getRawParameterValue(juce::String(p.id)));
    valid = true;
    statusText = "Loaded " + name + " " + juce::String(loaded.artifact.spec.version);
}

bool Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return !in.isDisabled() && in == out && in.size() <= dsp_effect::kMaxChannels;
}

void Processor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    runtime.prepare(sampleRate, getTotalNumInputChannels());
    dry.setSize(std::max(1, getTotalNumInputChannels()), std::clamp(samplesPerBlock, 1, kMaxBlock), false, true, true);
    lastInputGain = juce::Decibels::decibelsToGain(inputGainDb->load());
    lastOutputGain = juce::Decibels::decibelsToGain(outputGainDb->load());
    lastMix = mix->load();
    wasBypassed = false;
}

void Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    if (!valid || bypass->load() > 0.5f)
    {
        if (!valid || !wasBypassed)
            runtime.reset();
        wasBypassed = true;
        return; // untouched audio passes through
    }
    wasBypassed = false;
    for (size_t i = 0; i < effectParams.size(); ++i)
        runtime.setParam((int) i, effectParams[i]->load());

    const float inTarget = juce::Decibels::decibelsToGain(inputGainDb->load());
    const float outTarget = juce::Decibels::decibelsToGain(outputGainDb->load());
    const float mixTarget = mix->load();
    const int channels = std::min(buffer.getNumChannels(), dry.getNumChannels());
    const int total = buffer.getNumSamples();
    for (int start = 0; start < total; start += dry.getNumSamples())
    {
        const int n = std::min(dry.getNumSamples(), total - start);
        const float t0 = (float) start / (float) total, t1 = (float) (start + n) / (float) total;
        for (int ch = 0; ch < channels; ++ch)
        {
            float* data = buffer.getWritePointer(ch) + start;
            std::copy(data, data + n, dry.getWritePointer(ch));
            // gains and mix glide across the host block, never step
            const float a0 = lastInputGain + (inTarget - lastInputGain) * t0, a1 = lastInputGain + (inTarget - lastInputGain) * t1;
            for (int i = 0; i < n; ++i)
                data[i] *= a0 + (a1 - a0) * (float) i / (float) n;
            runtime.process(data, n, ch);
            const float g0 = lastOutputGain + (outTarget - lastOutputGain) * t0, g1 = lastOutputGain + (outTarget - lastOutputGain) * t1;
            const float m0 = lastMix + (mixTarget - lastMix) * t0, m1 = lastMix + (mixTarget - lastMix) * t1;
            const float* d = dry.getReadPointer(ch);
            for (int i = 0; i < n; ++i)
            {
                const float f = (float) i / (float) n;
                const float m = m0 + (m1 - m0) * f;
                data[i] = (data[i] * m + d[i] * (1.0f - m)) * (g0 + (g1 - g0) * f);
            }
        }
    }
    lastInputGain = inTarget;
    lastOutputGain = outTarget;
    lastMix = mixTarget;
}

juce::AudioProcessorEditor* Processor::createEditor() { return new juce::GenericAudioProcessorEditor(*this); }

void Processor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void Processor::setStateInformation(const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType()))
            parameters.replaceState(juce::ValueTree::fromXml(*xml));
}
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new math_effect_vst::Processor(); }
