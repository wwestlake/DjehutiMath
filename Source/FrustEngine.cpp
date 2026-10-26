#include "FrustEngine.h"
#include "MathSession.h"
#include <JuceHeader.h>

#include <frust_plugin_host/FrustPluginHost.h>
#include <frust_plugin_host/FrustPluginHostSource.h>
#include <CompilerApi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>

namespace frust_engine
{
namespace
{
// The plugin host keeps its application identity and host-function table per
// process, so loads happen one at a time.
std::mutex& loadMutex()
{
    static std::mutex m;
    return m;
}

double millisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string frustStringLiteral(const std::string& text)
{
    std::string literal = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            literal += '\\';
        literal += c;
    }
    return literal + "\"";
}

Diagnostic convert(const ::frust::Diagnostic& d, const std::string& unitName, int lineOffset)
{
    Diagnostic out;
    out.error = d.severity == ::frust::Diagnostic::Severity::Error;
    out.file = d.file.empty() && d.line > 0 ? unitName : d.file;
    out.line = d.line;
    if (out.file == unitName && out.line > lineOffset)
        out.line -= lineOffset;
    out.column = d.column;
    out.message = d.message;
    return out;
}

::frust::CompileRequest requestFor(const std::string& unitName, const std::string& source)
{
    ::frust::CompileRequest request;
    request.sources.push_back({ unitName, source });
    return request;
}

// Lines a running script logs; a script runs on one thread.
thread_local std::vector<std::string>* scriptLog = nullptr;

extern "C" std::int64_t djehuti_frust_log(const char* text)
{
    if (scriptLog != nullptr)
        std::cout << (text != nullptr ? text : "") << "\n";   // into the captured output, in order
    return 0;
}

// Math the generated audio DSP needs (Frust has no exp or log of its own).
extern "C" double djehuti_dsp_exp(double x) { return std::exp(x); }
extern "C" double djehuti_dsp_log(double x) { return std::log(x); }
extern "C" double djehuti_dsp_sin(double x) { return std::sin(x); }
extern "C" double djehuti_dsp_cos(double x) { return std::cos(x); }
extern "C" double djehuti_dsp_tan(double x) { return std::tan(x); }
extern "C" double djehuti_dsp_tanh(double x) { return std::tanh(x); }
extern "C" double djehuti_dsp_atan(double x) { return std::atan(x); }
extern "C" double djehuti_dsp_pow(double x, double y) { return std::pow(x, y); }
extern "C" double djehuti_dsp_sqrt(double x) { return std::sqrt(x); }

// The Frust compiler emits a call to frust_dbg_tick(line, column) before every
// expression (its IDE debugger hook); without a definition no compiled unit
// links. This registration and the FRust runtime's own frust_dbg_tick (which
// calls g_frust_dbg_callback) both lead to the app's handler, the debugger;
// with none installed a tick does nothing.
std::atomic<TickHandler> tickHandler { nullptr };
extern "C" std::int64_t djehuti_math_set_scalar(const char* name, double value)
{
    return MathSession::instance().setScalar(juce::String::fromUTF8(name != nullptr ? name : ""), value) ? 1 : 0;
}
extern "C" double djehuti_math_get_scalar(const char* name)
{
    return MathSession::instance().scalar(juce::String::fromUTF8(name != nullptr ? name : ""));
}

extern "C" void djehuti_frust_dbg_tick(int line, int column)
{
    if (auto* handler = tickHandler.load(std::memory_order_relaxed))
        handler(line, column);
}

// The script running on this thread, so a Stop from inside it can restore
// standard output and unload it (abandonRunningScript).
struct RunningScript
{
    FrustPluginHandle handle = nullptr;
    std::ostringstream* printed = nullptr;
    std::streambuf* consoleBuffer = nullptr;
};
thread_local RunningScript* runningScript = nullptr;

void ensureHostSetup()
{
    static std::once_flag once;
    std::call_once(once, [] {
        frust_plugin_host_set_application_identity("djehuti-math");
        frust_plugin_register_host_function("djehuti_math_set_scalar", reinterpret_cast<void*>(&djehuti_math_set_scalar));
        frust_plugin_register_host_function("djehuti_math_get_scalar", reinterpret_cast<void*>(&djehuti_math_get_scalar));
        frust_plugin_register_host_function("frust_dbg_tick", reinterpret_cast<void*>(&djehuti_frust_dbg_tick));
        frust_plugin_register_host_function("djehuti_frust_log", reinterpret_cast<void*>(&djehuti_frust_log));
        frust_plugin_register_host_function("djehuti_dsp_exp", reinterpret_cast<void*>(&djehuti_dsp_exp));
        frust_plugin_register_host_function("djehuti_dsp_log", reinterpret_cast<void*>(&djehuti_dsp_log));
        frust_plugin_register_host_function("djehuti_dsp_sin", reinterpret_cast<void*>(&djehuti_dsp_sin));
        frust_plugin_register_host_function("djehuti_dsp_cos", reinterpret_cast<void*>(&djehuti_dsp_cos));
        frust_plugin_register_host_function("djehuti_dsp_tan", reinterpret_cast<void*>(&djehuti_dsp_tan));
        frust_plugin_register_host_function("djehuti_dsp_tanh", reinterpret_cast<void*>(&djehuti_dsp_tanh));
        frust_plugin_register_host_function("djehuti_dsp_atan", reinterpret_cast<void*>(&djehuti_dsp_atan));
        frust_plugin_register_host_function("djehuti_dsp_pow", reinterpret_cast<void*>(&djehuti_dsp_pow));
        frust_plugin_register_host_function("djehuti_dsp_sqrt", reinterpret_cast<void*>(&djehuti_dsp_sqrt));
    });
}

Result checkWithOffset(const std::string& unitName, const std::string& source, int lineOffset)
{
    Result result;
    auto request = requestFor(unitName, source);
    request.emitObject = false;
    const auto start = std::chrono::steady_clock::now();
    const auto compiled = ::frust::Compile(request);
    result.compileMs = millisecondsSince(start);
    for (const auto& d : compiled.diagnostics)
        result.diagnostics.push_back(convert(d, unitName, lineOffset));
    result.compiled = compiled.ok;
    result.ok = compiled.ok;
    return result;
}
}

