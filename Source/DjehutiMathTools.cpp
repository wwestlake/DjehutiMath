// The programming half of the command service: FRust checking and running, node
// programs, the debugger, DSP effects, audio and VST3 export. Agent tools call
// exactly the operations the panels' buttons call.

#include "DjehutiMath.h"
#include "FrustExecution.h"

namespace
{
juce::String text(juce::DynamicObject* root) { return juce::JSON::toString(juce::var(root), true); }

juce::var diagnosticsVar(const frust_engine::Result& r)
{
    juce::Array<juce::var> list;
    for (const auto& d : r.diagnostics)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("severity", d.error ? "error" : "warning");
        o->setProperty("line", d.line);
        o->setProperty("column", d.column);
        o->setProperty("message", juce::String(d.message));
        list.add(juce::var(o));
    }
    return list;
}

bool absolute(const juce::String& path, const char* suffix)
{
    return juce::File::isAbsolutePath(path) && path.endsWithIgnoreCase(suffix);
}
}

void DjehutiMath::appendLog(const juce::String& line)
{
    if (diagnostics == nullptr) return;
    diagnostics->moveCaretToEnd();
    diagnostics->insertTextAtCaret(juce::Time::getCurrentTime().formatted("%H:%M:%S  ") + line + "\n");
}

juce::String DjehutiMath::startNodeDebugging()
{
    frust_exec::Program program;
    juce::String error;
    if (!nodes->buildDebugProgram(program, error))
        return error;
    // Debug programs get the math host functions as well as the debugger's.
    program.prelude += frust_engine::mathPrelude();
    for (const auto& host : frust_engine::mathHostFunctions())
        program.hostFunctions.push_back(host);
    const auto label = juce::String(program.label);
    std::string startError;
    if (!frust_exec::Executor::instance().start(std::move(program), startError))
        return juce::String(startError);
    appendLog("Debugging node program " + label + ".");
    return {};
}

void DjehutiMath::syncDebugMarkers()
{
    std::vector<frust_exec::Breakpoint> breakpoints;
    for (const auto& m : nodes->breakpointMarkers())
        breakpoints.push_back({ m.nodeId.toStdString(), 0, m.enabled, 0 });
    std::vector<std::string> watches;
    for (const auto& w : nodes->watchedNodes())
        watches.push_back(w.toStdString());
    frust_exec::Executor::instance().updateMarkers(nodes->programId().toStdString(), breakpoints, watches);
    if (debugger != nullptr)
        debugger->refreshBreakpoints();
}

void DjehutiMath::executionChanged()
{
    const auto snap = frust_exec::Executor::instance().snapshot();
    if (nodes != nullptr)
        nodes->setExecutionMarker(snap.state == frust_exec::State::Paused && snap.programId == nodes->programId().toStdString()
                                      ? juce::String(snap.nodeId) : juce::String());
    if (snap.session != 0 && !frust_exec::isActive(snap.state) && snap.session != lastReportedSession)
    {
        lastReportedSession = snap.session;
        juce::String line = "FRust program " + juce::String(snap.label) + " " + frust_exec::stateName(snap.state);
        if (snap.state == frust_exec::State::Failed)
            line << ": " << juce::String(snap.result.report()).trim();
        appendLog(line + ".");
    }
}

