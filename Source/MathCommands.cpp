#include "MathCommands.h"

#include "MathLinear.h"
#include "MathResult.h"

#include <set>

namespace math_commands
{
namespace
{
juce::var sourceVar(const math_ir::Source& s)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("cell", juce::String(s.cell));
    o->setProperty("node", (juce::int64) s.node);
    o->setProperty("begin", (int) s.begin);
    o->setProperty("end", (int) s.end);
    return juce::var(o);
}

juce::var diagnosticVar(const math_ir::Diagnostic& d)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("severity", math_ir::severityName(d.severity));
    o->setProperty("code", juce::String(d.code));
    o->setProperty("message", juce::String(d.message));
    o->setProperty("source", sourceVar(d.where));
    return juce::var(o);
}

juce::var stringsVar(const std::set<std::string>& values)
{
    juce::Array<juce::var> a;
    for (const auto& v : values) a.add(juce::String(v));
    return juce::var(a);
}
juce::var stringsVar(const std::vector<std::string>& values)
{
    juce::Array<juce::var> a;
    for (const auto& v : values) a.add(juce::String(v));
    return juce::var(a);
}

juce::String failure(const juce::String& message)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("ok", false);
    o->setProperty("message", message);
    return juce::JSON::toString(juce::var(o));
}

juce::var cellView(math_workspace::Workspace& workspace, const WorksheetCell& cell)
{
    const auto id = cell.id.toStdString();
    auto* o = new juce::DynamicObject();
    o->setProperty("id", cell.id);
    o->setProperty("role", math_doc::roleName(cell.math.role));
    o->setProperty("unit", cell.displayUnit);
    o->setProperty("digits", cell.displayDigits);
    const auto state = workspace.outcomeState(id);
    o->setProperty("state", math_workspace::outcomeStateName(state));
    const auto* outcome = workspace.outcome(id);
    if (outcome == nullptr || cell.source.trim().isEmpty())
    {
        o->setProperty("status", "not-evaluated");
        return juce::var(o);
    }
    const auto& response = outcome->response;
    o->setProperty("status", math_solver::statusName(response.status));
    o->setProperty("kind", math_value::kindName(response.value.kind));
    o->setProperty("value", juce::String(response.value.describe()));
    o->setProperty("exact", response.value.isExact() && response.provenance.exact);
    o->setProperty("engine", juce::String(response.provenance.engine));
    if (!response.diagnostics.empty()) o->setProperty("message", juce::String(response.diagnostics[0].message));
    if (response.value.ok())
    {
        math_doc::IdAllocator ids;
        math_result::Options options;
        options.displayUnit = cell.displayUnit.toStdString();
        options.significantDigits = cell.displayDigits;
        const auto tree = math_result::fromValue(response.value, ids, {}, options);
        o->setProperty("typeset", juce::String(math_linear::toLinear(tree.lines)));
    }
    return juce::var(o);
}

juce::var capabilityVar(const math_solver::EngineCapability& c)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("engine", juce::String(c.engine));
    o->setProperty("version", juce::String(c.version));
    o->setProperty("operation", math_solver::operationName(c.capability.op));
    o->setProperty("support", math_solver::supportName(c.capability.support));
    o->setProperty("exact", c.capability.exact);
    o->setProperty("numeric", c.capability.numeric);
    o->setProperty("notes", juce::String(c.capability.notes));
    return juce::var(o);
}

juce::var provenanceVar(const math_value::Provenance& p)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("engine", juce::String(p.engine));
    o->setProperty("version", juce::String(p.version));
    o->setProperty("method", juce::String(p.method));
    o->setProperty("exact", p.exact);
    o->setProperty("cell", juce::String(p.cell));
    o->setProperty("cellRevision", (juce::int64) p.cellRevision);
    o->setProperty("elapsedMs", p.elapsedMs);
    return juce::var(o);
}
}

bool handles(const std::string& name)
{
    return name == "worksheet_math_analyze" || name == "worksheet_math_dependencies" || name == "worksheet_math_evaluate" || name == "worksheet_math_evaluate_all"
        || name == "worksheet_math_results" || name == "math_solver_capabilities";
}

void sync(const WorksheetDocument& document, math_workspace::Workspace& workspace)
{
    std::set<std::string> present;
    for (const auto& cell : document.cells)
    {
        if (cell.kind != "math") continue;
        math_doc::MathSnapshot snapshot;
        if (!document.snapshot(cell.id, snapshot)) continue;
        present.insert(snapshot.cellId);
        workspace.setCell(snapshot);
    }
    for (const auto& id : workspace.cellIds())
        if (present.count(id) == 0) workspace.removeCell(id);
}

