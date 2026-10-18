#include "FrustExecution.h"

#include <frust_plugin_host/FrustPluginHost.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <iomanip>
#include <process.h>
#include <sstream>

// The FRust runtime's debugger hook (RuntimeBasic.cpp): its frust_dbg_tick
// calls this when set.
extern "C" void (*g_frust_dbg_callback)(int, int);

namespace frust_exec
{
const char* stateName(State state)
{
    switch (state)
    {
        case State::Idle: return "idle";
        case State::Compiling: return "compiling";
        case State::Running: return "running";
        case State::Paused: return "paused";
        case State::Completed: return "completed";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
    }
    return "idle";
}

bool isActive(State state)
{
    return state == State::Compiling || state == State::Running || state == State::Paused;
}

const char* commandName(Command command)
{
    switch (command)
    {
        case Command::Continue: return "continue";
        case Command::StepInto: return "step_into";
        case Command::StepOver: return "step_over";
        case Command::StepOut: return "step_out";
    }
    return "continue";
}

struct FrameState
{
    int function = -1;
    int line = 0;    // 0 until its first line runs
    int column = 0;
    std::map<int, std::string> values; // slot -> value recorded in this call
};

struct Recorded
{
    std::string value;
    int function = -1;
    int line = 0;
};

struct Executor::Session
{
    std::uint64_t id = 0;
    Program program;

    mutable std::mutex mutex;
    std::condition_variable resumed; // the worker waits on it while paused
    State state = State::Compiling;
    std::atomic<bool> cancel { false };
    bool pauseRequested = false;

    int lineOffset = 0;               // compiled-unit line = program line + lineOffset
    std::vector<FrameState> frames;   // the calls the program is in (debug compile)
    std::map<int, Recorded> latest;   // slot -> last value recorded (watches)

    bool stepping = false;
    Command step = Command::Continue;
    size_t stepDepth = 0;
    bool pauseAtNextLine = false;     // a step left the call it started in

