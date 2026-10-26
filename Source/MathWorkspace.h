#pragma once

// The mathematical workspace of one worksheet: it holds the snapshots of the math cells, analyses each of
// them with the frontend (resolving names across cells), maintains the dependency graph, reports circular and
// conflicting definitions, and keeps computed results together with the exact state they were computed from.
//
// The workspace is independent of the GUI and of every solver library. It is fed immutable snapshots
// (math_doc::MathSnapshot) and Registry engines (math_solver); it never reads or changes the document. A
// result is stored only against a Ticket that is still current: an edit to the cell, or to anything it
// depends on, makes older tickets stale, so a late answer to an old question is recognised and dropped.

#include "MathDeps.h"
#include "MathFrontend.h"
#include "MathSolver.h"

namespace math_workspace
{
// What a result is an answer to: the cell, its revision, and the stamp of everything it depends on.
struct Ticket
{
    std::string cell;
    std::uint64_t revision = 0;
    std::uint64_t stamp = 0;
    bool valid = false;
};

enum class ResultStatus { Missing, Current, Stale, Blocked };
const char* resultStatusName(ResultStatus status);

struct StoredResult
{
    math_value::Value value;
    Ticket ticket;
};

// The outcome of evaluating a cell together with the cells it depends on.
struct Evaluation
{
    math_solver::Response response;
    Ticket ticket;
    bool stored = false;                       // the result was current and has been kept
    std::vector<std::string> computed;         // dependency cells that had to be evaluated first, in order
};

class Workspace final : private math_frontend::Resolver
{
public:
    explicit Workspace(math_frontend::Options options = {});

    // Adds a cell or updates it when its revision or content changed. Returns the ids whose analysis and
    // results this invalidates (the cell and everything that depended on it), empty when nothing changed.
    std::vector<std::string> setCell(const math_doc::MathSnapshot& snapshot);
    std::vector<std::string> removeCell(const std::string& id);
    void clear();
    bool has(const std::string& id) const { return cells.count(id) > 0; }
    std::vector<std::string> cellIds() const;                   // sorted

    // Analyses whatever changed. Called by the accessors below; harmless to call again.
    void refresh();
    std::shared_ptr<const math_frontend::CellAnalysis> analysis(const std::string& id);
    // The cell's own diagnostics plus those that only the whole worksheet can see: circular definitions,
    // duplicate definitions, conflicting declarations, dependence on a circular definition.
    std::vector<math_ir::Diagnostic> diagnostics(const std::string& id);
    bool complete(const std::string& id);                       // analysed with no placeholder and no error of any kind
    const math_deps::Graph& graph();

    // ---- revision-aware results -------------------------------------------------------------------------------------------------------
    Ticket ticket(const std::string& id);
    // Stores the value if the ticket is still current. False (and nothing stored) for a stale ticket.
    bool submitResult(const Ticket& ticket, math_value::Value value);
    ResultStatus resultStatus(const std::string& id);
    const StoredResult* result(const std::string& id);          // only a current result; null otherwise
    // Evaluates a cell, first evaluating the cells whose definitions it needs (reusing current results).
    Evaluation evaluate(const std::string& id, const math_solver::Registry& registry,
                        math_solver::Operation op = math_solver::Operation::Evaluate,
                        std::shared_ptr<math_solver::CancelToken> cancel = nullptr);

    const math_doc::MathSnapshot* snapshot(const std::string& id) const;

private:
    struct CellState
    {
        math_doc::MathSnapshot snapshot;
        std::uint64_t contentHash = 0;
        std::shared_ptr<const math_frontend::CellAnalysis> analysis, header;
        bool dirty = true, headerDirty = true, justAnalysed = false;
        std::uint64_t analysedStamp = 0;
        std::shared_ptr<StoredResult> stored;
    };

    bool definition(const std::string& name, const std::string& fromCell, math_frontend::Definition& out) override;
    bool declaration(const std::string& name, math_frontend::Declaration& out) override;
    std::shared_ptr<const math_frontend::CellAnalysis> ensure(const std::string& id);
    void rebuildDeclarations();
    void rebuildGraph();
    void rebuildWorkspaceDiagnostics();

    math_frontend::Options options;
    std::map<std::string, CellState> cells;
    math_deps::Graph graphState;
    std::map<std::string, math_frontend::Declaration> declared;
    std::map<std::string, std::vector<math_ir::Diagnostic>> extra;
    std::set<std::string> analysing;
    bool graphDirty = true;
};
}
