// Focused functional check of the DEWB-parity capabilities (node compiler and editor model, audio
// pipeline, DSP effects, VST3 export). It runs in the application executable like the foundation
// check: djehuti_math.exe --dsp-check <report.json>. It needs no audio device and writes only
// under a temporary folder.

#include "DspCheck.h"

#include "AudioPipeline.h"
#include "DspEffect.h"
#include "FrustEngine.h"
#include "NodeDesignerPanel.h"
#include "VstExporter.h"

#include <cmath>

namespace
{
using dsp_effect::Artifact;
using dsp_effect::Spec;

struct Report
{
    juce::Array<juce::var> checks;
    bool passed = true;
    void add(bool ok, const juce::String& name, const juce::String& detail = {})
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("name", name);
        value->setProperty("passed", ok);
        value->setProperty("detail", detail);
        checks.add(juce::var(value));
        passed = passed && ok;
    }
};

bool compileSpec(const std::string& source, Spec& spec, Artifact& artifact, juce::String& why)
{
    std::string error;
    if (!dsp_effect::parseSpec(source, spec, error)) { why = "parse: " + juce::String(error); return false; }
    const auto compiled = dsp_effect::compile(spec);
    if (!compiled.ok) { why = "compile: " + juce::String(compiled.error); return false; }
    artifact = dsp_effect::makeArtifact(spec, compiled);
    return true;
}

double maxDifference(const std::vector<float>& a, const std::vector<double>& b)
{
    double worst = a.size() == b.size() ? 0.0 : 1.0e9;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        worst = std::max(worst, std::abs((double) a[i] - b[i]));
    return worst;
}
}