    std::string pauseReason;
    int pausedLine = 0, pausedColumn = 0;
    frust_engine::Result result;
};

namespace
{
// What the worker thread runs, owned by it; the session is shared with the
// executor (and outlives the thread).
struct Worker
{
    std::shared_ptr<Executor::Session> session;
};

thread_local Worker* currentWorker = nullptr;

std::string nodeAt(const Program& program, int line)
{
    const auto found = program.lineNodes.find(line);
    return found != program.lineNodes.end() ? found->second : std::string();
}

std::string formatValue(const Program& program, int slot, std::int64_t value)
{
    std::string text = std::to_string(value);
    if (slot >= 0 && slot < (int)program.info.slots.size())
    {
        const auto& names = program.info.slots[(size_t)slot].valueNames;
        if (value >= 0 && value < (std::int64_t)names.size())
            text += " (" + names[(size_t)value] + ")";
    }
    return text;
}

// Under the session's lock.
Snapshot snapshotOf(const Executor::Session& s)
{
    Snapshot snap;
    snap.session = s.id;
    snap.state = s.state;
    snap.label = s.program.label;
    snap.programId = s.program.programId;
    snap.debug = s.program.debug;
    snap.breakpoints = s.program.breakpoints;
    const auto& info = s.program.info;
    auto slotVariable = [&info](int slot, const std::string& value) {
        Variable v;
        if (slot >= 0 && slot < (int)info.slots.size())
        {
            const auto& d = info.slots[(size_t)slot];
            v.name = d.name;
            v.type = d.type;
            v.nodeIds = d.nodeIds;
        }
        v.value = value;
        return v;
    };
    auto functionName = [&info](int fn) {
        return fn >= 0 && fn < (int)info.functions.size() ? info.functions[(size_t)fn].name : std::string("?");
    };

    if (s.state == State::Paused)
    {
        snap.pauseReason = s.pauseReason;
        snap.line = s.pausedLine;
        snap.column = s.pausedColumn;
        snap.nodeId = nodeAt(s.program, s.pausedLine);
        for (auto it = s.frames.rbegin(); it != s.frames.rend(); ++it)
        {
            Frame f;
            f.function = functionName(it->function);
            if (it->function >= 0 && it->function < (int)info.functions.size())
                f.nodeId = info.functions[(size_t)it->function].nodeId;
            f.line = it->line;
            f.column = it->column;
            f.lineNodeId = nodeAt(s.program, it->line);
            for (const auto& [slot, value] : it->values)
                f.variables.push_back(slotVariable(slot, value));
            snap.stack.push_back(std::move(f));
        }
    }

    if (s.program.debug)
        for (const auto& nodeId : s.program.watches)
        {
            Watch w;
            w.nodeId = nodeId;
            for (int slot = 0; slot < (int)info.slots.size(); ++slot)
            {
                const auto& d = info.slots[(size_t)slot];
                if (std::find(d.nodeIds.begin(), d.nodeIds.end(), nodeId) == d.nodeIds.end())
                    continue;
                w.hasValue = true;
                w.name = d.name;
                w.type = d.type;
                if (const auto found = s.latest.find(slot); found != s.latest.end())
                {
                    w.available = true;
                    w.value = found->second.value;
                    w.function = functionName(found->second.function);
                    w.line = found->second.line;
                }
                break;
            }
            snap.watches.push_back(std::move(w));
        }

    if (!isActive(s.state))
        snap.result = s.result;
    return snap;
}

// A value recorded by a debug compile (djehuti_dbg_*), on the worker.
void record(int slot, std::string value)
{
    auto* worker = currentWorker;
    if (worker == nullptr)
        return;
    auto& s = *worker->session;
    std::lock_guard<std::mutex> lock(s.mutex);
    Recorded r { value, -1, 0 };
    if (!s.frames.empty())
    {
        auto& top = s.frames.back();
        top.values[slot] = value;
        r.function = top.function;
        r.line = top.line;
    }
    s.latest[slot] = std::move(r);
}

void finishCancelled(Worker* worker);

// frust_dbg_tick: before every expression of every FRust program, on
// whatever thread runs it. Only the executor's worker does anything here.
void onTick(int line, int column)
{
    auto* worker = currentWorker;
    if (worker == nullptr)
        return;
    auto& s = *worker->session;
    if (s.cancel.load(std::memory_order_relaxed))
        finishCancelled(worker);
    if (!s.program.debug)
        return;

    std::unique_lock<std::mutex> lock(s.mutex);
    const int programLine = line - s.lineOffset;
    // Code outside the program's own calls (the header, the run() wrapper).
    if (s.frames.empty() || programLine < 1 || programLine > s.program.programLines)
        return;
    auto& top = s.frames.back();
    // A tick outside the current function's lines is not this call's: the
    // prologue of a call being entered (the djehuti_dbg_enter call itself)
    // or what is left of one that has just returned.
    const auto& functions = s.program.info.functions;
    if (top.function >= 0 && top.function < (int)functions.size())
    {
        const auto& fn = functions[(size_t)top.function];
        if (fn.firstLine > 0 && (programLine < fn.firstLine || programLine > fn.lastLine))
            return;
        // A function with a body: its signature line is the prologue (the
        // parameters being recorded); execution stops on the body's lines,
        // with the parameters in view. A one-line function stops on its line.
        if (fn.firstLine > 0 && programLine == fn.firstLine && fn.lastLine > fn.firstLine)
            return;
    }
    const bool newLine = top.line != programLine;
    top.line = programLine;
    top.column = column;
    if (!newLine && !s.pauseAtNextLine)
        return; // the rest of a line already stopped at, or already passed

    const size_t depth = s.frames.size();
    std::string reason;
    if (s.pauseAtNextLine)
        reason = "step";
    else
    {
        for (auto& bp : s.program.breakpoints)
            if (bp.enabled && bp.line == programLine)
            {
                ++bp.hits;
                reason = "breakpoint";
            }
        if (reason.empty() && s.stepping)
        {
            if (s.step == Command::StepInto
                || (s.step == Command::StepOver && depth <= s.stepDepth)
                || (s.step == Command::StepOut && depth < s.stepDepth))
                reason = "step";
        }
        if (reason.empty() && s.pauseRequested)
            reason = "pause";
    }
    if (reason.empty())
        return;

    s.pauseAtNextLine = false;
    s.stepping = false;
    s.pauseRequested = false;
    s.state = State::Paused;
    s.pauseReason = reason;
    s.pausedLine = programLine;
    s.pausedColumn = column;
    lock.unlock();
    Executor::instance().publish(worker->session);
    lock.lock();
    // The worker waits here; Continue, a step or Stop wakes it.
    s.resumed.wait(lock, [&s] { return s.state != State::Paused || s.cancel.load(); });
    lock.unlock();
    if (s.cancel.load())
        finishCancelled(worker);
}

extern "C" std::int64_t djehuti_dbg_enter(std::int64_t function)
{
    if (auto* worker = currentWorker)
    {
        auto& s = *worker->session;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.frames.push_back({ (int)function });
    }
    return 0;
}

extern "C" std::int64_t djehuti_dbg_leave(std::int64_t)
{
    if (auto* worker = currentWorker)
    {
        auto& s = *worker->session;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.frames.empty())
            s.frames.pop_back();
        // A step that leaves the call it was in stops in the caller, at the
        // line the call returns to.
        if (s.stepping && s.frames.size() < s.stepDepth && !s.frames.empty())
            s.pauseAtNextLine = true;
    }
    return 0;
}

extern "C" std::int64_t djehuti_dbg_i64(std::int64_t slot, std::int64_t value)
{
    if (auto* worker = currentWorker)
        record((int)slot, formatValue(worker->session->program, (int)slot, value));
    return 0;
}

extern "C" std::int64_t djehuti_dbg_f64(std::int64_t slot, double value)
{
    std::ostringstream text;
    text << std::setprecision(15) << value;
    record((int)slot, text.str());
    return 0;
}

extern "C" std::int64_t djehuti_dbg_bool(std::int64_t slot, std::int64_t value)
{
    record((int)slot, value != 0 ? "true" : "false");
    return 0;
}

extern "C" std::int64_t djehuti_dbg_str(std::int64_t slot, const char* value)
{
    record((int)slot, "\"" + std::string(value != nullptr ? value : "") + "\"");
    return 0;
}

const std::vector<std::string>& debugHostFunctions()
{
    static const std::vector<std::string> names { "djehuti_dbg_enter", "djehuti_dbg_leave", "djehuti_dbg_i64",
                                                  "djehuti_dbg_f64", "djehuti_dbg_bool", "djehuti_dbg_str" };
    return names;
}

void installHooks()
{
    static std::once_flag once;
    std::call_once(once, [] {
        frust_plugin_register_host_function("djehuti_dbg_enter", reinterpret_cast<void*>(&djehuti_dbg_enter));
        frust_plugin_register_host_function("djehuti_dbg_leave", reinterpret_cast<void*>(&djehuti_dbg_leave));
        frust_plugin_register_host_function("djehuti_dbg_i64", reinterpret_cast<void*>(&djehuti_dbg_i64));
        frust_plugin_register_host_function("djehuti_dbg_f64", reinterpret_cast<void*>(&djehuti_dbg_f64));
        frust_plugin_register_host_function("djehuti_dbg_bool", reinterpret_cast<void*>(&djehuti_dbg_bool));
        frust_plugin_register_host_function("djehuti_dbg_str", reinterpret_cast<void*>(&djehuti_dbg_str));
        // Whichever frust_dbg_tick a compiled unit links to (the app's
        // registration or the runtime's export) comes here.
        frust_engine::setTickHandler(&onTick);
        g_frust_dbg_callback = &onTick;
    });
}

void finish(Worker* worker, frust_engine::Result result)
{
    auto& s = *worker->session;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        const bool stopped = s.cancel.load() && !result.ok;
        if (stopped && result.error.empty())
            result.error = "Stopped.";
        s.result = std::move(result);
        s.state = stopped ? State::Cancelled : (s.result.ok ? State::Completed : State::Failed);
        s.frames.clear();
    }
    Executor::instance().publish(worker->session);
}

