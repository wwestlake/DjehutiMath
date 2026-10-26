#include "MathWorkspace.h"

#include <algorithm>

namespace math_workspace
{
using namespace math_frontend;
using math_ir::Diagnostic;

namespace
{
std::uint64_t mix(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

std::uint64_t hashText(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Identity of what a snapshot contains: node ids, kinds and text. Two worksheets that happen to reuse a cell
// id and a revision number are still told apart.
std::uint64_t contentHash(const math_doc::MathSnapshot& s)
{
    std::uint64_t h = mix((std::uint64_t) s.role, s.cellRevision);
    h = mix(h, hashText(s.cellId));
    for (const auto& line : s.lines)
        math_doc::forEach(line, [&h](const math_doc::Node& n) {
            h = mix(h, n.id);
            h = mix(h, (std::uint64_t) n.kind);
            h = mix(h, hashText(n.text));
            h = mix(h, hashText(n.text2));
        });
    return h;
}

std::string join(const std::vector<std::string>& v, const char* separator)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? separator : "") + v[i];
    return out;
}
}

const char* resultStatusName(ResultStatus status)
{
    switch (status)
    {
        case ResultStatus::Current: return "current";
        case ResultStatus::Stale: return "stale";
        case ResultStatus::Blocked: return "blocked";
        default: return "missing";
    }
}

Workspace::Workspace(Options o) : options(o) {}

std::vector<std::string> Workspace::setCell(const math_doc::MathSnapshot& snapshot)
{
    const auto hash = contentHash(snapshot);
    auto it = cells.find(snapshot.cellId);
    if (it != cells.end() && it->second.contentHash == hash)
        return {};
    std::set<std::string> invalidated { snapshot.cellId };
    if (graphState.has(snapshot.cellId))
        for (const auto& d : graphState.transitiveDependents(snapshot.cellId)) invalidated.insert(d);
    auto& state = cells[snapshot.cellId];
    state.snapshot = snapshot;
    state.contentHash = hash;
    state.dirty = state.headerDirty = true;
    graphDirty = true;
    return { invalidated.begin(), invalidated.end() };
}

std::vector<std::string> Workspace::removeCell(const std::string& id)
{
    if (cells.erase(id) == 0) return {};
    outcomes.erase(id);
    std::set<std::string> invalidated { id };
    if (graphState.has(id))
        for (const auto& d : graphState.transitiveDependents(id)) invalidated.insert(d);
    graphDirty = true;
    return { invalidated.begin(), invalidated.end() };
}

void Workspace::clear()
{
    cells.clear();
    outcomes.clear();
    graphState = math_deps::Graph();
    declared.clear();
    extra.clear();
    analysing.clear();
    graphDirty = false;
}

std::vector<std::string> Workspace::cellIds() const
{
    std::vector<std::string> out;
    for (const auto& c : cells) out.push_back(c.first);
    return out;
}

const math_doc::MathSnapshot* Workspace::snapshot(const std::string& id) const
{
    const auto it = cells.find(id);
    return it == cells.end() ? nullptr : &it->second.snapshot;
}

// ---- resolution across cells ---------------------------------------------------------------------------------------------------------
bool Workspace::definition(const std::string& name, const std::string& fromCell, Definition& out)
{
    for (auto& entry : cells)
    {
        if (entry.first == fromCell || entry.second.header == nullptr) continue;
        for (const auto& d : entry.second.header->definitions)
        {
            if (d.name != name) continue;
            if (analysing.count(entry.first) > 0) { out = d; return true; }   // a cycle: only the shape of the definition is known
            const auto full = ensure(entry.first);
            if (full != nullptr)
                for (const auto& fd : full->definitions)
                    if (fd.name == name) { out = fd; return true; }
            out = d;
            return true;
        }
    }
    return false;
}

bool Workspace::declaration(const std::string& name, Declaration& out)
{
    const auto it = declared.find(name);
    if (it == declared.end()) return false;
    out = it->second;
    return true;
}

std::shared_ptr<const CellAnalysis> Workspace::ensure(const std::string& id)
{
    auto it = cells.find(id);
    if (it == cells.end()) return nullptr;
    auto& state = it->second;
    if ((state.analysis == nullptr || state.dirty) && analysing.insert(id).second)
    {
        state.analysis = analyzeCell(state.snapshot, this, options);
        analysing.erase(id);
        state.dirty = false;
        state.justAnalysed = true;
    }
    return state.analysis;
}

void Workspace::rebuildDeclarations()
{
    declared.clear();
    for (const auto& entry : cells)
    {
        if (entry.second.header == nullptr) continue;
        for (const auto& d : entry.second.header->declarations)
        {
            auto& merged = declared[d.name];
            if (merged.name.empty()) { merged = d; continue; }
            if (d.hasDomain && !merged.hasDomain) { merged.hasDomain = true; merged.domain = d.domain; }
            if (d.hasSign && !merged.hasSign) { merged.hasSign = true; merged.sign = d.sign; }
            if (d.hasDim && !merged.hasDim) { merged.hasDim = true; merged.dim = d.dim; merged.unit = d.unit; }
        }
    }
}

void Workspace::rebuildGraph()
{
    graphState = math_deps::Graph();
    for (const auto& entry : cells)
    {
        if (entry.second.analysis == nullptr) continue;
        math_deps::CellNames names;
        names.id = entry.first;
        names.revision = entry.second.snapshot.cellRevision;
        names.defines = entry.second.analysis->defines;
        names.declares = entry.second.analysis->declares;
        names.uses = entry.second.analysis->uses;
        graphState.set(names);
    }
}

void Workspace::rebuildWorkspaceDiagnostics()
{
    extra.clear();
    auto add = [this](const std::string& cell, Diagnostic::Severity sev, const char* code, const std::string& message, const math_ir::Source& where) {
        extra[cell].push_back({ sev, code, message, where });
    };
    for (const auto& cycle : graphState.cycles())
    {
        std::vector<std::string> names;
        for (const auto& id : cycle)
        {
            const auto& defs = cells[id].analysis->defines;
            for (const auto& n : defs) names.push_back(n);
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        for (const auto& id : cycle)
        {
            math_ir::Source where;
            const auto& a = *cells[id].analysis;
            for (const auto& use : a.useSites)
                if (std::find(names.begin(), names.end(), use.first) != names.end()) { where = use.second; break; }
            add(id, Diagnostic::Severity::Error, "circular-definition", "Circular definition: " + join(names, ", ") + " depend on each other.", where);
        }
    }
    for (const auto& conflict : graphState.conflicts())
        for (const auto& id : conflict.cells)
        {
            const auto& a = *cells[id].analysis;
            const auto site = a.defineSites.find(conflict.name);
            add(id, Diagnostic::Severity::Error, "duplicate-definition", "'" + conflict.name + "' is defined in more than one cell.",
                site != a.defineSites.end() ? site->second : math_ir::Source());
        }
    // declarations of the same name that disagree
    std::map<std::string, std::vector<const Declaration*>> byName;
    for (const auto& entry : cells)
        if (entry.second.header != nullptr)
            for (const auto& d : entry.second.header->declarations) byName[d.name].push_back(&d);
    for (const auto& entry : byName)
        for (size_t i = 1; i < entry.second.size(); ++i)
        {
            const auto& a = *entry.second[0];
            const auto& b = *entry.second[i];
            const bool dim = a.hasDim && b.hasDim && a.dim != b.dim;
            const bool domain = a.hasDomain && b.hasDomain && a.domain != b.domain && !(a.hasDim || b.hasDim);
            if (dim || domain)
                add(b.cell, Diagnostic::Severity::Warning, "conflicting-declaration", "'" + entry.first + "' is declared differently elsewhere; the first declaration is used.", b.where);
        }
    for (const auto& entry : cells)
    {
        if (entry.second.analysis == nullptr || graphState.inCycle(entry.first) || !graphState.blocked(entry.first)) continue;
        add(entry.first, Diagnostic::Severity::Warning, "blocked", "This cell depends on a circular definition and cannot be computed.", math_ir::Source());
    }
}

void Workspace::refresh()
{
    bool pending = graphDirty;
    for (const auto& c : cells) pending = pending || c.second.dirty || c.second.headerDirty;
    if (!pending) return;
    for (auto& entry : cells)
    {
        auto& state = entry.second;
        if (!state.headerDirty) continue;
        const bool defining = state.snapshot.role == math_doc::Role::Definition || state.snapshot.role == math_doc::Role::Assumption;
        state.header = defining ? analyzeCell(state.snapshot, nullptr, options) : nullptr;
        state.headerDirty = false;
    }
    rebuildDeclarations();
    for (int pass = 0; pass < 8; ++pass)
    {
        for (auto& entry : cells) if (entry.second.dirty || entry.second.analysis == nullptr) ensure(entry.first);
        rebuildGraph();
        bool again = false;
        for (auto& entry : cells)
        {
            auto& state = entry.second;
            const auto stamp = graphState.stamp(entry.first);
            if (state.justAnalysed) { state.analysedStamp = stamp; state.justAnalysed = false; }
            else if (stamp != state.analysedStamp) { state.dirty = true; again = true; }
        }
        if (!again) break;
    }
    for (auto& entry : cells) entry.second.dirty = false;
    rebuildWorkspaceDiagnostics();
    graphDirty = false;
}

std::shared_ptr<const CellAnalysis> Workspace::analysis(const std::string& id)
{
    refresh();
    const auto it = cells.find(id);
    return it == cells.end() ? nullptr : it->second.analysis;
}

const math_deps::Graph& Workspace::graph()
{
    refresh();
    return graphState;
}

std::vector<Diagnostic> Workspace::diagnostics(const std::string& id)
{
    refresh();
    std::vector<Diagnostic> out;
    const auto it = cells.find(id);
    if (it == cells.end() || it->second.analysis == nullptr) return out;
    out = it->second.analysis->diagnostics;
    const auto more = extra.find(id);
    if (more != extra.end()) out.insert(out.end(), more->second.begin(), more->second.end());
    return out;
}

bool Workspace::complete(const std::string& id)
{
    const auto a = analysis(id);
    if (a == nullptr || !a->complete) return false;
    for (const auto& d : diagnostics(id)) if (d.severity == Diagnostic::Severity::Error) return false;
    return true;
}

// ---- results --------------------------------------------------------------------------------------------------------------------------------
Ticket Workspace::ticket(const std::string& id)
{
    refresh();
    Ticket t;
    const auto it = cells.find(id);
    if (it == cells.end()) return t;
    t.cell = id;
    t.revision = it->second.snapshot.cellRevision;
    t.stamp = graphState.stamp(id);
    t.valid = it->second.analysis != nullptr && !graphState.blocked(id);
    return t;
}

bool Workspace::submitResult(const Ticket& submitted, math_value::Value value)
{
    const auto current = ticket(submitted.cell);
    if (!current.valid || current.revision != submitted.revision || current.stamp != submitted.stamp) return false;
    auto stored = std::make_shared<StoredResult>();
    value.provenance.cell = submitted.cell;
    value.provenance.cellRevision = submitted.revision;
    stored->value = std::move(value);
    stored->ticket = submitted;
    cells[submitted.cell].stored = stored;
    return true;
}

ResultStatus Workspace::resultStatus(const std::string& id)
{
    refresh();
    const auto it = cells.find(id);
    if (it == cells.end() || it->second.stored == nullptr) return graphState.blocked(id) ? ResultStatus::Blocked : ResultStatus::Missing;
    if (graphState.blocked(id)) return ResultStatus::Blocked;
    const auto now = ticket(id);
    return it->second.stored->ticket.revision == now.revision && it->second.stored->ticket.stamp == now.stamp ? ResultStatus::Current : ResultStatus::Stale;
}

const StoredResult* Workspace::result(const std::string& id)
{
    return resultStatus(id) == ResultStatus::Current ? cells[id].stored.get() : nullptr;
}

const char* outcomeStateName(OutcomeState state)
{
    switch (state)
    {
        case OutcomeState::Current: return "current";
        case OutcomeState::Stale: return "stale";
        case OutcomeState::Blocked: return "blocked";
        default: return "none";
    }
}

const Outcome* Workspace::outcome(const std::string& id) const
{
    const auto it = outcomes.find(id);
    return it == outcomes.end() ? nullptr : &it->second;
}

void Workspace::clearOutcome(const std::string& id) { outcomes.erase(id); }

bool Workspace::evaluable(const std::string& id)
{
    const auto a = analysis(id);
    if (a == nullptr || a->role == math_doc::Role::Assumption || a->ir == nullptr || a->ir->op == math_ir::Op::Hole) return false;   // nothing entered yet
    for (const auto& d : a->definitions)
        if (d.isFunction) return false;          // a function definition has nothing to compute until it is called
    return true;
}

OutcomeState Workspace::outcomeState(const std::string& id)
{
    refresh();
    const auto it = outcomes.find(id);
    if (cells.find(id) == cells.end()) return OutcomeState::None;
    if (graphState.blocked(id)) return OutcomeState::Blocked;
    if (it == outcomes.end()) return OutcomeState::None;
    const auto now = ticket(id);
    return it->second.ticket.revision == now.revision && it->second.ticket.stamp == now.stamp ? OutcomeState::Current : OutcomeState::Stale;
}

const Outcome* Workspace::currentOutcome(const std::string& id, math_solver::Operation op)
{
    const auto state = outcomeState(id);
    if (state != OutcomeState::Current) return nullptr;
    const auto* o = outcome(id);
    if (o == nullptr || o->op != op) return nullptr;
    // an interrupted run says nothing about the mathematics; try again
    if (o->response.status == math_solver::Status::Cancelled || o->response.status == math_solver::Status::TimedOut) return nullptr;
    return o;
}

Evaluation Workspace::compute(const std::string& id, const math_solver::Registry& registry, const EvaluateOptions& options,
                              const std::map<std::string, math_value::Value>& bindings,
                              const std::map<std::string, math_solver::FunctionDefinition>& functions,
                              const std::vector<std::string>& upstreamFailed)
{
    Evaluation evaluation;
    evaluation.ticket = ticket(id);
    const auto self = analysis(id);
    math_solver::Request request;
    request.op = options.op;
    request.cancel = options.cancel;
    if (options.timeoutMs > 0)
    {
        request.hasDeadline = true;
        request.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    }
    request.cell = id;
    request.cellRevision = cells[id].snapshot.cellRevision;
    request.bindings = bindings;
    request.functions = functions;
    request.expr = self->ir;
    if (self->role == math_doc::Role::Definition && self->ir != nullptr && self->ir->op == math_ir::Op::Define && self->ir->args.size() == 2)
        request.expr = self->ir->args[1];
    ++statistics.engineRuns;
    ++statistics.evaluations;
    evaluation.response = registry.run(request);
    const bool good = evaluation.response.status == math_solver::Status::Ok && evaluation.response.value.ok();
    if (!good && !upstreamFailed.empty())
    {
        std::string names;
        for (size_t i = 0; i < upstreamFailed.size(); ++i) names += (i ? ", " : "") + upstreamFailed[i];
        evaluation.response.diagnostics.push_back({ Diagnostic::Severity::Info, "upstream-failed", "It depends on " + names + ", which could not be computed.", {} });
    }
    if (evaluation.response.status == math_solver::Status::Ok)
        evaluation.stored = submitResult(evaluation.ticket, evaluation.response.value);
    Outcome o;
    o.response = evaluation.response;
    o.ticket = evaluation.ticket;
    o.op = options.op;
    o.sequence = ++sequence;
    outcomes[id] = std::move(o);
    return evaluation;
}

Evaluation Workspace::evaluate(const std::string& id, const math_solver::Registry& registry, math_solver::Operation op,
                               std::shared_ptr<math_solver::CancelToken> cancel)
{
    EvaluateOptions options;
    options.op = op;
    options.cancel = std::move(cancel);
    return evaluate(id, registry, options);
}

Evaluation Workspace::evaluate(const std::string& id, const math_solver::Registry& registry, const EvaluateOptions& options)
{
    refresh();
    Evaluation evaluation;
    evaluation.ticket = ticket(id);
    auto fail = [&](const char* code, const std::string& message) {
        evaluation.response.status = math_solver::Status::Failed;
        evaluation.response.value = math_value::Value::failed(code, message);
        evaluation.response.diagnostics.push_back({ Diagnostic::Severity::Error, code, message, {} });
        Outcome o;
        o.response = evaluation.response; o.ticket = evaluation.ticket; o.op = options.op; o.sequence = ++sequence;
        if (cells.count(id) > 0) outcomes[id] = std::move(o);
        return evaluation;
    };
    const auto self = analysis(id);
    if (self == nullptr) return fail("unknown-cell", "The cell is not a math cell of this worksheet.");
    if (!evaluation.ticket.valid) return fail("blocked", "The cell depends on a circular definition and cannot be computed.");
    if (self->hasPlaceholder) return fail("incomplete", "The cell has an empty position (placeholder) and cannot be computed.");
    if (self->ir == nullptr) return fail("no-expression", "The cell has no expression to compute.");
    for (const auto& d : diagnostics(id))
        if (d.severity == Diagnostic::Severity::Error && d.code != "blocked")
        {
            evaluation = fail(d.code.c_str(), d.message);
            evaluation.response.diagnostics.back().where = d.where;
            outcomes[id].response.diagnostics.back().where = d.where;
            return evaluation;
        }
    if (!options.force)
        if (const auto* cached = currentOutcome(id, options.op))
        {
            evaluation.response = cached->response;
            evaluation.reused = true;
            evaluation.stored = resultStatus(id) == ResultStatus::Current;
            ++statistics.reused;
            return evaluation;
        }

    // the definitions this cell needs, dependencies first
    const std::set<std::string> needed = graphState.transitiveDependencies(id);
    std::map<std::string, math_value::Value> bindings;
    std::map<std::string, math_solver::FunctionDefinition> functions;
    std::vector<std::string> upstreamFailed;
    for (const auto& dependency : graphState.order())
    {
        if (needed.count(dependency) == 0 || dependency == id) continue;
        const auto a = cells[dependency].analysis;
        if (a == nullptr || a->role != math_doc::Role::Definition) continue;
        for (const auto& def : a->definitions)
        {
            if (def.body == nullptr) continue;
            if (def.isFunction)
            {
                functions[def.name] = { def.params, def.body };
                continue;
            }
            bool broken = false;
            for (const auto& d : diagnostics(dependency))
                if (d.severity == Diagnostic::Severity::Error && d.code != "blocked") broken = true;
            if (broken) { upstreamFailed.push_back(def.name); continue; }
            math_value::Value value;
            EvaluateOptions dependencyOptions = options;
            dependencyOptions.op = math_solver::Operation::Evaluate;
            if (const auto* cached = currentOutcome(dependency, math_solver::Operation::Evaluate))
            {
                ++statistics.reused;
                value = cached->response.value;
            }
            else
            {
                const auto dep = compute(dependency, registry, dependencyOptions, bindings, functions, {});
                if (dep.response.status == math_solver::Status::Ok) evaluation.computed.push_back(dependency);
                value = dep.response.value;
            }
            if (value.ok() && (value.isScalar() || value.kind == math_value::Kind::Vector || value.kind == math_value::Kind::Matrix))
                bindings[def.name] = value;
            else
                upstreamFailed.push_back(def.name);
        }
    }
    const auto computedFirst = evaluation.computed;
    evaluation = compute(id, registry, options, bindings, functions, upstreamFailed);
    evaluation.computed = computedFirst;
    return evaluation;
}

BatchResult Workspace::evaluateCells(const std::vector<std::string>& ids, const math_solver::Registry& registry, const EvaluateOptions& options)
{
    refresh();
    BatchResult batch;
    const std::set<std::string> wanted(ids.begin(), ids.end());
    for (const auto& id : graphState.order())
    {
        if (wanted.count(id) == 0) continue;
        if (!evaluable(id)) { batch.skipped.push_back(id); clearOutcome(id); continue; }
        const auto result = evaluate(id, registry, options);
        (result.reused ? batch.reused : batch.evaluated).push_back(id);
        if (result.response.status != math_solver::Status::Ok || !result.response.value.ok()) batch.failed.push_back(id);
    }
    return batch;
}

BatchResult Workspace::evaluateAll(const math_solver::Registry& registry, const EvaluateOptions& options)
{
    refresh();
    return evaluateCells(cellIds(), registry, options);
}
}