juce::String execute(const std::string& name, const juce::var& args, WorksheetDocument& document, math_workspace::Workspace& workspace)
{
    static const auto registry = math_solver::defaultRegistry();
    sync(document, workspace);

    if (name == "math_solver_capabilities")
    {
        math_solver::Operation op;
        const bool filter = args.hasProperty("operation");
        if (filter && !math_solver::operationFromName(args["operation"].toString().toStdString(), op))
            return failure("operation must be evaluate, simplify, solve, differentiate, integrate or numeric.");
        juce::Array<juce::var> engines, rows;
        std::set<std::string> seen;
        for (const auto& c : registry.capabilities())
        {
            if (filter && c.capability.op != op) continue;
            rows.add(capabilityVar(c));
            if (seen.insert(c.engine).second) engines.add(juce::String(c.engine));
        }
        // operations nobody can do yet are listed so that a caller can tell "unsupported" from "unknown"
        juce::Array<juce::var> unavailable;
        for (auto o : { math_solver::Operation::Evaluate, math_solver::Operation::Simplify, math_solver::Operation::Solve,
                        math_solver::Operation::Differentiate, math_solver::Operation::Integrate, math_solver::Operation::Numeric })
            if (registry.discover(o).empty()) unavailable.add(juce::String(math_solver::operationName(o)));
        auto* result = new juce::DynamicObject();
        result->setProperty("ok", true);
        result->setProperty("engines", engines);
        result->setProperty("capabilities", rows);
        result->setProperty("operationsWithoutEngine", unavailable);
        return juce::JSON::toString(juce::var(result));
    }

    if (name == "worksheet_math_dependencies")
    {
        const auto& graph = workspace.graph();
        auto* result = new juce::DynamicObject();
        result->setProperty("ok", true);
        juce::Array<juce::var> cellsOut;
        for (const auto& id : graph.ids())
        {
            if (args.hasProperty("id") && args["id"].toString().toStdString() != id) continue;
            const auto* names = graph.find(id);
            const auto analysis = workspace.analysis(id);
            auto* o = new juce::DynamicObject();
            o->setProperty("id", juce::String(id));
            o->setProperty("revision", (juce::int64) names->revision);
            o->setProperty("role", math_doc::roleName(analysis->role));
            o->setProperty("defines", stringsVar(names->defines));
            o->setProperty("declares", stringsVar(names->declares));
            o->setProperty("uses", stringsVar(names->uses));
            o->setProperty("dependsOn", stringsVar(graph.dependenciesOf(id)));
            o->setProperty("dependents", stringsVar(graph.dependentsOf(id)));
            o->setProperty("inCycle", graph.inCycle(id));
            o->setProperty("blocked", graph.blocked(id));
            o->setProperty("result", math_workspace::resultStatusName(workspace.resultStatus(id)));
            cellsOut.add(juce::var(o));
        }
        result->setProperty("cells", cellsOut);
        result->setProperty("order", stringsVar(graph.order()));
        juce::Array<juce::var> cycles, conflicts;
        for (const auto& c : graph.cycles()) cycles.add(stringsVar(c));
        for (const auto& c : graph.conflicts())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("name", juce::String(c.name));
            o->setProperty("cells", stringsVar(c.cells));
            conflicts.add(juce::var(o));
        }
        result->setProperty("cycles", cycles);
        result->setProperty("conflicts", conflicts);
        return juce::JSON::toString(juce::var(result));
    }

    if (name == "worksheet_math_evaluate_all")
    {
        math_workspace::EvaluateOptions options;
        options.timeoutMs = 5000;
        options.force = (bool) args["force"];
        const auto before = workspace.stats();
        const auto batch = workspace.evaluateAll(registry, options);
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", batch.failed.empty());
        o->setProperty("message", batch.failed.empty() ? juce::String("Every math cell is current.") : juce::String((int) batch.failed.size()) + " cell(s) could not be calculated.");
        o->setProperty("evaluated", stringsVar(batch.evaluated));
        o->setProperty("reused", stringsVar(batch.reused));
        o->setProperty("failed", stringsVar(batch.failed));
        o->setProperty("skipped", stringsVar(batch.skipped));
        auto* stats = new juce::DynamicObject();
        stats->setProperty("engineRuns", (juce::int64) (workspace.stats().engineRuns - before.engineRuns));
        stats->setProperty("cellsComputed", (juce::int64) (workspace.stats().evaluations - before.evaluations));
        o->setProperty("stats", juce::var(stats));
        juce::Array<juce::var> cellsOut;
        for (const auto& cell : document.cells)
            if (cell.kind == "math") cellsOut.add(cellView(workspace, cell));
        o->setProperty("cells", cellsOut);
        return juce::JSON::toString(juce::var(o));
    }

    if (name == "worksheet_math_results")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", true);
        juce::Array<juce::var> cellsOut;
        for (const auto& cell : document.cells)
            if (cell.kind == "math" && (!args.hasProperty("id") || args["id"].toString() == cell.id)) cellsOut.add(cellView(workspace, cell));
        o->setProperty("cells", cellsOut);
        return juce::JSON::toString(juce::var(o));
    }

    const auto id = args["id"].toString().toStdString();
    if (!workspace.has(id)) return failure("The id is not a math cell.");

    if (name == "worksheet_math_analyze")
    {
        const auto analysis = workspace.analysis(id);
        auto* o = new juce::DynamicObject();
        o->setProperty("ok", true);
        o->setProperty("cellId", juce::String(id));
        o->setProperty("revision", (juce::int64) analysis->revision);
        o->setProperty("role", math_doc::roleName(analysis->role));
        o->setProperty("complete", workspace.complete(id));
        o->setProperty("hasPlaceholder", analysis->hasPlaceholder);
        o->setProperty("type", juce::String(analysis->type.toString()));
        o->setProperty("defines", stringsVar(analysis->defines));
        o->setProperty("declares", stringsVar(analysis->declares));
        o->setProperty("uses", stringsVar(analysis->uses));
        juce::Array<juce::var> lines, diagnostics;
        for (const auto& l : analysis->lines) lines.add(juce::String(math_ir::dump(l)));
        for (const auto& d : workspace.diagnostics(id)) diagnostics.add(diagnosticVar(d));
        o->setProperty("ir", juce::String(math_ir::dump(analysis->ir)));
        o->setProperty("lines", lines);
        o->setProperty("diagnostics", diagnostics);
        return juce::JSON::toString(juce::var(o));
    }

    // worksheet_math_evaluate
    math_solver::Operation op = math_solver::Operation::Evaluate;
    if (args.hasProperty("operation") && (!math_solver::operationFromName(args["operation"].toString().toStdString(), op)
                                          || (op != math_solver::Operation::Evaluate && op != math_solver::Operation::Numeric)))
        return failure("operation must be evaluate or numeric.");
    const int cellIndex = document.indexOf(args["id"].toString());
    if (cellIndex >= 0)
    {
        auto& cell = document.cells[(size_t) cellIndex];
        const auto unit = args.hasProperty("display_unit") ? args["display_unit"].toString().trim().substring(0, 40) : cell.displayUnit;
        int digits = args.hasProperty("digits") ? (int) args["digits"] : cell.displayDigits;
        digits = digits >= 3 && digits <= 17 ? digits : 15;
        if (unit != cell.displayUnit || digits != cell.displayDigits) { cell.displayUnit = unit; cell.displayDigits = digits; document.modified = true; }
    }
    math_workspace::EvaluateOptions options;
    options.op = op;
    options.timeoutMs = 5000;
    options.force = (bool) args["force"];
    const auto evaluation = workspace.evaluate(id, registry, options);
    const auto& response = evaluation.response;
    auto* o = new juce::DynamicObject();
    const bool ok = response.status == math_solver::Status::Ok && response.value.ok();
    o->setProperty("ok", ok);
    o->setProperty("message", ok ? juce::String("Evaluated.")
                                 : juce::String(response.diagnostics.empty() ? math_solver::statusName(response.status) : response.diagnostics[0].message));
    o->setProperty("status", math_solver::statusName(response.status));
    o->setProperty("kind", math_value::kindName(response.value.kind));
    o->setProperty("value", juce::String(response.value.describe()));
    o->setProperty("exact", response.value.isExact() && response.provenance.exact);
    o->setProperty("stored", evaluation.stored);
    o->setProperty("reused", evaluation.reused);
    o->setProperty("computedFirst", stringsVar(evaluation.computed));
    auto* ticket = new juce::DynamicObject();
    ticket->setProperty("revision", (juce::int64) evaluation.ticket.revision);
    ticket->setProperty("stamp", juce::String::toHexString((juce::int64) evaluation.ticket.stamp));
    o->setProperty("ticket", juce::var(ticket));
    o->setProperty("provenance", provenanceVar(response.provenance));
    juce::Array<juce::var> diagnostics;
    for (const auto& d : response.diagnostics) diagnostics.add(diagnosticVar(d));
    o->setProperty("diagnostics", diagnostics);
    if (response.value.ok())
    {
        math_doc::IdAllocator ids;
        math_result::Options options;
        if (cellIndex >= 0)
        {
            options.displayUnit = document.cells[(size_t) cellIndex].displayUnit.toStdString();
            options.significantDigits = document.cells[(size_t) cellIndex].displayDigits;
        }
        const auto tree = math_result::fromValue(response.value, ids, {}, options);
        o->setProperty("typeset", juce::String(math_linear::toLinear(tree.lines)));
        if (!tree.problem.empty()) o->setProperty("displayNote", juce::String(tree.problem));
    }
    return juce::JSON::toString(juce::var(o));
}
}