// Stop, on the worker, with the program's code on the stack: the program
// is ended here (its frames are not returned to), standard output restored
// and the unit unloaded, and the thread ends.
void finishCancelled(Worker* worker)
{
    frust_engine::Result result;
    result.compiled = true;
    result.output = frust_engine::abandonRunningScript();
    result.error = "Stopped.";
    {
        auto& s = *worker->session;
        std::lock_guard<std::mutex> lock(s.mutex);
        s.result = std::move(result);
        s.state = State::Cancelled;
        s.frames.clear();
    }
    Executor::instance().publish(worker->session);
    currentWorker = nullptr;
    delete worker;
    _endthreadex(0);
}

unsigned __stdcall workerMain(void* argument)
{
    auto* worker = static_cast<Worker*>(argument);
    currentWorker = worker;
    auto session = worker->session;
    frust_engine::RunOptions options;
    options.prelude = session->program.prelude;
    options.hostFunctions = session->program.hostFunctions;
    options.shouldRun = [session] { return !session->cancel.load(); };
    options.beforeRun = [session](int lineOffset) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->lineOffset = lineOffset;
            session->state = State::Running;
        }
        Executor::instance().publish(session);
    };
    auto result = frust_engine::runScript(session->program.script, options);
    finish(worker, std::move(result));
    currentWorker = nullptr;
    delete worker;
    return 0;
}
}

