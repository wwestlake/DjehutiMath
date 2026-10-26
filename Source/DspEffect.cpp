#include "DspEffect.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <regex>
#include <sstream>

namespace dsp_effect
{
namespace
{
struct HostFunction { const char* name; int arguments; };
const HostFunction kHosts[] = {
    { "djehuti_dsp_exp", 1 }, { "djehuti_dsp_log", 1 }, { "djehuti_dsp_sin", 1 }, { "djehuti_dsp_cos", 1 },
    { "djehuti_dsp_tan", 1 }, { "djehuti_dsp_tanh", 1 }, { "djehuti_dsp_atan", 1 }, { "djehuti_dsp_pow", 2 },
    { "djehuti_dsp_sqrt", 1 },
};

std::uint64_t fnv1a(const void* data, std::size_t size)
{
    std::uint64_t h = 1469598103934665603ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(std::uint64_t value)
{
    static const char* digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i, value >>= 4)
        out[(size_t) i] = digits[value & 0xf];
    return out;
}

std::string trim(const std::string& text)
{
    const auto a = text.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    return text.substr(a, text.find_last_not_of(" \t\r") - a + 1);
}

// Splits on spaces, keeping a "quoted string" as one token (quotes removed).
std::vector<std::string> tokens(const std::string& text)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
            ++i;
        if (i >= text.size())
            break;
        if (text[i] == '"')
        {
            const auto end = text.find('"', i + 1);
            out.push_back(text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
            i = end == std::string::npos ? text.size() : end + 1;
        }
        else
        {
            const auto end = text.find_first_of(" \t", i);
            out.push_back(text.substr(i, end == std::string::npos ? std::string::npos : end - i));
            i = end == std::string::npos ? text.size() : end;
        }
    }
    return out;
}

bool toNumber(const std::string& text, double& value)
{
    try
    {
        std::size_t used = 0;
        value = std::stod(text, &used);
        return used == text.size() && std::isfinite(value);
    }
    catch (...) { return false; }
}

bool validIdentifier(const std::string& id)
{
    if (id.empty() || id.size() > 48 || !(std::isalpha((unsigned char) id[0]) || id[0] == '_'))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char) c) || c == '_'; });
}

std::string sanitizedName(const std::string& text)
{
    std::string out;
    for (const char c : text)
        out += (std::isalnum((unsigned char) c) || c == '_') ? c : '_';
    return out.empty() ? "effect" : out;
}

std::vector<frust_engine::Diagnostic> shifted(std::vector<frust_engine::Diagnostic> list, int prefixLines)
{
    for (auto& d : list)
        if (d.line > prefixLines)
            d.line -= prefixLines;
    return list;
}

std::vector<std::shared_ptr<EffectFrontend>>& frontendList()
{
    static std::vector<std::shared_ptr<EffectFrontend>> list;
    return list;
}
std::mutex& frontendMutex() { static std::mutex m; return m; }
}

std::string hashHex(const void* data, std::size_t size) { return hex64(fnv1a(data, size)); }

std::string derivePluginCode4CC(const std::string& packageId)
{
    const auto h = fnv1a(packageId.data(), packageId.size());
    static constexpr const char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    constexpr std::size_t count = sizeof(chars) - 1;
    std::string code = "D";
    code.push_back(chars[(h >> 8) % 26]);
    code.push_back(chars[(h >> 20) % count]);
    code.push_back(chars[(h >> 32) % count]);
    return code;
}

std::string deriveVst3UidHex(const std::string& packageId)
{
    const std::string a = "djehuti.math.vst3.uid.a:" + packageId, b = "djehuti.math.vst3.uid.b:" + packageId;
    return hashHex(a.data(), a.size()) + hashHex(b.data(), b.size());
}

