#include "AudioPipeline.h"

#include <algorithm>
#include <cmath>

AudioPipeline::AudioPipeline()
{
    formatManager.registerBasicFormats();
}

AudioPipeline::~AudioPipeline()
{
    close();
}

juce::String AudioPipeline::open()
{
    if (opened)
        return {};
    auto error = deviceManager.initialiseWithDefaultDevices(1, 2);
    if (error.isNotEmpty())
        error = deviceManager.initialise(0, 2, nullptr, true); // no input device: output only
    if (error.isNotEmpty())
        return error;
    deviceManager.addAudioCallback(this);
    opened = true;
    return {};
}

void AudioPipeline::close()
{
    if (!opened)
        return;
    deviceManager.removeAudioCallback(this);
    deviceManager.closeAudioDevice();
    opened = false;
}

bool AudioPipeline::isOpen() const
{
    return opened && deviceManager.getCurrentAudioDevice() != nullptr;
}

juce::String AudioPipeline::describeDevice() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && opened)
        return device->getName() + ", " + juce::String(device->getCurrentSampleRate(), 0) + " Hz, "
             + juce::String(device->getCurrentBufferSizeSamples()) + " samples, "
             + juce::String(device->getActiveInputChannels().countNumberOfSetBits()) + " in / "
             + juce::String(device->getActiveOutputChannels().countNumberOfSetBits()) + " out";
    return "closed";
}

double AudioPipeline::sampleRate() const
{
    if (auto* device = deviceManager.getCurrentAudioDevice(); device != nullptr && opened)
        return device->getCurrentSampleRate();
    return deviceRate;
}

void AudioPipeline::showAudioSettings(juce::Component* centreAround)
{
    const auto error = open();
    if (error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Audio Settings",
                                               "No audio device could be opened:\n" + error);
        return;
    }
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Audio Settings";
    auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 0, 2, 0, 2, false, false, true, false);
    selector->setSize(520, 420);
    options.content.setOwned(selector);
    options.componentToCentreAround = centreAround;
    options.launchAsync();
}

void AudioPipeline::setProcessCallback(ProcessCallback callback)
{
    const juce::ScopedLock lock(callbackLock);
    processCallback = std::move(callback);
}

void AudioPipeline::setRouting(const juce::String& inType, const juce::String& inFile,
                               const juce::String& outType, const juce::String& outFile)
{
    std::unique_ptr<juce::AudioFormatReader> newReader;
    std::unique_ptr<juce::AudioFormatWriter> newWriter;
    juce::String problem;
    const bool hardwareIn = inType != "File";
    const bool hardwareOut = outType != "File";
    if (!hardwareIn)
    {
        const juce::File file(inFile);
        if (!file.existsAsFile())
            problem = "Input file not found: " + inFile;
        else if ((newReader.reset(formatManager.createReaderFor(file)), newReader == nullptr))
            problem = "Input file is not a readable audio format: " + inFile;
    }
    if (!hardwareOut && problem.isEmpty())
    {
        const juce::File file(outFile);
        if (outFile.isEmpty() || !file.getParentDirectory().isDirectory())
            problem = "Output file folder does not exist: " + outFile;
        else
        {
            file.deleteFile();
            if (auto* wav = formatManager.findFormatForFileExtension("wav"))
                newWriter.reset(wav->createWriterFor(new juce::FileOutputStream(file), sampleRate(), 1, 16, {}, 0));
            if (newWriter == nullptr)
                problem = "Could not create the output file: " + outFile;
        }
    }
    const juce::ScopedLock lock(callbackLock);
    reader = std::move(newReader);
    writer = std::move(newWriter);
    currentReadPosition = 0;
    useHardwareIn = hardwareIn || problem.isNotEmpty();
    useHardwareOut = hardwareOut || problem.isNotEmpty();
    routingProblem = problem;
}

juce::String AudioPipeline::routingError() const
{
    const juce::ScopedLock lock(callbackLock);
    return routingProblem;
}