struct Executor::Waiter
{
    std::function<bool(const Snapshot&)> until;
    std::function<void(const Snapshot&, bool)> done;
    bool finished = false;
};

Executor& Executor::instance()
{
    // Never destroyed: a worker may still be ending when the app exits.
    static auto* executor = new Executor();
    return *executor;
}

Executor::Executor()
{
    poster = [](std::function<void()> fn) { juce::MessageManager::callAsync(std::move(fn)); };
    installHooks();
}

void Executor::setPoster(Poster p)
{
    std::lock_guard<std::mutex> lock(mutex);
    poster = std::move(p);
}

bool Executor::start(Program program, std::string& error)
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (current != nullptr)
        {
            std::lock_guard<std::mutex> sessionLock(current->mutex);
            if (isActive(current->state))
            {
                error = "A FRust program is already " + std::string(stateName(current->state)) + " (" + current->program.label
                      + "); stop it or let it finish first.";
                return false;
            }
        }
        session = std::make_shared<Session>();
        session->id = nextSession++;
        if (program.debug)
            program.hostFunctions = debugHostFunctions();
        session->program = std::move(program);
        current = session;
    }
    publish(session);
    auto* worker = new Worker { session };
    const auto handle = _beginthreadex(nullptr, 0, &workerMain, worker, 0, nullptr);
    if (handle == 0)
    {
        delete worker;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->state = State::Failed;
            session->result.error = "Could not start the FRust worker thread.";
        }
        publish(session);
        error = "Could not start the FRust worker thread.";
        return false;
    }
    CloseHandle(reinterpret_cast<HANDLE>(handle));
    return true;
}

bool Executor::command(Command c, std::string& error)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        s = current;
    }
    if (s == nullptr)
    {
        error = "No FRust program is running.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (s->state != State::Paused)
        {
            error = "The program is " + std::string(stateName(s->state)) + ", not paused.";
            return false;
        }
        s->stepping = c != Command::Continue;
        s->step = c;
        s->stepDepth = s->frames.size();
        s->pauseAtNextLine = false;
        s->state = State::Running;
    }
    s->resumed.notify_all();
    publish(s);
    return true;
}