bool parseSpec(const std::string& source, Spec& spec, std::string& error)
{
    Spec out;
    out.source = source;
    bool workspaceDeclared = false;
    int declaredWorkspace = 0, lineNumber = 0;
    std::istringstream lines(source);
    for (std::string raw; std::getline(lines, raw);)
    {
        ++lineNumber;
        const auto line = trim(raw);
        if (line.rfind("//", 0) != 0)
            continue;
        const auto body = trim(line.substr(2));
        if (body.empty() || body[0] != '@')
            continue;
        const auto space = body.find_first_of(" \t");
        const auto key = body.substr(1, space == std::string::npos ? std::string::npos : space - 1);
        const auto rest = space == std::string::npos ? std::string() : trim(body.substr(space));
        const auto at = [&](const std::string& why) { return "line " + std::to_string(lineNumber) + ": " + why; };
        if (key == "effect") { if (rest.empty()) { error = at("@effect needs a name"); return false; } out.name = rest; }
        else if (key == "id") { if (rest.empty() || rest.find(' ') != std::string::npos) { error = at("@id must be one word, such as com.example.softclip"); return false; } out.id = rest; }
        else if (key == "version") { out.version = rest; }
        else if (key == "manufacturer") { out.manufacturer = rest; }
        else if (key == "workspace")
        {
            double v = 0;
            if (!toNumber(rest, v) || v < 1 || v > kMaxWorkspace || v != std::floor(v))
            {
                error = at("@workspace must be a whole number from 1 to " + std::to_string(kMaxWorkspace));
                return false;
            }
            workspaceDeclared = true;
            declaredWorkspace = (int) v;
        }
        else if (key == "param")
        {
            const auto t = tokens(rest);
            Param p;
            if (t.size() < 5 || t.size() > 6 || !toNumber(t[2], p.minValue) || !toNumber(t[3], p.maxValue) || !toNumber(t[4], p.defaultValue))
            {
                error = at("@param needs: id \"Name\" min max default [unit]");
                return false;
            }
            p.id = t[0];
            p.name = t[1];
            if (t.size() == 6) p.unit = t[5];
            if (!validIdentifier(p.id)) { error = at("parameter id '" + p.id + "' must be letters, digits and underscores"); return false; }
            if (p.name.empty()) { error = at("parameter '" + p.id + "' needs a display name"); return false; }
            if (p.id == "bypass" || p.id == "input_gain_db" || p.id == "output_level_db" || p.id == "mix")
            {
                error = at("parameter id '" + p.id + "' is reserved by the plugin wrapper (bypass, input_gain_db, output_level_db, mix)");
                return false;
            }
            if (p.minValue >= p.maxValue) { error = at("parameter '" + p.id + "' needs min < max"); return false; }
            if (p.defaultValue < p.minValue || p.defaultValue > p.maxValue) { error = at("parameter '" + p.id + "' default is outside min..max"); return false; }
            for (const auto& other : out.params)
                if (other.id == p.id) { error = at("parameter id '" + p.id + "' is declared twice"); return false; }
            if ((int) out.params.size() >= kMaxParams) { error = at("at most " + std::to_string(kMaxParams) + " parameters"); return false; }
            out.params.push_back(std::move(p));
        }
        // unknown @keys are ignored so comments such as @author stay harmless
    }

    static const std::regex signature(
        R"(pub\s+fn\s+process_sample\s*\(\s*audio_in\s*:\s*f64\s*,\s*ws\s*:\s*Array\s*<\s*f64\s*,\s*(\d+)\s*>\s*,\s*coeffs\s*:\s*Array\s*<\s*f64\s*,\s*(\d+)\s*>\s*\)\s*->\s*f64)");
    std::smatch match;
    if (!std::regex_search(source, match, signature))
    {
        error = "No usable process_sample. Define: pub fn process_sample(audio_in: f64, ws: Array<f64, W>, coeffs: Array<f64, P+1>) -> f64";
        return false;
    }
    const int signatureWorkspace = std::stoi(match[1].str());
    const int signatureCoeffs = std::stoi(match[2].str());
    out.workspaceSize = workspaceDeclared ? declaredWorkspace : signatureWorkspace;
    if (signatureWorkspace != out.workspaceSize)
    {
        error = "ws is Array<f64, " + std::to_string(signatureWorkspace) + "> but @workspace says " + std::to_string(out.workspaceSize);
        return false;
    }
    if (out.workspaceSize < 1 || out.workspaceSize > kMaxWorkspace)
    {
        error = "workspace must be 1 to " + std::to_string(kMaxWorkspace);
        return false;
    }
    if (signatureCoeffs != (int) out.params.size() + 1)
    {
        error = "coeffs is Array<f64, " + std::to_string(signatureCoeffs) + "> but " + std::to_string(out.params.size())
              + " parameter(s) are declared; it must be " + std::to_string(out.params.size() + 1)
              + " (the parameters, then the sample rate)";
        return false;
    }
    spec = std::move(out);
    return true;
}