std::string Diagnostic::text() const
{
    std::string s = file.empty() ? std::string("frust") : file;
    if (line > 0) s += ":" + std::to_string(line);
    if (column > 0) s += ":" + std::to_string(column);
    return s + ": " + (error ? "error: " : "warning: ") + message;
}

std::string Result::report() const
{
    std::string s;
    for (const auto& d : diagnostics)
        s += d.text() + "\n";
    if (!error.empty())
        s += error + "\n";
    return s;
}

std::string manifestJson(const std::string& name, const std::string& description,
                         const std::vector<std::string>& hostFunctions,
                         const std::vector<std::string>& entryPoints)
{
    std::string id;
    for (const char c : name)
        id += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') ? c : '_';
    std::string required;
    for (const auto& fn : hostFunctions)
        required += std::string(required.empty() ? "" : ",") + "{\"name\":\"" + fn + "\"}";
    std::string entries;
    for (const auto& ep : entryPoints)
        entries += std::string(entries.empty() ? "" : ",") + frustStringLiteral(ep);
    return "{\"name\":\"" + (id.empty() ? std::string("unit") : id) + "\",\"version\":\"0.1.0\",\"description\":"
           + frustStringLiteral(description)
           + (entries.empty() ? std::string() : ",\"entryPoints\":[" + entries + "]")
           + (required.empty() ? std::string() : ",\"requiredHostFunctions\":[" + required + "]") + "}";
}

std::string manifestLine(const std::string& name, const std::string& description, const std::vector<std::string>& hostFunctions)
{
    return "manifest " + frustStringLiteral(manifestJson(name, description, hostFunctions)) + ";\n";
}