bool Executor::pause(std::string& error)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        s = current;
    }
    if (s == nullptr)
    {
        error = "No FRust program is running.";
        return false;
    }
    std::lock_guard<std::mutex> lock(s->mutex);
    if (!s->program.debug)
    {
        error = "Pause needs a debug session (Start Debugging); this program was started with Run.";
        return false;
    }
    if (s->state != State::Running)
    {
        error = "The program is " + std::string(stateName(s->state)) + ", not running.";
        return false;
    }
    s->pauseRequested = true;
    return true;
}

bool Executor::stop(std::string& error)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        s = current;
    }
    if (s == nullptr)
    {
        error = "No FRust program is running.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (!isActive(s->state))
        {
            error = "The program has already " + std::string(stateName(s->state)) + ".";
            return false;
        }
        s->cancel = true;
    }
    s->resumed.notify_all();
    return true;
}

bool Executor::updateMarkers(const std::string& programId, const std::vector<Breakpoint>& breakpoints,
                             const std::vector<std::string>& watches)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        s = current;
    }
    if (s == nullptr)
        return false;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (!isActive(s->state) || !s->program.debug || s->program.programId != programId)
            return false;
        std::vector<Breakpoint> next;
        for (auto bp : breakpoints)
        {
            bp.line = 0;
            for (const auto& [line, nodeId] : s->program.lineNodes)
                if (nodeId == bp.nodeId)
                {
                    bp.line = line;
                    break;
                }
            for (const auto& old : s->program.breakpoints)
                if (old.nodeId == bp.nodeId)
                    bp.hits = old.hits;
            next.push_back(bp);
        }
        s->program.breakpoints = std::move(next);
        s->program.watches = watches;
    }
    publish(s);
    return true;
}

Snapshot Executor::snapshot() const
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex);
        s = current;
    }
    if (s == nullptr)
        return {};
    std::lock_guard<std::mutex> lock(s->mutex);
    return snapshotOf(*s);
}

bool Executor::busy() const
{
    return isActive(snapshot().state);
}

void Executor::publish(const std::shared_ptr<Session>& session)
{
    Snapshot snap;
    {
        std::lock_guard<std::mutex> lock(session->mutex);
        snap = snapshotOf(*session);
    }
    Poster p;
    {
        std::lock_guard<std::mutex> lock(mutex);
        p = poster;
    }
    if (p != nullptr)
        p([this, snap] { deliver(snap); });
}

void Executor::deliver(const Snapshot& snap)
{
    // Message thread. A listener may add or remove listeners.
    const auto toCall = listeners;
    for (const auto& [id, listener] : toCall)
        if (listeners.count(id) != 0)
            listener(snap);
    // Waiters are checked against the state now, not the (possibly older)
    // snapshot that triggered this delivery.
    const auto now = snapshot();
    auto pending = waiters;
    for (auto& w : pending)
        if (!w->finished && w->until(now))
        {
            w->finished = true;
            w->done(now, false);
        }
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(), [](const auto& w) { return w->finished; }), waiters.end());
}

int Executor::addListener(std::function<void(const Snapshot&)> listener)
{
    const int id = nextListener++;
    listeners[id] = std::move(listener);
    return id;
}

void Executor::removeListener(int id)
{
    listeners.erase(id);
}

void Executor::whenState(std::function<bool(const Snapshot&)> until, int timeoutMs,
                         std::function<void(const Snapshot&, bool timedOut)> done)
{
    const auto now = snapshot();
    if (until(now))
    {
        done(now, false);
        return;
    }
    auto waiter = std::make_shared<Waiter>();
    waiter->until = std::move(until);
    waiter->done = std::move(done);
    waiters.push_back(waiter);
    juce::Timer::callAfterDelay(juce::jmax(1, timeoutMs), [this, waiter] {
        if (waiter->finished)
            return;
        waiter->finished = true;
        waiters.erase(std::remove(waiters.begin(), waiters.end(), waiter), waiters.end());
        waiter->done(snapshot(), true);
    });
}