std::vector<std::string> starterKinds() { return { "gain", "softclip", "lowpass", "tremolo" }; }

std::string starterSource(const std::string& kind)
{
    if (kind == "softclip")
        return "// @effect Soft Clip\n// @id com.djehuti.math.softclip\n// @param drive \"Drive\" 1 20 4 x\n"
               "// @param level \"Level\" 0 1 0.7\n// @workspace 1\n"
               "pub fn process_sample(audio_in: f64, ws: Array<f64, 1>, coeffs: Array<f64, 3>) -> f64 = {\n"
               "    djehuti_dsp_tanh(audio_in * coeffs[0]) * coeffs[1]\n}\n";
    if (kind == "lowpass")
        return "// @effect One-Pole Low-Pass\n// @id com.djehuti.math.lowpass\n// @param cutoff \"Cutoff\" 20 20000 1000 Hz\n"
               "// @workspace 1\n"
               "pub fn process_sample(audio_in: f64, ws: Array<f64, 1>, coeffs: Array<f64, 2>) -> f64 = {\n"
               "    let a = 1.0 - djehuti_dsp_exp((0.0 - 6.283185307179586) * coeffs[0] / coeffs[1]);\n"
               "    let y = ws[0] + a * (audio_in - ws[0]);\n"
               "    ws[0] = y;\n"
               "    y\n}\n";
    if (kind == "tremolo")
        return "// @effect Tremolo\n// @id com.djehuti.math.tremolo\n// @param rate \"Rate\" 0.1 20 4 Hz\n"
               "// @param depth \"Depth\" 0 1 0.6\n// @workspace 1\n"
               "pub fn process_sample(audio_in: f64, ws: Array<f64, 1>, coeffs: Array<f64, 3>) -> f64 = {\n"
               "    let two_pi = 6.283185307179586;\n"
               "    let gain = 1.0 - coeffs[1] * 0.5 * (1.0 - djehuti_dsp_sin(ws[0]));\n"
               "    let mut next = ws[0] + two_pi * coeffs[0] / coeffs[2];\n"
               "    if (next > two_pi) { next = next - two_pi; };\n"
               "    ws[0] = next;\n"
               "    audio_in * gain\n}\n";
    return "// @effect Gain\n// @id com.djehuti.math.gain\n// @param gain \"Gain\" 0 2 1 x\n// @workspace 1\n"
           "pub fn process_sample(audio_in: f64, ws: Array<f64, 1>, coeffs: Array<f64, 2>) -> f64 = {\n"
           "    audio_in * coeffs[0]\n}\n";
}

const std::vector<std::string>& hostFunctionNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto& h : kHosts) n.push_back(h.name);
        return n;
    }();
    return names;
}

std::string unitSource(const Spec& spec, int* prefixLines)
{
    std::string prefix;
    for (const auto& h : kHosts)
    {
        if (spec.source.find(std::string("extern fn ") + h.name) != std::string::npos)
            continue;
        prefix += std::string("extern fn ") + h.name + (h.arguments == 2 ? "(x: f64, y: f64) -> f64;\n" : "(x: f64) -> f64;\n");
    }
    if (prefixLines != nullptr)
        *prefixLines = (int) std::count(prefix.begin(), prefix.end(), '\n');
    return prefix + spec.source;
}