CompiledUnit compileToObject(const std::string& unitName, const std::string& source)
{
    ensureHostSetup();
    CompiledUnit out;
    auto request = requestFor(unitName, source);
    request.emitObject = true;
    request.captureIr = true;
    const auto start = std::chrono::steady_clock::now();
    const auto compiled = ::frust::Compile(request);
    out.result.compileMs = millisecondsSince(start);
    for (const auto& d : compiled.diagnostics)
        out.result.diagnostics.push_back(convert(d, unitName, 0));
    out.result.compiled = compiled.ok && !compiled.object.empty();
    out.result.ok = out.result.compiled;
    if (!out.result.ok && out.result.diagnostics.empty())
        out.result.error = "Frust compilation failed to produce native object code.";
    out.objectBytes = compiled.object;
    out.optimizedIr = compiled.irAfterOptimization;
    return out;
}

Result check(const std::string& unitName, const std::string& source)
{
    return checkWithOffset(unitName, source, 0);
}

Engine::Engine()
{
    ensureHostSetup();
}

Engine::~Engine()
{
    unloadAll();
}

Result Engine::loadObject(const std::string& key, const std::string& manifestJsonText,
                          const std::vector<std::uint8_t>& objectBytes)
{
    ensureHostSetup();
    Result result;
    const auto start = std::chrono::steady_clock::now();
    FrustPluginHandle handle = nullptr;
    {
        std::lock_guard<std::mutex> loading(loadMutex());
        const auto isolatedName = std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "_" + key;
        handle = frust_plugin_load_object(
            isolatedName.c_str(),
            manifestJsonText.c_str(),
            objectBytes.empty() ? nullptr : objectBytes.data(),
            objectBytes.size());
        if (handle == nullptr)
        {
            const char* why = frust_plugin_last_error();
            result.error = why != nullptr && *why != '\0' ? why : "Precompiled Frust object failed validation or linking.";
        }
    }
    result.compileMs = millisecondsSince(start);
    if (handle == nullptr)
    {
        result.ok = false;
        result.compiled = false;
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (auto found = units.find(key); found != units.end())
        frust_plugin_unload(found->second);
    units[key] = handle;
    result.ok = true;
    result.compiled = true;
    return result;
}

Result Engine::load(const std::string& key, const std::string& source)
{
    const auto unitName = key + ".frust";
    auto result = checkWithOffset(unitName, source, 0);
    if (!result.ok)
        return result;
    const auto start = std::chrono::steady_clock::now();
    FrustPluginHandle handle = nullptr;
    {
        std::lock_guard<std::mutex> loading(loadMutex());
        handle = frust_plugin_host::loadFromSource(requestFor(unitName, source));
        if (handle == nullptr)
        {
            const char* why = frust_plugin_last_error();
            result.error = why != nullptr && *why != '\0' ? why : "The Frust unit compiled but could not be loaded.";
        }
    }
    result.compileMs += millisecondsSince(start);
    if (handle == nullptr)
    {
        result.ok = false;
        return result;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (auto found = units.find(key); found != units.end())
        frust_plugin_unload(found->second);
    units[key] = handle;
    return result;
}

void* Engine::function(const std::string& key, const std::string& name) const
{
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = units.find(key);
    return found != units.end() ? frust_plugin_get_fn(found->second, name.c_str()) : nullptr;
}

void Engine::unload(const std::string& key)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (auto found = units.find(key); found != units.end())
    {
        frust_plugin_unload(found->second);
        units.erase(found);
    }
}

void Engine::unloadAll()
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [key, handle] : units)
        frust_plugin_unload(handle);
    units.clear();
}

bool Engine::isLoaded(const std::string& key) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return units.count(key) != 0;
}

void setTickHandler(TickHandler handler)
{
    tickHandler.store(handler);
}