bool DjehutiMath::dispatchAsync(const std::string& toolName, const std::string& arguments, std::function<void(juce::String)> done)
{
    static const juce::StringArray later { "frust_run", "node_program_run", "node_debug_start", "node_debug_control", "dsp_compile" };
    const juce::String name(toolName);
    if (!later.contains(name))
        return false;
    const auto args = juce::JSON::parse(arguments.empty() ? "{}" : juce::String(arguments));
    auto fail = [name, done](const juce::String& error) {
        auto* o = new juce::DynamicObject();
        o->setProperty("tool", name);
        o->setProperty("ok", false);
        o->setProperty("error", error);
        done(text(o));
        return true;
    };
    if (!args.isObject())
        return fail("Arguments must be a JSON object.");
    using frust_exec::State;
    auto& executor = frust_exec::Executor::instance();
    const int waitMs = juce::jlimit(0, 100000, (int) args.getProperty("wait_ms", 10000));

    if (name == "dsp_compile")
    {
        dsp->compile([this, done](bool ok, const juce::String& message) {
            const auto described = dsp->describe();
            auto* o = described.getDynamicObject();
            auto* root = new juce::DynamicObject();
            if (o != nullptr)
                for (const auto& p : o->getProperties())
                    root->setProperty(p.name, p.value);
            root->setProperty("tool", "dsp_compile");
            root->setProperty("ok", ok);
            root->setProperty(ok ? "message" : "error", message);
            done(text(root));
        });
        return true;
    }

    // Replies once the session has ended (or paused, for debugging), or after wait_ms.
    auto settle = [&executor, name, done, waitMs](std::uint64_t session, bool untilEnded) {
        executor.whenState(
            [session, untilEnded](const frust_exec::Snapshot& s) {
                if (s.session != session) return true;
                return untilEnded ? !frust_exec::isActive(s.state) : (s.state == State::Paused || !frust_exec::isActive(s.state));
            },
            waitMs,
            [name, done](const frust_exec::Snapshot& s, bool timedOut) {
                auto state = frust_exec::toVar(s);
                auto* o = state.getDynamicObject();
                o->setProperty("tool", name);
                o->setProperty("ok", s.state != State::Failed);
                if (timedOut)
                    o->setProperty("note", "Still " + juce::String(frust_exec::stateName(s.state)) + " after wait_ms; node_debug_inspect reads it, node_debug_control stop ends it.");
                done(juce::JSON::toString(state, true));
            });
        return true;
    };
    auto settleRun = [&executor, name, done, waitMs](std::uint64_t session) {
        executor.whenState(
            [session](const frust_exec::Snapshot& s) { return s.session != session || !frust_exec::isActive(s.state); },
            waitMs,
            [name, done](const frust_exec::Snapshot& s, bool timedOut) {
                auto* o = new juce::DynamicObject();
                o->setProperty("tool", name);
                o->setProperty("state", frust_exec::stateName(s.state));
                if (timedOut)
                {
                    o->setProperty("ok", true);
                    o->setProperty("note", "Still running after wait_ms; execution_status reads it, execution_stop ends it.");
                }
                else
                {
                    const auto& r = s.result;
                    o->setProperty("ok", r.ok);
                    if (r.ok || s.state == State::Cancelled)
                        o->setProperty("output", juce::String(r.output));
                    if (!r.error.empty())
                        o->setProperty("error", juce::String(r.error));
                    if (!r.diagnostics.empty())
                        o->setProperty("diagnostics", diagnosticsVar(r));
                    o->setProperty("compileMs", r.compileMs);
                    o->setProperty("runMs", r.runMs);
                }
                done(text(o));
            });
        return true;
    };

    if (name == "frust_run")
    {
        const auto source = args.hasProperty("source") ? args["source"].toString() : frust->getSource();
        std::string error;
        if (!frust->start(source, "Agent FRust", error))
            return fail(juce::String(error));
        return settleRun(frust->lastSession());
    }
    if (name == "node_program_run")
    {
        juce::String message, script, error;
        if (!nodes->compileProgram(message)) return fail(message);
        if (!nodes->buildRunScript(script, error)) return fail(error);
        std::string startError;
        if (!frust->start(script, nodes->programLabel(), startError)) return fail(juce::String(startError));
        return settleRun(frust->lastSession());
    }
    if (name == "node_debug_start")
    {
        const auto why = startNodeDebugging();
        if (why.isNotEmpty()) return fail(why);
        return settle(executor.snapshot().session, false);
    }
    if (name == "node_debug_control")
    {
        const auto action = args["action"].toString();
        std::string error;
        if (action == "pause")
        {
            if (!executor.pause(error)) return fail(juce::String(error));
            return settle(executor.snapshot().session, false);
        }
        if (action == "stop")
        {
            if (!executor.stop(error)) return fail(juce::String(error));
            return settle(executor.snapshot().session, true);
        }
        const auto command = action == "continue" ? frust_exec::Command::Continue
                           : action == "step_into" ? frust_exec::Command::StepInto
                           : action == "step_over" ? frust_exec::Command::StepOver
                           : action == "step_out" ? frust_exec::Command::StepOut : frust_exec::Command::Continue;
        if (!juce::StringArray { "continue", "step_into", "step_over", "step_out" }.contains(action))
            return fail("action must be continue, step_into, step_over, step_out, pause or stop.");
        if (!executor.command(command, error)) return fail(juce::String(error));
        return settle(executor.snapshot().session, false);
    }
    return false;
}