static std::string runtimeManifest(const Spec& spec)
{
    return frust_engine::manifestJson("mathfx_" + sanitizedName(spec.id), spec.name, hostFunctionNames(), { kEntryPoint });
}

Compiled check(const Spec& spec)
{
    Compiled out;
    int prefix = 0;
    const auto unit = unitSource(spec, &prefix);
    const auto result = frust_engine::check("effect.frust", unit);
    out.compileMs = result.compileMs;
    out.diagnostics = shifted(result.diagnostics, prefix);
    out.ok = result.ok;
    if (!out.ok)
    {
        for (const auto& d : out.diagnostics) out.error += d.text() + "\n";
        if (!result.error.empty()) out.error += result.error + "\n";
    }
    return out;
}

Compiled compile(const Spec& spec)
{
    Compiled out;
    int prefix = 0;
    const auto unit = unitSource(spec, &prefix);
    const auto unitResult = frust_engine::compileToObject("effect.frust", unit);
    out.compileMs = unitResult.result.compileMs;
    out.diagnostics = shifted(unitResult.result.diagnostics, prefix);
    if (!unitResult.result.ok || unitResult.objectBytes.empty())
    {
        for (const auto& d : out.diagnostics) out.error += d.text() + "\n";
        if (!unitResult.result.error.empty()) out.error += unitResult.result.error + "\n";
        if (out.error.empty()) out.error = "FRust produced no native code.\n";
        return out;
    }
    out.object = unitResult.objectBytes;
    out.manifest = runtimeManifest(spec);
    out.ok = true;
    return out;
}

Artifact makeArtifact(const Spec& spec, const Compiled& compiled)
{
    Artifact a;
    a.spec = spec;
    a.pluginCode4CC = derivePluginCode4CC(spec.id);
    a.vst3UidHex = deriveVst3UidHex(spec.id);
    a.manifest = compiled.manifest;
    a.object = compiled.object;
    a.objectHash = hashHex(a.object.data(), a.object.size());
    std::ostringstream material;
    material << a.spec.id << '|' << a.spec.name << '|' << a.spec.version << '|' << a.spec.workspaceSize << '|' << a.manifest << '|' << a.objectHash;
    for (const auto& p : a.spec.params)
        material << '|' << p.id << ':' << p.minValue << ':' << p.maxValue << ':' << p.defaultValue;
    const auto text = material.str();
    a.integrityHash = hashHex(text.data(), text.size());
    return a;
}

juce::String serialize(const Artifact& artifact, bool includeSource)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("magic", kArtifactMagic);
    root->setProperty("schemaVersion", kArtifactSchema);
    root->setProperty("status", "executable_object_ready");
    auto* product = new juce::DynamicObject();
    product->setProperty("id", juce::String(artifact.spec.id));
    product->setProperty("name", juce::String(artifact.spec.name));
    product->setProperty("version", juce::String(artifact.spec.version));
    product->setProperty("manufacturer", juce::String(artifact.spec.manufacturer));
    product->setProperty("pluginCode4CC", juce::String(artifact.pluginCode4CC));
    product->setProperty("vst3Uid", juce::String(artifact.vst3UidHex));
    root->setProperty("product", juce::var(product));
    auto* target = new juce::DynamicObject();
    target->setProperty("arch", kTargetArch);
    target->setProperty("objectFormat", kObjectFormat);
    target->setProperty("entryPoint", kEntryPoint);
    root->setProperty("target", juce::var(target));
    juce::Array<juce::var> params;
    for (const auto& p : artifact.spec.params)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("id", juce::String(p.id));
        o->setProperty("name", juce::String(p.name));
        o->setProperty("unit", juce::String(p.unit));
        o->setProperty("min", p.minValue);
        o->setProperty("max", p.maxValue);
        o->setProperty("default", p.defaultValue);
        params.add(juce::var(o));
    }
    root->setProperty("parameters", params);
    root->setProperty("workspaceSize", artifact.spec.workspaceSize);
    root->setProperty("runtimeManifest", juce::String(artifact.manifest));
    root->setProperty("objectBase64", juce::Base64::toBase64(artifact.object.data(), artifact.object.size()));
    root->setProperty("objectHash", juce::String(artifact.objectHash));
    root->setProperty("integrityHash", juce::String(artifact.integrityHash));
    if (includeSource)
        root->setProperty("source", juce::String(artifact.spec.source));
    return juce::JSON::toString(juce::var(root), false);
}

