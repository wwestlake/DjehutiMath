#pragma once

// Djehuti Math audio effects. An effect is FRust source defining
//
//   pub fn process_sample(audio_in: f64, ws: Array<f64, W>, coeffs: Array<f64, P + 1>) -> f64
//
// called once per sample and channel. `ws` is that channel's persistent state
// (W entries, zero at start), `coeffs[0..P)` are the declared parameters in
// order and `coeffs[P]` is the sample rate. The source declares itself with
// comment directives:
//
//   // @effect Soft Clip
//   // @id com.example.softclip            (optional; identity for packaging)
//   // @param drive "Drive" 1 20 4 x       (id "Name" min max default [unit])
//   // @workspace 1
//
// The same contract is the target of the future Math-to-FRust compiler: a
// frontend produces a Spec (see EffectFrontend) and everything downstream -
// native compilation, live audition, offline rendering, the executable
// artifact and VST3 export - is already in place.
//
// This file is shared by the application and the generated VST3 plugin, so it
// depends only on JUCE core and the FRust engine.

#include "FrustEngine.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsp_effect
{
constexpr int kMaxParams = 64;
constexpr int kMaxWorkspace = 65536;
constexpr int kMaxChannels = 8;
inline constexpr const char* kArtifactMagic = "DJEHUTI_MATH_EFFECT_IL";
inline constexpr int kArtifactSchema = 1;
inline constexpr const char* kTargetArch = "x86_64-pc-windows-msvc";
inline constexpr const char* kObjectFormat = "COFF-x86_64";
inline constexpr const char* kEntryPoint = "process_sample";

struct Param
{
    std::string id;
    std::string name;
    std::string unit;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
};

struct Spec
{
    std::string id = "com.djehuti.math.effect";
    std::string name = "Djehuti Math Effect";
    std::string version = "1.0.0";
    std::string manufacturer = "Djehuti Math";
    std::vector<Param> params;
    int workspaceSize = 1;
    std::string source;   // complete FRust source, directives included
};

// Reads the directives and checks the process_sample signature against them.
// False with `error` (one line, with the source line number) when unusable.
bool parseSpec(const std::string& source, Spec& spec, std::string& error);

// Starter effects: "gain", "softclip", "lowpass", "tremolo".
std::vector<std::string> starterKinds();
std::string starterSource(const std::string& kind);

// The names of the host math functions an effect may declare with `extern fn`
// (exp, log, sin, cos, tan, tanh, atan, pow, sqrt). All are registered, and
// those the source does not declare itself are declared for it.
const std::vector<std::string>& hostFunctionNames();

// The complete unit that is compiled: manifest line, extern declarations the
// source lacks, then the source. `prefixLines` receives the lines added in front.
std::string unitSource(const Spec& spec, int* prefixLines = nullptr);

struct Compiled
{
    bool ok = false;
    std::string error;                           // report() style text when !ok
    std::vector<frust_engine::Diagnostic> diagnostics; // lines refer to the user's source
    std::vector<std::uint8_t> object;            // native x64 COFF
    std::string manifest;                        // plugin manifest JSON
    double compileMs = 0.0;
};

Compiled compile(const Spec& spec);
// Syntax and type check only (no object code).
Compiled check(const Spec& spec);

struct Artifact
{
    Spec spec;                                    // source kept for reference/debugging
    std::string pluginCode4CC;
    std::string vst3UidHex;
    std::string manifest;
    std::vector<std::uint8_t> object;
    std::string objectHash;
    std::string integrityHash;
};

Artifact makeArtifact(const Spec& spec, const Compiled& compiled);
juce::String serialize(const Artifact& artifact, bool includeSource = true);
// Strict: rejects a wrong magic/schema/architecture, an empty object, bad
// parameters and any hash mismatch.
bool parseArtifact(const juce::String& json, Artifact& artifact, std::string& error);
bool loadArtifactFile(const juce::File& file, Artifact& artifact, std::string& error);

std::string hashHex(const void* data, std::size_t size);
std::string derivePluginCode4CC(const std::string& packageId);
std::string deriveVst3UidHex(const std::string& packageId);

// Runs an artifact's native code. process() is real-time safe (no allocation,
// locking or logging); load/prepare are not.
class Runtime
{
public:
    Runtime();
    ~Runtime();
    bool load(const Artifact& artifact, std::string& error);
    bool isLoaded() const { return fn != nullptr; }
    void prepare(double sampleRate, int channels);   // clears all state
    void reset();                                    // clears state, keeps parameters
    int paramCount() const { return (int) values.size(); }
    void setParam(int index, double value);          // clamped to the declared range
    double param(int index) const;
    // In place on one channel. A non-finite result resets that channel's state
    // and outputs silence for the sample; resets() counts them.
    void process(float* data, int numSamples, int channel) noexcept;
    // Input to output; the buffers may be the same.
    void process(const float* input, float* output, int numSamples, int channel) noexcept;
    int resets() const noexcept { return resetCount.load(std::memory_order_relaxed); }
    const Artifact& artifact() const { return loaded; }

private:
    using ProcessFn = double (*)(double, double*, const double*);
    Artifact loaded;
    frust_engine::Engine engine;
    ProcessFn fn = nullptr;
    double sampleRate = 48000.0;
    int channelCount = 1;
    int paramTotal = 0;
    std::vector<std::unique_ptr<std::atomic<double>>> values;
    std::vector<double> workspace;      // channelCount x workspaceSize
    std::vector<double> coeffScratch;   // channelCount x (paramTotal + 1)
    std::atomic<int> resetCount { 0 };
};

// Offline test signals and measurements.
enum class Signal { Sine, Noise, Impulse, Sweep, Silence };
std::vector<float> makeSignal(Signal kind, double sampleRate, double seconds, double frequency, double amplitude);
struct Measurement
{
    double peak = 0.0, rms = 0.0, dc = 0.0;
    int nonFinite = 0;
    int clipped = 0;       // samples at or beyond full scale
    std::string summary() const;
};
Measurement measure(const std::vector<float>& samples);
// Level (dBFS, amplitude) of the signal at f0, 2*f0, ... below Nyquist, by windowed Goertzel
// (about +-1.5 dB scalloping). For a sine test this shows the harmonics an effect adds.
std::vector<double> harmonicLevelsDb(const std::vector<float>& samples, double sampleRate, double f0, int count);
// Renders a signal through an artifact (mono). Returns false with `error` if it won't load.
bool renderThrough(const Artifact& artifact, const std::vector<float>& input, double sampleRate,
                   const std::vector<double>& paramValues, std::vector<float>& output, std::string& error);

// Future frontends (e.g. Math-to-FRust) turn authored content into an effect.
class EffectFrontend
{
public:
    virtual ~EffectFrontend() = default;
    virtual std::string name() const = 0;                    // "Math"
    virtual bool available(std::string& whyNot) const = 0;
    virtual bool toSpec(const juce::String& authoredContent, Spec& spec, std::string& error) = 0;
};
void registerFrontend(std::shared_ptr<EffectFrontend> frontend);
std::vector<std::shared_ptr<EffectFrontend>> frontends();
}