juce::String DjehutiMath::dispatchProgramming(const juce::String& name, const juce::var& args, bool& handled)
{
    handled = true;
    auto* root = new juce::DynamicObject();
    root->setProperty("tool", name);
    auto fail = [root](const juce::String& error) {
        root->setProperty("ok", false);
        root->setProperty("error", error);
        return text(root);
    };
    auto str = [&args](const char* key) { return args.getProperty(key, {}).toString().trim(); };
    auto num = [&args](const char* key, double fallback) { return args.hasProperty(key) ? (double) args.getProperty(key, fallback) : fallback; };
    juce::String error;

    if (name == "frust_run" || name == "node_program_run" || name == "node_debug_start" || name == "node_debug_control" || name == "dsp_compile")
        return fail("This tool answers when the program settles; the agent worker calls it asynchronously.");

    // ---- FRust ----------------------------------------------------------
    if (name == "frust_check")
    {
        const auto result = frust->checkNow(args.hasProperty("source") ? args["source"].toString() : frust->getSource());
        root->setProperty("compiled", result.ok);
        root->setProperty("diagnostics", diagnosticsVar(result));
        if (!result.error.empty()) root->setProperty("error", juce::String(result.error));
        root->setProperty("compileMs", result.compileMs);
        root->setProperty("ok", true);
        return text(root);
    }

    // ---- node programs ----------------------------------------------------
    if (name == "node_program_new")
    {
        const auto type = str("diagramType");
        if (type != "node_graph" && type != "state_machine") return fail("diagramType must be node_graph or state_machine.");
        nodes->newGraph(type, (bool) args.getProperty("starter", false));
        dock->activatePanel("nodes");
        root->setProperty("program", nodes->describeGraph());
    }
    else if (name == "node_program_inspect") root->setProperty("program", nodes->describeGraph());
    else if (name == "node_program_node_types") root->setProperty("nodeTypes", nodes->describeNodeTypes());
    else if (name == "node_program_add_node")
    {
        const auto id = nodes->addNodeOfType(str("type"), (float) num("x", 0), (float) num("y", 0), str("id"), error);
        if (id.isEmpty()) return fail(error);
        root->setProperty("id", id);
    }
    else if (name == "node_program_delete_node") { if (!nodes->deleteNode(str("id"), error)) return fail(error); }
    else if (name == "node_program_move_node") { if (!nodes->moveNode(str("id"), (float) num("x", 0), (float) num("y", 0), error)) return fail(error); }
    else if (name == "node_program_connect")
    {
        const auto id = nodes->connect(str("from"), str("fromPin"), str("to"), str("toPin"), error);
        if (id.isEmpty()) return fail(error);
        root->setProperty("connection", id);
    }
    else if (name == "node_program_disconnect") { if (!nodes->disconnect(str("connection"), error)) return fail(error); }
    else if (name == "node_program_set_parameter")
    {
        juce::var value = args.getProperty("value", {});
        if (value.isString())
        {
            const auto s = value.toString().trim();
            if (s.equalsIgnoreCase("true") || s.equalsIgnoreCase("false")) value = s.equalsIgnoreCase("true");
            else if (s.isNotEmpty() && s != "-" && s.retainCharacters("-0123456789") == s) value = s.getLargeIntValue();
        }
        if (!nodes->setNodeParameter(str("id"), str("name"), value, error)) return fail(error);
    }
    else if (name == "node_program_validate")
    {
        juce::Array<juce::var> problems;
        for (const auto& p : nodes->validateGraph()) problems.add(p);
        root->setProperty("valid", problems.isEmpty());
        root->setProperty("problems", problems);
    }
    else if (name == "node_program_compile")
    {
        juce::String message;
        if (!nodes->compileProgram(message)) return fail(message);
        root->setProperty("message", message);
        root->setProperty("source", nodes->generatedProgramSource());
    }
    else if (name == "node_program_export")
    {
        juce::File packageRoot;
        juce::String message;
        if (!nodes->exportProgram(packageRoot, message)) return fail(message);
        juce::Array<juce::var> files;
        for (const auto& f : packageRoot.findChildFiles(juce::File::findFiles, true))
            files.add(f.getRelativePathFrom(packageRoot).replaceCharacter('\\', '/'));
        root->setProperty("folder", packageRoot.getFullPathName());
        root->setProperty("files", files);
    }
    else if (name == "node_program_save" || name == "node_program_open")
    {
        const auto path = str("path");
        if (!absolute(path, ".frnode.json")) return fail("Use an absolute path ending in .frnode.json.");
        if (name == "node_program_save")
        {
            if (!nodes->saveToFile(juce::File(path), error)) return fail(error);
        }
        else
        {
            if (!nodes->openFile(juce::File(path), error)) return fail(error);
            if (error.isNotEmpty()) root->setProperty("problems", error);
            dock->activatePanel("nodes");
            root->setProperty("program", nodes->describeGraph());
        }
        root->setProperty("file", path);
    }
    // ---- debugger -----------------------------------------------------------
    else if (name == "node_debug_inspect")
    {
        const auto what = str("what").isEmpty() ? juce::String("state") : str("what");
        const auto snap = frust_exec::Executor::instance().snapshot();
        auto state = frust_exec::toVar(snap);
        if (what == "state")
            return juce::JSON::toString(state, true);
        if (what == "stack" || what == "variables")
        {
            if (snap.state != frust_exec::State::Paused)
                return fail("The program is " + juce::String(frust_exec::stateName(snap.state)) + "; the stack and variables are read while it is paused.");
            const auto* frames = state.getProperty("stack", {}).getArray();
            if (what == "stack") root->setProperty("stack", state.getProperty("stack", {}));
            else
            {
                const int frame = (int) args.getProperty("frame", 0);
                if (frames == nullptr || frame < 0 || frame >= frames->size())
                    return fail("No frame " + juce::String(frame) + ".");
                root->setProperty("frame", frame);
                root->setProperty("variables", frames->getReference(frame).getProperty("variables", {}));
            }
        }
        else if (what == "watches") root->setProperty("watches", state.getProperty("watches", {}));
        else if (what == "breakpoints")
        {
            juce::String linesError;
            const auto lines = nodes->nodeLines(linesError);
            juce::Array<juce::var> list;
            for (const auto& m : nodes->breakpointMarkers())
            {
                auto* b = new juce::DynamicObject();
                b->setProperty("node", m.nodeId);
                b->setProperty("enabled", m.enabled);
                if (lines.count(m.nodeId) != 0) b->setProperty("line", lines.at(m.nodeId));
                list.add(juce::var(b));
            }
            root->setProperty("breakpoints", list);
            juce::Array<juce::var> watched;
            for (const auto& w : nodes->watchedNodes()) watched.add(w);
            root->setProperty("watchedNodes", watched);
            if (linesError.isNotEmpty()) root->setProperty("linesUnavailable", linesError);
        }
        else return fail("what must be state, stack, variables, watches or breakpoints.");
    }
    else if (name == "node_debug_marker")
    {
        const auto kind = str("kind");
        const bool set = args.hasProperty("set") ? (bool) args.getProperty("set", true) : true;
        if (kind == "breakpoint") { if (!nodes->setBreakpoint(str("node"), set, args.hasProperty("enabled") ? (bool) args.getProperty("enabled", true) : true, error)) return fail(error); }
        else if (kind == "watch") { if (!nodes->setWatch(str("node"), set, error)) return fail(error); }
        else return fail("kind must be breakpoint or watch.");
    }
    // ---- DSP effects --------------------------------------------------------
    else if (name == "dsp_template")
    {
        if (!dsp->loadStarter(str("kind"), error)) return fail(error);
        dock->activatePanel("dsp");
        root->setProperty("source", dsp->source());
    }
    else if (name == "dsp_source")
    {
        if (args.hasProperty("source")) dsp->setSource(args["source"].toString());
        dock->activatePanel("dsp");
        root->setProperty("source", dsp->source());
        dsp_effect::Spec spec;
        std::string specError;
        const bool valid = dsp_effect::parseSpec(dsp->source().toStdString(), spec, specError);
        root->setProperty("valid", valid);
        if (!valid) root->setProperty("problem", juce::String(specError));
    }
    else if (name == "dsp_status")
    {
        auto status = dsp->describe();
        if (auto* o = status.getDynamicObject()) { o->setProperty("tool", name); o->setProperty("ok", true); }
        return juce::JSON::toString(status, true);
    }
    else if (name == "dsp_set_parameter") { if (dsp->isCompiling()) return fail("A compile is in progress."); if (!dsp->setParameter(str("id"), num("value", 0), error)) return fail(error); }
    else if (name == "dsp_render")
    {
        if (dsp->isCompiling()) return fail("A compile is in progress; wait for dsp_compile to finish.");
        const auto signal = str("signal").isEmpty() ? juce::String("sine") : str("signal");
        const auto file = str("file");
        if (signal.equalsIgnoreCase("file") && !absolute(file, "") ) return fail("signal 'file' needs an absolute audio file path in 'file'.");
        const auto r = dsp->render(signal, num("frequency", 1000.0), num("seconds", 2.0), num("amplitude", 0.5), num("sampleRate", 48000.0), juce::File(file));
        if (!r.ok) return fail(r.error);
        auto stats = [](const dsp_effect::Measurement& m) {
            auto* o = new juce::DynamicObject();
            o->setProperty("peak", m.peak); o->setProperty("peakDbfs", m.peak > 0 ? 20.0 * std::log10(m.peak) : -200.0);
            o->setProperty("rms", m.rms); o->setProperty("dc", m.dc);
            o->setProperty("clippedSamples", m.clipped); o->setProperty("nonFiniteSamples", m.nonFinite);
            return juce::var(o);
        };
        root->setProperty("samples", (int) r.output.size());
        root->setProperty("sampleRate", r.sampleRate);
        root->setProperty("input", stats(r.inputStats));
        root->setProperty("output", stats(r.outputStats));
        if (!r.harmonicDb.empty())
        {
            juce::Array<juce::var> h;
            for (const auto db : r.harmonicDb) h.add(db);
            root->setProperty("outputHarmonicsDbfs", h);
            root->setProperty("harmonicsNote", "Levels of the output at 1x, 2x, 3x... the test frequency, in dBFS.");
        }
    }
    else if (name == "dsp_live")
    {
        const auto action = str("action");
        if (action == "stop") dsp->stopLive();
        else if (action == "start")
        {
            if (dsp->isCompiling()) return fail("A compile is in progress.");
            if (args.hasProperty("input"))
                if (!dsp->setLiveInput(str("input"), str("file"), error)) return fail(error);
            juce::String message;
            if (!dsp->startLive(message)) return fail(message);
            root->setProperty("message", message);
        }
        else return fail("action must be start or stop.");
        root->setProperty("live", dsp->isLive());
    }
    else if (name == "dsp_play_render") { if (!dsp->playLastRender(error)) return fail(error); }
    else if (name == "dsp_export_wav")
    {
        const auto path = str("path");
        if (!absolute(path, ".wav")) return fail("Use an absolute .wav path.");
        if (!dsp->exportLastRender(juce::File(path), !args.hasProperty("processed") || (bool) args.getProperty("processed", true), error)) return fail(error);
        root->setProperty("file", path);
    }
    else if (name == "vst_export")
    {
        if (dsp->isCompiling()) return fail("A compile is in progress.");
        const auto folder = str("folder");
        if (!juce::File::isAbsolutePath(folder)) return fail("folder must be an absolute path.");
        const auto r = dsp->exportVst3(juce::File(folder));
        if (!r.ok) return fail(juce::String(r.error));
        juce::Array<juce::var> files;
        for (const auto& f : r.files) files.add(juce::String(f));
        root->setProperty("package", r.packageDirectory.getFullPathName());
        root->setProperty("bundle", r.bundleDirectory.getFullPathName());
        root->setProperty("files", files);
        root->setProperty("identityLimitation", juce::String(r.identityLimitation));
    }
    else if (name == "audio_status")
    {
        if ((bool) args.getProperty("open", false))
        {
            const auto openError = audio.open();
            if (openError.isNotEmpty()) return fail("No audio device could be opened: " + openError);
        }
        root->setProperty("device", audio.describeDevice());
        root->setProperty("open", audio.isOpen());
        root->setProperty("sampleRate", audio.sampleRate());
        root->setProperty("playingRender", audio.isPlayingRenderedAudio());
    }
    else
    {
        handled = false;
        delete root;
        return {};
    }
    root->setProperty("ok", true);
    return text(root);
}