bool parseArtifact(const juce::String& json, Artifact& artifact, std::string& error)
{
    const auto root = juce::JSON::parse(json);
    if (!root.isObject()) { error = "Not a JSON effect artifact."; return false; }
    if (root["magic"].toString() != kArtifactMagic) { error = "Not a Djehuti Math effect artifact (wrong magic)."; return false; }
    if ((int) root["schemaVersion"] != kArtifactSchema) { error = "Unsupported effect artifact schema version."; return false; }
    if (root["status"].toString() != "executable_object_ready") { error = "Artifact is not executable."; return false; }
    if (root["target"]["arch"].toString() != kTargetArch || root["target"]["objectFormat"].toString() != kObjectFormat)
    {
        error = "Artifact targets a different architecture than " + std::string(kTargetArch) + ".";
        return false;
    }
    Artifact a;
    a.spec.id = root["product"]["id"].toString().toStdString();
    a.spec.name = root["product"]["name"].toString().toStdString();
    a.spec.version = root["product"]["version"].toString().toStdString();
    a.spec.manufacturer = root["product"]["manufacturer"].toString().toStdString();
    a.pluginCode4CC = root["product"]["pluginCode4CC"].toString().toStdString();
    a.vst3UidHex = root["product"]["vst3Uid"].toString().toStdString();
    if (a.spec.id.empty() || a.spec.name.empty()) { error = "Artifact has no product identity."; return false; }
    a.spec.workspaceSize = (int) root["workspaceSize"];
    if (a.spec.workspaceSize < 1 || a.spec.workspaceSize > kMaxWorkspace) { error = "Artifact workspace size is out of range."; return false; }
    const auto* params = root["parameters"].getArray();
    if (params == nullptr || (int) params->size() > kMaxParams) { error = "Artifact parameters are missing or too many."; return false; }
    for (const auto& v : *params)
    {
        Param p;
        p.id = v["id"].toString().toStdString();
        p.name = v["name"].toString().toStdString();
        p.unit = v["unit"].toString().toStdString();
        p.minValue = (double) v["min"];
        p.maxValue = (double) v["max"];
        p.defaultValue = (double) v["default"];
        if (!validIdentifier(p.id) || !std::isfinite(p.minValue) || !std::isfinite(p.maxValue) || !std::isfinite(p.defaultValue)
            || p.minValue >= p.maxValue || p.defaultValue < p.minValue || p.defaultValue > p.maxValue)
        {
            error = "Artifact parameter '" + p.id + "' is invalid.";
            return false;
        }
        for (const auto& other : a.spec.params)
            if (other.id == p.id) { error = "Artifact repeats parameter '" + p.id + "'."; return false; }
        a.spec.params.push_back(std::move(p));
    }
    a.manifest = root["runtimeManifest"].toString().toStdString();
    juce::MemoryOutputStream decoded;
    if (!juce::Base64::convertFromBase64(decoded, root["objectBase64"].toString()) || decoded.getDataSize() == 0)
    {
        error = "Artifact object code is missing or not valid base64.";
        return false;
    }
    a.object.assign(static_cast<const std::uint8_t*>(decoded.getData()), static_cast<const std::uint8_t*>(decoded.getData()) + decoded.getDataSize());
    a.objectHash = root["objectHash"].toString().toStdString();
    a.integrityHash = root["integrityHash"].toString().toStdString();
    a.spec.source = root["source"].toString().toStdString();
    if (hashHex(a.object.data(), a.object.size()) != a.objectHash) { error = "Object code does not match its hash (corrupt artifact)."; return false; }
    const auto expected = makeArtifact(a.spec, Compiled { true, {}, {}, a.object, a.manifest, 0.0 });
    if (expected.integrityHash != a.integrityHash || expected.pluginCode4CC != a.pluginCode4CC || expected.vst3UidHex != a.vst3UidHex)
    {
        error = "Artifact integrity check failed (identity, parameters or manifest were changed).";
        return false;
    }
    artifact = std::move(a);
    return true;
}