std::string abandonRunningScript()
{
    auto* running = runningScript;
    if (running == nullptr)
        return {};
    runningScript = nullptr;
    scriptLog = nullptr;
    std::string printed;
    if (running->printed != nullptr)
    {
        std::cout.rdbuf(running->consoleBuffer);
        printed = running->printed->str();
    }
    if (running->handle != nullptr)
        frust_plugin_unload(running->handle);
    return printed;
}

Result runScript(const std::string& script)
{
    return runScript(script, RunOptions {});
}

Result runScript(const std::string& script, const RunOptions& options)
{
    ensureHostSetup();
    const std::string unitName = "script.frust";
    std::vector<std::string> hostFunctions { "djehuti_frust_log" };
    hostFunctions.insert(hostFunctions.end(), mathHostFunctions().begin(), mathHostFunctions().end());
    hostFunctions.insert(hostFunctions.end(), options.hostFunctions.begin(), options.hostFunctions.end());
    const std::string header = manifestLine("djehuti_math_script", "A script run in Djehuti Math.", hostFunctions)
                             + "extern fn djehuti_frust_log(text: String) -> i64;\n"
                               "pub fn print_line(text: String) -> i64 = { djehuti_frust_log(text) }\n"
                             + options.prelude;
    const int headerLines = (int)std::count(header.begin(), header.end(), '\n');
    const auto source = header + script;

    auto result = checkWithOffset(unitName, source, headerLines);
    if (!result.ok)
        return result;

    FrustPluginHandle handle = nullptr;
    const auto start = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> loading(loadMutex());
        handle = frust_plugin_host::loadFromSource(requestFor(unitName, source));
        if (handle == nullptr)
        {
            const char* why = frust_plugin_last_error();
            result.error = why != nullptr && *why != '\0' ? why : "The script compiled but could not be loaded.";
        }
    }
    result.compileMs += millisecondsSince(start);
    if (handle == nullptr)
    {
        result.ok = false;
        return result;
    }

    using RunFn = const char* (*)();
    auto* run = reinterpret_cast<RunFn>(frust_plugin_get_fn(handle, "run"));
    if (run == nullptr)
    {
        result.ok = false;
        result.error = "The script has no `pub fn run() -> String`; define it, and what it returns is reported.";
        frust_plugin_unload(handle);
        return result;
    }
    // The FRust runtime prints (frust_print_str, which node programs' Print
    // nodes call) to standard output; while the script runs that is captured,
    // with its print_line lines in order, as the script's output.
    if (options.shouldRun != nullptr && !options.shouldRun())
    {
        frust_plugin_unload(handle);
        result.ok = false;
        result.error = "Stopped before the script ran.";
        return result;
    }
    std::vector<std::string> lines;
    std::ostringstream printed;
    if (options.beforeRun != nullptr)
        options.beforeRun(headerLines);
    RunningScript running;
    running.handle = handle;
    running.printed = &printed;
    running.consoleBuffer = std::cout.rdbuf(printed.rdbuf());
    runningScript = &running;
    scriptLog = &lines;
    const auto runStart = std::chrono::steady_clock::now();
    const char* returned = run();
    result.runMs = millisecondsSince(runStart);
    scriptLog = nullptr;
    runningScript = nullptr;
    std::cout.rdbuf(running.consoleBuffer);
    result.output += printed.str();
    if (returned != nullptr)
        result.output += returned;
    frust_plugin_unload(handle);
    result.ok = true;
    return result;
}

std::string mathPrelude()
{
    return "extern fn djehuti_math_set_scalar(name: String, value: f64) -> i64;\n"
           "extern fn djehuti_math_get_scalar(name: String) -> f64;\n"
           "pub fn math_set_scalar(name: String, value: f64) -> i64 = { djehuti_math_set_scalar(name, value) }\n"
           "pub fn math_get_scalar(name: String) -> f64 = { djehuti_math_get_scalar(name) }\n";
}
const std::vector<std::string>& mathHostFunctions()
{
    static const std::vector<std::string> names { "djehuti_math_set_scalar", "djehuti_math_get_scalar" };
    return names;
}
}