void Executor::shutdown(std::function<void()> then, int timeoutMs)
{
    if (!busy())
    {
        then();
        return;
    }
    std::string ignored;
    stop(ignored);
    whenState([](const Snapshot& s) { return !isActive(s.state); }, timeoutMs,
              [then](const Snapshot&, bool) { then(); });
}

juce::var toVar(const Snapshot& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("session", (juce::int64)s.session);
    o->setProperty("state", stateName(s.state));
    o->setProperty("label", juce::String(s.label));
    o->setProperty("program", juce::String(s.programId));
    o->setProperty("debug", s.debug);
    if (s.state == State::Paused)
    {
        auto* at = new juce::DynamicObject();
        at->setProperty("reason", juce::String(s.pauseReason));
        at->setProperty("line", s.line);
        at->setProperty("column", s.column);
        at->setProperty("node", juce::String(s.nodeId));
        o->setProperty("paused", juce::var(at));
        juce::Array<juce::var> frames;
        for (size_t i = 0; i < s.stack.size(); ++i)
        {
            const auto& f = s.stack[i];
            auto* fo = new juce::DynamicObject();
            fo->setProperty("index", (int)i);
            fo->setProperty("function", juce::String(f.function));
            if (!f.nodeId.empty()) fo->setProperty("functionNode", juce::String(f.nodeId));
            fo->setProperty("line", f.line);
            fo->setProperty("column", f.column);
            fo->setProperty("node", juce::String(f.lineNodeId));
            juce::Array<juce::var> vars;
            for (const auto& v : f.variables)
            {
                auto* vo = new juce::DynamicObject();
                vo->setProperty("name", juce::String(v.name));
                vo->setProperty("type", juce::String(v.type));
                vo->setProperty("value", juce::String(v.value));
                juce::Array<juce::var> ids;
                for (const auto& id : v.nodeIds) ids.add(juce::String(id));
                vo->setProperty("nodes", ids);
                vars.add(juce::var(vo));
            }
            fo->setProperty("variables", vars);
            frames.add(juce::var(fo));
        }
        o->setProperty("stack", frames);
    }
    juce::Array<juce::var> bps;
    for (const auto& b : s.breakpoints)
    {
        auto* bo = new juce::DynamicObject();
        bo->setProperty("node", juce::String(b.nodeId));
        bo->setProperty("line", b.line);
        bo->setProperty("enabled", b.enabled);
        bo->setProperty("hits", b.hits);
        if (b.line <= 0) bo->setProperty("note", "This node has no code of its own in the generated program, so execution cannot stop at it.");
        bps.add(juce::var(bo));
    }
    o->setProperty("breakpoints", bps);
    juce::Array<juce::var> ws;
    for (const auto& w : s.watches)
    {
        auto* wo = new juce::DynamicObject();
        wo->setProperty("node", juce::String(w.nodeId));
        wo->setProperty("available", w.available);
        if (w.available)
        {
            wo->setProperty("name", juce::String(w.name));
            wo->setProperty("type", juce::String(w.type));
            wo->setProperty("value", juce::String(w.value));
            wo->setProperty("recordedIn", juce::String(w.function));
            wo->setProperty("recordedAtLine", w.line);
        }
        else
            wo->setProperty("note", w.hasValue ? "Not computed yet." : "This node produces no value in the generated program.");
        ws.add(juce::var(wo));
    }
    o->setProperty("watches", ws);
    if (!isActive(s.state) && s.state != State::Idle)
    {
        o->setProperty("ok", s.result.ok);
        o->setProperty("output", juce::String(s.result.output));
        if (!s.result.error.empty() || !s.result.diagnostics.empty())
            o->setProperty("error", juce::String(s.result.report()));
        o->setProperty("runMs", s.result.runMs);
    }
    return juce::var(o);
}
}