bool loadArtifactFile(const juce::File& file, Artifact& artifact, std::string& error)
{
    if (!file.existsAsFile() || file.getSize() > 64 * 1024 * 1024)
    {
        error = "Artifact file is missing or larger than 64 MB: " + file.getFullPathName().toStdString();
        return false;
    }
    return parseArtifact(file.loadFileAsString(), artifact, error);
}

// ---------------------------------------------------------------------------

Runtime::Runtime() = default;
Runtime::~Runtime() { engine.unloadAll(); }

bool Runtime::load(const Artifact& artifact, std::string& error)
{
    fn = nullptr;
    const auto result = engine.loadObject("effect", artifact.manifest, artifact.object);
    if (!result.ok)
    {
        error = "Could not link the effect's native code: " + result.report();
        return false;
    }
    fn = reinterpret_cast<ProcessFn>(engine.function("effect", kEntryPoint));
    if (fn == nullptr)
    {
        error = std::string("The linked effect has no ") + kEntryPoint + " function.";
        return false;
    }
    loaded = artifact;
    paramTotal = (int) artifact.spec.params.size();
    values.clear();
    for (const auto& p : artifact.spec.params)
        values.push_back(std::make_unique<std::atomic<double>>(p.defaultValue));
    prepare(sampleRate, channelCount);
    return true;
}

void Runtime::prepare(double rate, int channels)
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    channelCount = std::clamp(channels, 1, kMaxChannels);
    workspace.assign((size_t) channelCount * (size_t) std::max(1, loaded.spec.workspaceSize), 0.0);
    coeffScratch.assign((size_t) channelCount * (size_t) (paramTotal + 1), 0.0);
    resetCount.store(0);
}

void Runtime::reset() { std::fill(workspace.begin(), workspace.end(), 0.0); }

void Runtime::setParam(int index, double value)
{
    if (index < 0 || index >= paramTotal)
        return;
    const auto& p = loaded.spec.params[(size_t) index];
    values[(size_t) index]->store(std::clamp(value, p.minValue, p.maxValue), std::memory_order_relaxed);
}

double Runtime::param(int index) const
{
    return index >= 0 && index < paramTotal ? values[(size_t) index]->load(std::memory_order_relaxed) : 0.0;
}

void Runtime::process(float* data, int numSamples, int channel) noexcept { process(data, data, numSamples, channel); }