bool dspCheck(const juce::File& destination)
{
    Report report;
    const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("djehuti-math-dsp-check-" + juce::Uuid().toString());
    work.createDirectory();
    juce::String why;

    // ---- directives and diagnostics ---------------------------------------------------------
    bool startersOk = true;
    for (const auto& kind : dsp_effect::starterKinds())
    {
        Spec spec;
        std::string error;
        const bool parsed = dsp_effect::parseSpec(dsp_effect::starterSource(kind), spec, error);
        const bool checked = parsed && dsp_effect::check(spec).ok;
        if (!checked) { startersOk = false; why = juce::String(kind) + ": " + juce::String(error); }
    }
    report.add(startersOk, "Every starter effect parses and passes the FRust compiler", why);

    {
        std::string error;
        Spec spec;
        const auto src = dsp_effect::starterSource("gain");
        auto wrongCoeffs = src; wrongCoeffs.replace(wrongCoeffs.find("Array<f64, 2>"), 13, "Array<f64, 5>");
        const bool a = !dsp_effect::parseSpec(wrongCoeffs, spec, error) && error.find("coeffs") != std::string::npos;
        const bool b = !dsp_effect::parseSpec("pub fn nothing() -> f64 = { 1.0 }", spec, error) && error.find("process_sample") != std::string::npos;
        auto reserved = src; reserved.replace(reserved.find("gain \"Gain\""), 11, "mix \"Gain\"");
        const bool c = !dsp_effect::parseSpec(reserved, spec, error) && error.find("reserved") != std::string::npos;
        auto twice = src; twice.insert(twice.find("// @workspace"), "// @param gain \"Again\" 0 1 0\n");
        const bool d = !dsp_effect::parseSpec(twice, spec, error);
        auto range = src; range.replace(range.find("0 2 1 x"), 7, "0 2 9 x");
        const bool e = !dsp_effect::parseSpec(range, spec, error) && error.find("default") != std::string::npos;
        report.add(a && b && c && d && e, "Bad directives and signatures are refused with a reason",
                   juce::String((int) a) + juce::String((int) b) + juce::String((int) c) + juce::String((int) d) + juce::String((int) e));
    }
    {
        Spec spec;
        std::string error;
        auto broken = dsp_effect::starterSource("gain");
        broken.replace(broken.find("audio_in * coeffs[0]"), 20, "audio_in * ;");
        dsp_effect::parseSpec(broken, spec, error);
        const auto result = dsp_effect::compile(spec);
        const bool mapped = !result.ok && !result.diagnostics.empty() && result.diagnostics.front().line == 6;
        report.add(mapped, "Compile diagnostics point at the author's own source line",
                   result.diagnostics.empty() ? "no diagnostic" : "line " + juce::String(result.diagnostics.front().line) + ": " + juce::String(result.error).trim());
    }

    // ---- native compile and numeric agreement with C++ references -------------------------------
    const double rate = 48000.0;
    const auto sine = dsp_effect::makeSignal(dsp_effect::Signal::Sine, rate, 0.1, 1000.0, 0.5);
    Artifact gainArtifact;
    {
        Spec spec;
        const bool ok = compileSpec(dsp_effect::starterSource("gain"), spec, gainArtifact, why);
        std::vector<float> out; std::string error;
        const bool rendered = ok && dsp_effect::renderThrough(gainArtifact, sine, rate, { 0.5 }, out, error);
        std::vector<double> expected; for (float v : sine) expected.push_back(0.5 * (double) v);
        report.add(rendered && maxDifference(out, expected) < 1.0e-6, "Gain effect: native output equals input x 0.5", ok ? juce::String(error) : why);
    }
    {
        Spec spec; Artifact art;
        const bool ok = compileSpec(dsp_effect::starterSource("softclip"), spec, art, why);
        std::vector<float> out; std::string error;
        const bool rendered = ok && dsp_effect::renderThrough(art, sine, rate, { 4.0, 0.7 }, out, error);
        std::vector<double> expected; for (float v : sine) expected.push_back(std::tanh(4.0 * (double) v) * 0.7);
        const double worst = rendered ? maxDifference(out, expected) : 1.0e9;
        report.add(rendered && worst < 2.0e-6, "Soft clip effect matches tanh(drive x) x level", juce::String(worst, 9) + (ok ? juce::String(error) : why));
        // a sine through tanh must add odd harmonics: the 3rd is clearly present, the 2nd is not
        const auto harmonics = dsp_effect::harmonicLevelsDb(out, rate, 1000.0, 3);
        report.add(harmonics.size() == 3 && harmonics[2] > harmonics[1] + 30.0, "Harmonic measurement shows the odd harmonics a symmetric clipper adds",
                   harmonics.size() == 3 ? juce::String(harmonics[0], 1) + " / " + juce::String(harmonics[1], 1) + " / " + juce::String(harmonics[2], 1) + " dBFS" : "none");
    }
    {
        Spec spec; Artifact art;
        const bool ok = compileSpec(dsp_effect::starterSource("lowpass"), spec, art, why);
        const auto impulse = dsp_effect::makeSignal(dsp_effect::Signal::Impulse, rate, 0.05, 0.0, 1.0);
        std::vector<float> out; std::string error;
        const bool rendered = ok && dsp_effect::renderThrough(art, impulse, rate, { 1000.0 }, out, error);
        std::vector<double> expected; double y = 0.0; const double a = 1.0 - std::exp(-6.283185307179586 * 1000.0 / rate);
        for (float v : impulse) { y += a * ((double) v - y); expected.push_back(y); }
        const double worst = rendered ? maxDifference(out, expected) : 1.0e9;
        report.add(rendered && worst < 2.0e-6, "One-pole low-pass keeps its state between samples (matches the C++ recursion)", juce::String(worst, 9) + (ok ? juce::String(error) : why));
    }
    {
        Spec spec; Artifact art;
        const bool ok = compileSpec(dsp_effect::starterSource("tremolo"), spec, art, why);
        const auto ones = std::vector<float>((size_t) 4800, 1.0f);
        std::vector<float> out; std::string error;
        const bool rendered = ok && dsp_effect::renderThrough(art, ones, rate, { 4.0, 0.6 }, out, error);
        std::vector<double> expected; double phase = 0.0;
        for (size_t i = 0; i < ones.size(); ++i)
        {
            expected.push_back(1.0 - 0.6 * 0.5 * (1.0 - std::sin(phase)));
            phase += 6.283185307179586 * 4.0 / rate;
            if (phase > 6.283185307179586) phase -= 6.283185307179586;
        }
        const double worst = rendered ? maxDifference(out, expected) : 1.0e9;
        report.add(rendered && worst < 5.0e-6, "Tremolo (host sin function plus state) matches the C++ reference", juce::String(worst, 9) + (ok ? juce::String(error) : why));
    }
    {
        Spec spec; Artifact art;
        const bool ok = compileSpec("// @effect Divider\n// @id com.test.divider\n// @param d \"D\" 0 1 0\n// @workspace 1\n"
                                    "pub fn process_sample(audio_in: f64, ws: Array<f64, 1>, coeffs: Array<f64, 2>) -> f64 = {\n    audio_in / coeffs[0]\n}\n", spec, art, why);
        dsp_effect::Runtime runtime; std::string error;
        bool guarded = false;
        if (ok && runtime.load(art, error))
        {
            runtime.prepare(rate, 1);
            std::vector<float> block(64, 1.0f);
            runtime.process(block.data(), 64, 0);
            guarded = runtime.resets() == 64 && dsp_effect::measure(block).nonFinite == 0 && dsp_effect::measure(block).peak == 0.0;
        }
        report.add(guarded, "A diverging effect outputs silence, resets its state and is counted", ok ? juce::String(error) : why);
    }

    // ---- artifact validation -----------------------------------------------------------------
    {
        const auto json = dsp_effect::serialize(gainArtifact);
        Artifact parsed; std::string error;
        const bool roundTrip = dsp_effect::parseArtifact(json, parsed, error) && parsed.object == gainArtifact.object && parsed.spec.params.size() == 1;
        // Damage the document through its parsed form so the test does not depend on the JSON layout.
        auto damage = [&json](const std::function<void(juce::DynamicObject&)>& change) {
            auto parsed = juce::JSON::parse(json);
            change(*parsed.getDynamicObject());
            return juce::JSON::toString(parsed, false);
        };
        const auto tamperedObject = damage([](juce::DynamicObject& o) {
            auto text = o.getProperty("objectBase64").toString();
            text = text.replaceSection(20, 1, text[20] == 'A' ? "B" : "A");
            o.setProperty("objectBase64", text);
        });
        const auto tamperedParam = damage([](juce::DynamicObject& o) { o.getProperty("parameters")[0].getDynamicObject()->setProperty("max", 3.0); });
        const auto wrongArch = damage([](juce::DynamicObject& o) { o.getProperty("target").getDynamicObject()->setProperty("arch", "aarch64-pc-windows-msvc"); });
        const auto wrongMagic = damage([](juce::DynamicObject& o) { o.setProperty("magic", "DJEHUTI_FRUST_EXECUTABLE_IL"); });
        Artifact scratch; std::string e1, e2, e3, e4;
        const bool rejected = !dsp_effect::parseArtifact(tamperedObject, scratch, e1) && !dsp_effect::parseArtifact(tamperedParam, scratch, e2)
                           && !dsp_effect::parseArtifact(wrongArch, scratch, e3) && !dsp_effect::parseArtifact(wrongMagic, scratch, e4);
        report.add(roundTrip && rejected, "Artifact round-trips; tampered object, parameters, architecture or magic are rejected", juce::String(e1) + " | " + juce::String(e2) + " | " + juce::String(e3) + " | " + juce::String(e4));
    }

    // ---- audio pipeline (driven without a device) -------------------------------------------------
    {
        const auto wav = work.getChildFile("tone.wav");
        std::vector<float> tone = dsp_effect::makeSignal(dsp_effect::Signal::Sine, rate, 0.25, 440.0, 0.4);
        AudioPipeline pipeline; std::vector<float> back; double backRate = 0.0; juce::String error;
        const bool wrote = AudioPipeline::exportToWav(wav, tone, rate);
        const bool read = wrote && pipeline.readAudioFile(wav, back, backRate, error);
        double worst = read && back.size() == tone.size() ? 0.0 : 1.0e9;
        for (size_t i = 0; read && i < std::min(back.size(), tone.size()); ++i) worst = std::max(worst, (double) std::abs(back[i] - tone[i]));
        report.add(wrote && read && backRate == rate && worst < 1.0 / 16000.0, "WAV export and read-back agree to 16-bit precision", juce::String(worst, 7) + error);

        // a block larger than the device buffer, processed in chunks, with a callback
        pipeline.prepareOffline(rate, 256);
        pipeline.setProcessCallback([](const float* in, float* out, int n) { for (int i = 0; i < n; ++i) out[i] = in[i] * 0.5f; });
        std::vector<float> input(1000), left(1000, 9.0f), right(1000, 9.0f);
        for (int i = 0; i < 1000; ++i) input[(size_t) i] = std::sin(0.01f * (float) i);
        const float* ins[] = { input.data() }; float* outs[] = { left.data(), right.data() };
        pipeline.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 1000, {});
        bool chunked = true;
        for (int i = 0; i < 1000; ++i) chunked = chunked && std::abs(left[(size_t) i] - input[(size_t) i] * 0.5f) < 1.0e-7f && left[(size_t) i] == right[(size_t) i];
        report.add(chunked, "Audio callback handles blocks larger than its buffer and feeds every output channel");

        // file in -> file out routing with a looping input
        const auto out = work.getChildFile("routed.wav");
        pipeline.setRouting("File", wav.getFullPathName(), "File", out.getFullPathName());
        const bool routeOk = pipeline.routingError().isEmpty();
        for (int block = 0; block < 60; ++block)
        {
            std::vector<float> l(256, 0.0f), r(256, 0.0f); float* o[] = { l.data(), r.data() };
            pipeline.audioDeviceIOCallbackWithContext(nullptr, 0, o, 2, 256, {});
        }
        pipeline.setRouting("Hardware", {}, "Hardware", {}); // closes and flushes the output file
        std::vector<float> routed; double routedRate = 0.0;
        const bool readOut = pipeline.readAudioFile(out, routed, routedRate, error);
        double routeWorst = readOut && routed.size() >= tone.size() ? 0.0 : 1.0e9;
        for (size_t i = 0; readOut && i < tone.size() && i < routed.size(); ++i) routeWorst = std::max(routeWorst, (double) std::abs(routed[i] - tone[i] * 0.5f));
        // 60 x 256 = 15360 samples out of a 12000-sample file: the input looped
        report.add(routeOk && readOut && routed.size() == 15360 && routeWorst < 1.0 / 8000.0, "File input loops through the callback into a WAV output",
                   juce::String((int) routed.size()) + " samples, error " + juce::String(routeWorst, 7) + error);
        report.add(!pipeline.isOpen() && pipeline.describeDevice() == "closed", "The audio device stays closed until it is asked for");
    }

    // ---- node programs ---------------------------------------------------------------------------
    {
        const auto graph = R"({"functionName":"compute","params":[],"output":"result","nodes":[
            {"id":"ten","type":"literal_i64","value":10},{"id":"four","type":"literal_i64","value":4},
            {"id":"sum","type":"add","inputs":[{"ref":"ten"},{"ref":"four"}]},
            {"id":"result","type":"mul","inputs":[{"ref":"sum"},{"ref":"ten"}]}]})";
        const auto compiled = node_compiler::CompileGraphToSource(graph);
        bool ran = false; juce::String output;
        if (compiled.ok)
        {
            const auto script = juce::String(compiled.source) + "\npub fn run() -> String = {\n    \"ok\"\n}\n";
            const auto result = frust_engine::runScript(script.toStdString());
            ran = result.ok; output = juce::String(result.report());
        }
        report.add(compiled.ok && ran && juce::String(compiled.source).contains("compute"), "Node compiler turns a graph into FRust that the embedded compiler accepts", compiled.ok ? output : juce::String(compiled.errorMessage));
    }
    {
        NodeDesignerPanel panel;
        panel.newGraph("node_graph", true);
        const auto problems = panel.validateGraph();
        juce::String message, script, error;
        const bool compiled = panel.compileProgram(message);
        const bool wrapped = compiled && panel.buildRunScript(script, error);
        bool ran = false; juce::String detail = message + error;
        if (wrapped)
        {
            const auto result = frust_engine::runScript(script.toStdString());
            ran = result.ok; detail = juce::String(result.report()) + " | output: " + juce::String(result.output).trim();
        }
        // The file a graph was opened from is part of its description; the graph itself is what must match.
        auto withoutFile = [](juce::var graph) { graph.getDynamicObject()->removeProperty("file"); return juce::JSON::toString(graph, false); };
        const auto described = panel.describeGraph();
        const auto file = work.getChildFile("check.frnode.json");
        const bool saved = panel.saveToFile(file, error);
        NodeDesignerPanel reopened;
        const bool opened = saved && reopened.openFile(file, error) && withoutFile(reopened.describeGraph()) == withoutFile(described);
        report.add(problems.isEmpty() && compiled && ran, "Node Designer starter program validates, compiles and runs", detail);
        report.add(opened, "Node program saves and reopens with the same graph", error);
    }

    // ---- VST3 export and hosting ---------------------------------------------------------------------
    {
        const auto templateDir = vst_exporter::templateBundle();
        report.add(templateDir != juce::File(), "The VST3 runtime template is packaged beside the executable", templateDir.getFullPathName());
        if (templateDir != juce::File())
        {
            vst_exporter::ExportRequest request;
            request.artifact = gainArtifact;
            request.outputDirectory = work.getChildFile("export");
            const auto exported = vst_exporter::runExport(request);
            report.add(exported.ok && exported.binaryFile.existsAsFile() && exported.artifactFile.existsAsFile() && exported.files.size() == 4, "VST3 export writes the bundle, artifact, manifest and UI schema", juce::String(exported.error));
            if (exported.ok)
            {
                juce::AudioPluginFormatManager formats;
                formats.addFormat(new juce::VST3PluginFormat());
                juce::OwnedArray<juce::PluginDescription> found;
                for (auto* format : formats.getFormats())
                    format->findAllTypesForFile(found, exported.bundleDirectory.getFullPathName());
                juce::String loadError;
                std::unique_ptr<juce::AudioPluginInstance> plugin;
                if (found.size() > 0)
                    plugin = formats.createPluginInstance(*found[0], rate, 512, loadError);
                report.add(plugin != nullptr, "A VST3 host loads the exported bundle", found.size() == 0 ? "no plugin types found in the bundle" : loadError);
                if (plugin != nullptr)
                {
                    plugin->setPlayConfigDetails(2, 2, rate, 512);
                    plugin->prepareToPlay(rate, 512);
                    juce::AudioProcessorParameter* gainParam = nullptr; juce::AudioProcessorParameter* bypassParam = nullptr;
                    for (auto* p : plugin->getParameters())
                    {
                        if (p->getName(64) == "Gain") gainParam = p;
                        if (p->getName(64) == "Bypass") bypassParam = p;
                    }
                    const bool hasParams = gainParam != nullptr && bypassParam != nullptr;
                    auto run = [&](juce::AudioPluginInstance& instance) {
                        juce::AudioBuffer<float> buffer(2, 512);
                        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < 512; ++i) buffer.setSample(ch, i, sine[(size_t) i] * (ch == 0 ? 1.0f : -1.0f));
                        juce::MidiBuffer midi;
                        instance.processBlock(buffer, midi);
                        return buffer;
                    };
                    if (hasParams) gainParam->setValue(0.25f); // range 0..2, so 0.25 is a gain of 0.5
                    auto processed = run(*plugin);
                    // the first block glides from the default gain (1.0) to 0.5, so compare the second
                    processed = run(*plugin);
                    double worst = 0.0;
                    for (int i = 0; i < 512; ++i) worst = std::max({ worst, (double) std::abs(processed.getSample(0, i) - sine[(size_t) i] * 0.5f), (double) std::abs(processed.getSample(1, i) + sine[(size_t) i] * 0.5f) });
                    report.add(hasParams && worst < 2.0e-5, "The hosted plugin applies the effect on both channels with the automated parameter", "max error " + juce::String(worst, 8));
                    if (bypassParam != nullptr) bypassParam->setValue(1.0f);
                    const auto bypassed = run(*plugin);
                    double bypassWorst = 0.0;
                    for (int i = 0; i < 512; ++i) bypassWorst = std::max(bypassWorst, (double) std::abs(bypassed.getSample(0, i) - sine[(size_t) i]));
                    report.add(bypassParam != nullptr && bypassWorst < 1.0e-9, "Bypass passes audio through untouched");
                    juce::MemoryBlock state;
                    if (bypassParam != nullptr) bypassParam->setValue(0.0f);
                    plugin->getStateInformation(state);
                    std::unique_ptr<juce::AudioPluginInstance> second;
                    if (found.size() > 0) second = formats.createPluginInstance(*found[0], rate, 512, loadError);
                    bool restored = false;
                    if (second != nullptr)
                    {
                        second->setStateInformation(state.getData(), (int) state.getSize());
                        for (auto* p : second->getParameters())
                            if (p->getName(64) == "Gain") restored = std::abs(p->getValue() - 0.25f) < 1.0e-3f;
                    }
                    report.add(restored, "Plugin state (automated parameter) saves and restores in a new instance");
                    plugin->releaseResources();
                }
                // the same bundle with its artifact damaged must not load an effect
                const auto damaged = exported.bundleDirectory.getChildFile("Contents").getChildFile("Resources").getChildFile("effect.frust-il");
                damaged.replaceWithText(damaged.loadFileAsString().replace("DJEHUTI_MATH_EFFECT_IL", "NOT_AN_EFFECT"));
                juce::OwnedArray<juce::PluginDescription> again;
                for (auto* format : formats.getFormats()) format->findAllTypesForFile(again, exported.bundleDirectory.getFullPathName());
                juce::String damagedError;
                auto broken = again.size() > 0 ? formats.createPluginInstance(*again[0], rate, 512, damagedError) : nullptr;
                bool passesThrough = false;
                if (broken != nullptr)
                {
                    broken->prepareToPlay(rate, 512);
                    juce::AudioBuffer<float> buffer(2, 64); buffer.clear(); for (int i = 0; i < 64; ++i) buffer.setSample(0, i, 0.25f);
                    juce::MidiBuffer midi; broken->processBlock(buffer, midi);
                    passesThrough = buffer.getSample(0, 10) == 0.25f; // unchanged: no effect was loaded
                }
                report.add(passesThrough, "A bundle with a damaged artifact loads inert (audio unchanged) instead of crashing");
            }
        }
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("passed", report.passed);
    root->setProperty("checks", report.checks);
    destination.getParentDirectory().createDirectory();
    destination.replaceWithText(juce::JSON::toString(juce::var(root), false));
    work.deleteRecursively();
    return report.passed;
}
