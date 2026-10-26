#pragma once

// The solver-independent computation interface of Djehuti Math. Any engine, in this process or not, that can
// answer mathematical questions implements `Solver`: it states what it can do (capability discovery), is
// given a Request made of MathIR and Values, and returns a Response with a Value, structured diagnostics and
// the provenance of the answer. Requests can be cancelled and can carry a deadline. The registry chooses
// among the registered engines and stamps every response with the cell and revision it answers.
//
// Nothing here knows about a particular library, the GUI, the document or FRust. S0 registers one engine, a
// deliberately small reference evaluator (exact and floating-point arithmetic on fully numeric expressions)
// that exercises the whole path and gives the checks something real to run; symbolic and numerical engines
// arrive in later milestones as further implementations of this interface.

#include "MathValue.h"

#include <atomic>
#include <chrono>
#include <functional>

namespace math_solver
{
enum class Operation : std::uint8_t { Evaluate, Simplify, Solve, Differentiate, Integrate, Numeric };
const char* operationName(Operation op);
bool operationFromName(const std::string& name, Operation& out);

enum class Support : std::uint8_t { None, Partial, Full };
const char* supportName(Support s);

struct Capability
{
    Operation op = Operation::Evaluate;
    Support support = Support::None;
    bool exact = false;        // can return mathematically exact results
    bool numeric = false;      // can return approximate results
    std::string notes;         // limits, in plain language
};

// Cooperative cancellation: the requester calls request(); engines poll requested().
class CancelToken
{
public:
    void request() { flag.store(true); }
    bool requested() const { return flag.load(); }
private:
    std::atomic<bool> flag { false };
};

struct NumericOptions
{
    int precisionBits = 53;               // 53 = IEEE double
    double relativeTolerance = 1e-10;
    double absoluteTolerance = 1e-12;
    int maxIterations = 1000;
};

// A function defined by a worksheet cell, available to engines that can evaluate it.
struct FunctionDefinition
{
    std::vector<std::string> params;
    math_ir::ExprPtr body;
};

struct Request
{
    Operation op = Operation::Evaluate;
    math_ir::ExprPtr expr;                                  // what to evaluate, simplify, differentiate, integrate; or one equation
    std::vector<math_ir::ExprPtr> equations;                // Solve: the system
    std::vector<std::string> variables;                     // Solve: the unknowns; Differentiate/Integrate: the variable
    std::map<std::string, math_value::Value> bindings;      // values of symbols
    std::map<std::string, FunctionDefinition> functions;    // user-defined functions the expression may call
    NumericOptions numeric;
    std::shared_ptr<CancelToken> cancel;
    std::chrono::steady_clock::time_point deadline {};
    bool hasDeadline = false;
    std::string cell;                                       // what the question is about
    std::uint64_t cellRevision = 0;
    bool interrupted() const;                               // cancelled, or past the deadline
};

enum class Status : std::uint8_t { Ok, Unsupported, Failed, Cancelled, TimedOut };
const char* statusName(Status s);

struct Response
{
    Status status = Status::Unsupported;
    math_value::Value value;
    std::vector<math_ir::Diagnostic> diagnostics;
    math_value::Provenance provenance;
};

class Solver
{
public:
    virtual ~Solver() = default;
    virtual std::string id() const = 0;
    virtual std::string version() const = 0;
    virtual int priority() const { return 0; }                     // higher wins between equal support
    virtual std::vector<Capability> capabilities() const = 0;
    // How well this engine can answer this particular request (it may inspect the expression).
    virtual Support assess(const Request& request) const;
    // In words, why this engine cannot answer the request (empty when it can, or when it cannot say).
    virtual std::string whyNot(const Request&) const { return {}; }
    virtual Response run(const Request& request) = 0;
};

struct EngineCapability
{
    std::string engine, version;
    Capability capability;
};

class Registry
{
public:
    void add(std::shared_ptr<Solver> solver);
    std::vector<EngineCapability> capabilities() const;                       // everything registered
    std::vector<EngineCapability> discover(Operation op) const;               // who can do `op`
    Solver* select(const Request& request) const;                            // the best engine, or null
    // Runs the best engine and stamps the provenance (cell, revision, elapsed time). Unsupported requests,
    // cancellation and deadlines come back as a Response with the matching Status and a diagnostic.
    Response run(const Request& request) const;
    size_t size() const { return solvers.size(); }
private:
    std::vector<std::shared_ptr<Solver>> solvers;
};

std::shared_ptr<Solver> makeReferenceEvaluator();
// A registry holding the built-in engines (S0: the reference evaluator).
Registry defaultRegistry();
}
