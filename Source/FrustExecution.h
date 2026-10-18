#pragma once

// FRust program execution and the FRust debugger.
//
// Every FRust program the app runs goes through the one Executor: the Frust
// panel's Run, the Node Designer's Compile & Run and Start Debugging, and the
// agent's frust_run, node_program_run and node_debug_* tools. A program runs
// on a worker thread, never on the JUCE message thread, one program at a time;
// it is compiled and run by frust_engine::runScript, as before.
//
// The debugger is FrustIDE's DebuggerController (commit 431543f) adapted. As
// there, the FRust compiler's frust_dbg_tick(line, column), called before
// every expression, blocks the program's thread at a breakpoint until it is
// told to go on. Here:
//   - only the worker thread ever waits; the message thread is told about
//     every change asynchronously (the poster) and never blocks;
//   - a breakpoint belongs to a program and a node, and is placed at the
//     node's line in that program's generated code (the node compiler's
//     source map, with the header the run adds in front taken off);
//   - execution stops once per line, not once per expression;
//   - Step Into / Over / Out and the call stack come from the calls a debug
//     compile of a node program adds (node_compiler DebugInfo): entering and
//     leaving each generated function. Variables and watches are the values
//     the same compile records as they are computed. Nothing is read from the
//     source text or the editor;
//   - Stop releases a paused program and stops a running one at its next
//     expression (see stop()).

#include <JuceHeader.h>

#include "FrustEngine.h"
#include "NodeCompiler.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace frust_exec
{
// Idle -> Compiling -> Running <-> Paused -> Completed / Failed / Cancelled
enum class State { Idle, Compiling, Running, Paused, Completed, Failed, Cancelled };
const char* stateName(State state);
bool isActive(State state); // Compiling, Running or Paused

enum class Command { Continue, StepInto, StepOver, StepOut };
const char* commandName(Command command);

struct Breakpoint
{
    std::string nodeId;
    int line = 0;         // the node's line in the generated program; 0 when the node has no code of its own
    bool enabled = true;
    int hits = 0;
};

struct Variable
{
    std::string name;
    std::string type;
    std::string value;
    std::vector<std::string> nodeIds; // the nodes whose value it is
};

struct Frame
{
    std::string function; // the generated FRust function
    std::string nodeId;   // the node it belongs to, if any
    int line = 0;         // where it is in the program (its call, for a caller)
    int column = 0;
    std::string lineNodeId; // the node at that line, if any
    std::vector<Variable> variables; // values recorded in this call so far
};

struct Watch
{
    std::string nodeId;
    bool available = false;  // a value has been recorded
    bool hasValue = false;   // the node produces a value in the generated code
    std::string name, type, value;
    std::string function;    // where it was recorded
    int line = 0;
};

// What to run.
struct Program
{
    std::string label;     // shown (file name)
    std::string programId; // the program breakpoints belong to (its file, or its label)
    std::string script;    // FRust defining pub fn run() -> String
    int programLines = 0;  // the program's own lines (ticks past them are the run() wrapper)

    bool debug = false;
    std::string prelude;                     // debug compile: node_compiler::DebugPrelude()
    std::vector<std::string> hostFunctions;  // debug compile: the debugger's host functions
    node_compiler::DebugInfo info;
    std::map<int, std::string> lineNodes;    // program line -> node
    std::vector<Breakpoint> breakpoints;
    std::vector<std::string> watches;        // node ids
};

struct Snapshot
{
    std::uint64_t session = 0;
    State state = State::Idle;
    std::string label, programId;
    bool debug = false;

    // Paused:
    std::string pauseReason; // breakpoint, step or pause
    int line = 0, column = 0;
    std::string nodeId;
    std::vector<Frame> stack; // innermost first

    std::vector<Breakpoint> breakpoints;
    std::vector<Watch> watches;

    // Completed / Failed / Cancelled:
    frust_engine::Result result;
};

class Executor
{
public:
    // The app's executor (it lives as long as the process: a worker thread
    // never outlives it).
    static Executor& instance();

    using Poster = std::function<void(std::function<void()>)>;
    // How changes reach the message thread; juce::MessageManager::callAsync
    // unless a test sets its own.
    void setPoster(Poster poster);

    // Starts the program on a new worker thread. Refused while another
    // program is compiling, running or paused.
    bool start(Program program, std::string& error);

    // Paused: go on (Continue), or to the next line in this call or any call
    // it makes (Step Into), the next line in this call or its caller (Step
    // Over), the caller (Step Out). False with error when not paused.
    bool command(Command command, std::string& error);
    // Running (debug session): stop at the next line the program reaches.
    bool pause(std::string& error);
    // Stops the program: a paused one is released and ended; a running one
    // ends at its next expression (the FRust compiler puts a check before
    // every expression). Code that never reaches an expression (a call into
    // native code that does not return) cannot be stopped. The ended thread's
    // FRust frames are not unwound: the program's own allocations are left.
    bool stop(std::string& error);

    // A program's breakpoints and watches changed (in the editor or by the
    // agent): a debug session of that program follows them at once. A
    // breakpoint's line is found from the session's own node lines; hit
    // counts are kept. True when a running session took the change.
    bool updateMarkers(const std::string& programId, const std::vector<Breakpoint>& breakpoints,
                       const std::vector<std::string>& watches);

    Snapshot snapshot() const;
    bool busy() const;

    // Message thread. Listeners are called on the message thread on every
    // change (state, pause, finish).
    int addListener(std::function<void(const Snapshot&)> listener);
    void removeListener(int id);
    // Message thread: `done` is called (message thread) once the state
    // satisfies `until`, or with timedOut after timeoutMs.
    void whenState(std::function<bool(const Snapshot&)> until, int timeoutMs,
                   std::function<void(const Snapshot&, bool timedOut)> done);
    // Message thread: stops any program, then calls `then` once its worker has
    // ended (or after timeoutMs if it cannot end, e.g. in native code).
    void shutdown(std::function<void()> then, int timeoutMs);

    // The running program's state (FrustExecution.cpp); publish posts its
    // snapshot to the message thread.
    struct Session;
    void publish(const std::shared_ptr<Session>& session);

private:
    Executor();
    void deliver(const Snapshot& snapshot);

    mutable std::mutex mutex;
    std::shared_ptr<Session> current;
    std::uint64_t nextSession = 1;
    Poster poster;

    // Message thread only.
    std::map<int, std::function<void(const Snapshot&)>> listeners;
    int nextListener = 1;
    struct Waiter;
    std::vector<std::shared_ptr<Waiter>> waiters;
};

// JSON for the agent tools and the panel.
juce::var toVar(const Snapshot& snapshot);
}
