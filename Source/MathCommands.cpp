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
    return name == "worksheet_math_analyze" || name == "worksheet_math_dependencies" || name == "worksheet_math_evaluate" || name == "math_solver_capabilities";
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

juce::String execute(const std::string& name, const juce::var& args, const WorksheetDocument& document, math_workspace::Workspace& workspace)
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
    const auto evaluation = workspace.evaluate(id, registry, op);
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
        options.displayUnit = args["display_unit"].toString().toStdString();
        const auto tree = math_result::fromValue(response.value, ids, {}, options);
        o->setProperty("typeset", juce::String(math_linear::toLinear(tree.lines)));
        if (!tree.problem.empty()) o->setProperty("displayNote", juce::String(tree.problem));
    }
    return juce::JSON::toString(juce::var(o));
}
}