void Runtime::process(const float* input, float* output, int numSamples, int channel) noexcept
{
    if (fn == nullptr || channel < 0 || channel >= channelCount)
    {
        if (input != output)
            std::copy(input, input + numSamples, output);
        return;
    }
    const auto stride = (size_t) std::max(1, loaded.spec.workspaceSize);
    double* ws = workspace.data() + (size_t) channel * stride;
    double* coeffs = coeffScratch.data() + (size_t) channel * (size_t) (paramTotal + 1);
    for (int i = 0; i < paramTotal; ++i)
        coeffs[i] = values[(size_t) i]->load(std::memory_order_relaxed);
    coeffs[paramTotal] = sampleRate;
    for (int n = 0; n < numSamples; ++n)
    {
        const double y = fn((double) input[n], ws, coeffs);
        if (std::isfinite(y))
            output[n] = (float) y;
        else
        {
            std::fill(ws, ws + stride, 0.0);
            output[n] = 0.0f;
            resetCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------

std::vector<float> makeSignal(Signal kind, double sampleRate, double seconds, double frequency, double amplitude)
{
    const int n = std::max(1, (int) std::lround(sampleRate * seconds));
    std::vector<float> out((size_t) n, 0.0f);
    const double twoPi = 6.283185307179586;
    juce::Random random(0x4d41544831ull); // fixed seed: renders are repeatable
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate;
        switch (kind)
        {
            case Signal::Sine: out[(size_t) i] = (float) (amplitude * std::sin(twoPi * frequency * t)); break;
            case Signal::Noise: out[(size_t) i] = (float) (amplitude * (random.nextDouble() * 2.0 - 1.0)); break;
            case Signal::Impulse: out[(size_t) i] = i == 0 ? (float) amplitude : 0.0f; break;
            case Signal::Sweep:
            {
                const double f0 = 20.0, f1 = std::min(20000.0, sampleRate * 0.45), k = std::log(f1 / f0) / seconds;
                out[(size_t) i] = (float) (amplitude * std::sin(twoPi * f0 * (std::exp(k * t) - 1.0) / k));
                break;
            }
            case Signal::Silence: break;
        }
    }
    return out;
}

Measurement measure(const std::vector<float>& samples)
{
    Measurement m;
    double sum = 0.0, squares = 0.0;
    size_t count = 0;
    for (const float v : samples)
    {
        if (!std::isfinite(v)) { ++m.nonFinite; continue; }
        sum += v;
        squares += (double) v * v;
        m.peak = std::max(m.peak, (double) std::abs(v));
        if (std::abs(v) >= 1.0f) ++m.clipped;
        ++count;
    }
    if (count > 0) { m.dc = sum / (double) count; m.rms = std::sqrt(squares / (double) count); }
    return m;
}

std::string Measurement::summary() const
{
    std::ostringstream s;
    s.precision(4);
    s << "peak " << peak << " (" << (peak > 0.0 ? 20.0 * std::log10(peak) : -200.0) << " dBFS), rms " << rms
      << ", dc " << dc << ", clipped " << clipped << ", non-finite " << nonFinite;
    return s.str();
}

std::vector<double> harmonicLevelsDb(const std::vector<float>& samples, double sampleRate, double f0, int count)
{
    std::vector<double> out;
    const size_t n = samples.size();
    if (n < 64 || !(f0 > 0.0) || !(sampleRate > 0.0))
        return out;
    const double twoPi = 6.283185307179586;
    std::vector<double> window(n);
    double windowSum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        window[i] = 0.5 - 0.5 * std::cos(twoPi * (double) i / (double) (n - 1));
        windowSum += window[i];
    }
    for (int k = 1; k <= count && k * f0 < sampleRate * 0.5; ++k)
    {
        const double w = twoPi * k * f0 / sampleRate, c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double s = window[i] * (double) samples[i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        const double re = s1 - s2 * std::cos(w), im = s2 * std::sin(w);
        const double amplitude = 2.0 * std::sqrt(re * re + im * im) / windowSum;
        out.push_back(amplitude > 1.0e-10 ? 20.0 * std::log10(amplitude) : -200.0);
    }
    return out;
}

bool renderThrough(const Artifact& artifact, const std::vector<float>& input, double sampleRate,
                   const std::vector<double>& paramValues, std::vector<float>& output, std::string& error)
{
    Runtime runtime;
    if (!runtime.load(artifact, error))
        return false;
    runtime.prepare(sampleRate, 1);
    for (size_t i = 0; i < paramValues.size() && (int) i < runtime.paramCount(); ++i)
        runtime.setParam((int) i, paramValues[i]);
    output.assign(input.size(), 0.0f);
    constexpr int block = 512;
    for (size_t at = 0; at < input.size(); at += block)
        runtime.process(input.data() + at, output.data() + at, (int) std::min<size_t>(block, input.size() - at), 0);
    return true;
}

void registerFrontend(std::shared_ptr<EffectFrontend> frontend)
{
    std::lock_guard<std::mutex> lock(frontendMutex());
    frontendList().push_back(std::move(frontend));
}

std::vector<std::shared_ptr<EffectFrontend>> frontends()
{
    std::lock_guard<std::mutex> lock(frontendMutex());
    return frontendList();
}
}