void AudioPipeline::raise(std::atomic<float>& peak, const float* data, int count)
{
    float value = 0.0f;
    for (int i = 0; i < count; ++i)
        value = std::max(value, std::abs(data[i]));
    float seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

float AudioPipeline::takeInputPeak() { return inPeak.exchange(0.0f); }
float AudioPipeline::takeOutputPeak() { return outPeak.exchange(0.0f); }

void AudioPipeline::processChunk(const float* hardwareInput, int numInputChannels, float* const* outputChannelData,
                                 int numOutputChannels, int offset, int count)
{
    juce::ignoreUnused(numInputChannels);
    // No effect is attached: stay silent. Passing the input through would play a live microphone
    // (feedback) or a looping file after the user stopped the effect.
    if (!processCallback)
        return;
    const float* input = hardwareInput != nullptr ? hardwareInput + offset : nullptr;
    if (!useHardwareIn && reader != nullptr)
    {
        float* destination[] = { inputScratch.data() };
        juce::AudioBuffer<float> buffer(destination, 1, count);
        int done = 0;
        while (done < count && reader->lengthInSamples > 0)
        {
            const auto left = reader->lengthInSamples - currentReadPosition;
            if (left <= 0) { currentReadPosition = 0; continue; }
            const int n = (int) std::min<juce::int64>(count - done, left);
            reader->read(&buffer, done, n, currentReadPosition, true, true);
            currentReadPosition += n;
            done += n;
        }
        input = inputScratch.data();
    }
    if (input == nullptr)
    {
        std::fill(inputScratch.begin(), inputScratch.begin() + count, 0.0f);
        input = inputScratch.data();
    }
    raise(inPeak, input, count);

    float* output = outputScratch.data();
    if (processCallback)
        processCallback(input, output, count);
    else
        std::copy(input, input + count, output);
    raise(outPeak, output, count);

    if (!useHardwareOut && writer != nullptr)
    {
        const float* source[] = { output };
        writer->writeFromFloatArrays(source, 1, count);
        return; // file output: the hardware stays silent
    }
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            std::copy(output, output + count, outputChannelData[ch] + offset);
}

void AudioPipeline::audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                                     float* const* outputChannelData, int numOutputChannels,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);

    const juce::ScopedLock lock(callbackLock);
    if (playing.load())
    {
        const double step = playbackRate / std::max(1.0, deviceRate);
        const auto total = (double) playbackSamples.size();
        for (int s = 0; s < numSamples; ++s)
        {
            if (playbackPosition >= total - 1.0)
            {
                playing.store(false);
                break;
            }
            const auto i = (size_t) playbackPosition;
            const float frac = (float) (playbackPosition - (double) i);
            const float value = juce::jlimit(-1.0f, 1.0f, (playbackSamples[i] * (1.0f - frac) + playbackSamples[i + 1] * frac) * playbackGain);
            playbackPosition += step;
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    outputChannelData[ch][s] = value;
        }
        return;
    }
    if (scratchCapacity <= 0)
        return;
    const float* hardwareInput = numInputChannels > 0 ? inputChannelData[0] : nullptr;
    for (int offset = 0; offset < numSamples; offset += scratchCapacity)
        processChunk(hardwareInput, numInputChannels, outputChannelData, numOutputChannels, offset,
                     std::min(scratchCapacity, numSamples - offset));
}

void AudioPipeline::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const juce::ScopedLock lock(callbackLock);
    deviceRate = device->getCurrentSampleRate();
    scratchCapacity = std::max(64, device->getCurrentBufferSizeSamples());
    inputScratch.assign((size_t) scratchCapacity, 0.0f);
    outputScratch.assign((size_t) scratchCapacity, 0.0f);
}

void AudioPipeline::prepareOffline(double rate, int maxBlock)
{
    const juce::ScopedLock lock(callbackLock);
    deviceRate = rate;
    scratchCapacity = std::max(16, maxBlock);
    inputScratch.assign((size_t) scratchCapacity, 0.0f);
    outputScratch.assign((size_t) scratchCapacity, 0.0f);
}

void AudioPipeline::audioDeviceStopped()
{
    const juce::ScopedLock lock(callbackLock);
    scratchCapacity = 0;
    playing.store(false);
}

void AudioPipeline::playRenderedAudio(const std::vector<float>& samples, double rate, float volume, bool condition)
{
    if (samples.size() < 2)
    {
        stopRenderedAudio();
        return;
    }
    std::vector<float> prepared = samples;
    if (condition)
    {
        double sum = 0.0;
        for (float v : prepared)
            sum += v;
        const auto mean = (float) (sum / (double) prepared.size());
        float peak = 0.0f;
        for (auto& v : prepared) { v -= mean; peak = std::max(peak, std::abs(v)); }
        if (peak > 1.0e-6f)
            for (auto& v : prepared) v *= 0.85f / peak;
    }
    const juce::ScopedLock lock(callbackLock);
    playbackSamples = std::move(prepared);
    playbackRate = rate > 0.0 ? rate : 48000.0;
    playbackPosition = 0.0;
    playbackGain = volume;
    playing.store(true);
}

void AudioPipeline::stopRenderedAudio() { playing.store(false); }
bool AudioPipeline::isPlayingRenderedAudio() const { return playing.load(); }

bool AudioPipeline::exportToWav(const juce::File& file, const std::vector<float>& samples, double sampleRate)
{
    if (samples.empty())
        return false;
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> out(wav.createWriterFor(new juce::FileOutputStream(file),
                                                                   sampleRate > 0.0 ? sampleRate : 48000.0, 1, 16, {}, 0));
    if (out == nullptr)
        return false;
    float peak = 0.0f;
    for (float v : samples)
        peak = std::max(peak, std::abs(v));
    const float scale = peak > 1.0f ? 0.95f / peak : 1.0f; // never clip the file; leave quieter audio alone
    std::vector<float> scaled(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        scaled[i] = samples[i] * scale;
    const float* channels[] = { scaled.data() };
    return out->writeFromFloatArrays(channels, 1, (int) scaled.size());
}

bool AudioPipeline::readAudioFile(const juce::File& file, std::vector<float>& samples, double& rate, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "Audio file not found: " + file.getFullPathName();
        return false;
    }
    std::unique_ptr<juce::AudioFormatReader> source(formatManager.createReaderFor(file));
    if (source == nullptr || source->lengthInSamples <= 0 || source->lengthInSamples > 48000LL * 600)
    {
        error = "Not a readable audio file of at most ten minutes: " + file.getFileName();
        return false;
    }
    const int n = (int) source->lengthInSamples;
    juce::AudioBuffer<float> buffer((int) std::max<unsigned>(1, source->numChannels), n);
    source->read(&buffer, 0, n, 0, true, true);
    samples.assign((size_t) n, 0.0f);
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < n; ++i)
            samples[(size_t) i] += buffer.getSample(ch, i) / (float) buffer.getNumChannels();
    rate = source->sampleRate;
    return true;
}
