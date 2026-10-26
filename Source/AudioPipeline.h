#pragma once

// Audio device access for DSP development: hardware or file input, a mono
// process callback, hardware or WAV-file output, level meters, and audition
// playback of rendered audio. Adapted from DEWB's AudioPipeline for Djehuti
// Math: the device opens on demand (never at application start), the callback
// is chunk-safe for any host buffer size, playback is resampled to the device
// rate, and rendered audio is played as it is unless conditioning is asked for.
// With no process callback the outputs are silent (never a pass-through).

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class AudioPipeline final : public juce::AudioIODeviceCallback
{
public:
    AudioPipeline();
    ~AudioPipeline() override;

    // Opens the default devices (1 input, 2 outputs; output only when there is
    // no input device). Message thread. Returns "" on success or the reason.
    juce::String open();
    void close();
    bool isOpen() const;
    // "Device, rate, buffer, channels" for status lines; "closed" when not open.
    juce::String describeDevice() const;
    double sampleRate() const;
    void showAudioSettings(juce::Component* centreAround);

    // The mono DSP callback (audio thread). Replacing it waits for the block
    // in flight; nullptr silences the output.
    using ProcessCallback = std::function<void(const float* input, float* output, int numSamples)>;
    void setProcessCallback(ProcessCallback callback);

    // Routing: inType/outType "Hardware" or "File". A file input loops; a file
    // output writes a 16-bit mono WAV at the device rate.
    void setRouting(const juce::String& inType, const juce::String& inFile,
                    const juce::String& outType, const juce::String& outFile);
    juce::String routingError() const;

    // Peak levels since the last call (0..1+), for meters.
    float takeInputPeak();
    float takeOutputPeak();

    // Audition playback of rendered audio (mono, at sampleRate). Resampled to
    // the device rate. conditionForAudition removes DC and normalises to 0.85.
    void playRenderedAudio(const std::vector<float>& samples, double sampleRate, float volume = 0.5f,
                           bool conditionForAudition = false);
    void stopRenderedAudio();
    bool isPlayingRenderedAudio() const;

    static bool exportToWav(const juce::File& file, const std::vector<float>& samples, double sampleRate);
    // Reads a mono mix of any registered format. False with `error` on failure.
    bool readAudioFile(const juce::File& file, std::vector<float>& samples, double& rate, juce::String& error);

    // Prepares the callback buffers without a device (offline checks drive the callback directly).
    void prepareOffline(double rate, int maxBlock);

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

private:
    void processChunk(const float* input, int numInputChannels, float* const* outputChannelData,
                      int numOutputChannels, int offset, int count);
    static void raise(std::atomic<float>& peak, const float* data, int count);

    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager formatManager;
    mutable juce::CriticalSection callbackLock;
    ProcessCallback processCallback;
    std::unique_ptr<juce::AudioFormatReader> reader;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 currentReadPosition = 0;
    bool useHardwareIn = true;
    bool useHardwareOut = true;
    juce::String routingProblem;
    bool opened = false;
    double deviceRate = 48000.0;

    std::vector<float> inputScratch;   // mono input for one chunk
    std::vector<float> outputScratch;  // mono output for one chunk
    int scratchCapacity = 0;

    std::vector<float> playbackSamples;
    double playbackRate = 48000.0;
    double playbackPosition = 0.0;
    float playbackGain = 0.5f;
    std::atomic<bool> playing { false };

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };
};
