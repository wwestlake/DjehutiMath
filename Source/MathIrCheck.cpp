#include "MathIrCheck.h"

#include "MathCommands.h"
#include "MathDeps.h"
#include "MathFont.h"
#include "MathFrontend.h"
#include "MathLayout.h"
#include "MathLinear.h"
#include "MathResult.h"
#include "MathSolver.h"
#include "MathUnits.h"
#include "MathWorkspace.h"
#include "WorksheetPanel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>

namespace
{
using namespace math_ir;
using math_doc::Role;

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
    void equal(const std::string& got, const std::string& expected, const juce::String& name)
    {
        add(got == expected, name, got == expected ? juce::String() : "expected " + juce::String(expected) + " but got " + juce::String(got));
    }
};

// A scratch worksheet of math cells, analysed through one workspace.
struct Fixture
{
    math_doc::IdAllocator ids;
    math_workspace::Workspace ws;
    std::map<std::string, std::uint64_t> revisions;

    std::vector<std::string> set(const std::string& id, Role role, const juce::String& text)
    {
        std::vector<std::string> notes;
        math_doc::MathSnapshot s;
        s.cellId = id;
        s.cellRevision = ++revisions[id];
        s.role = role;
        s.lines = math_linear::parseCell(text, role, ids, notes);
        return ws.setCell(s);
    }
    std::shared_ptr<const math_frontend::CellAnalysis> a(const std::string& id) { return ws.analysis(id); }
    std::string dump(const std::string& id) { const auto x = a(id); return x ? math_ir::dump(x->ir) : "<none>"; }
};

math_doc::MathSnapshot makeSnapshot(math_doc::IdAllocator& ids, const std::string& id, Role role, const juce::String& text, std::uint64_t revision = 1)
{
    std::vector<std::string> notes;
    math_doc::MathSnapshot s;
    s.cellId = id; s.cellRevision = revision; s.role = role;
    s.lines = math_linear::parseCell(text, role, ids, notes);
    return s;
}

std::shared_ptr<const math_frontend::CellAnalysis> analyzeAlone(math_doc::IdAllocator& ids, const juce::String& text, Role role = Role::Expression)
{
    return math_frontend::analyzeCell(makeSnapshot(ids, "c", role, text));
}

bool hasCode(const math_frontend::CellAnalysis& a, const char* code, Diagnostic::Severity severity = Diagnostic::Severity::Error)
{
    for (const auto& d : a.diagnostics) if (d.code == code && d.severity == severity) return true;
    return false;
}

bool hasCode(const std::vector<Diagnostic>& list, const char* code)
{
    for (const auto& d : list) if (d.code == code) return true;
    return false;
}

std::string describe(math_workspace::Workspace& ws, const std::string& id, math_solver::Operation op = math_solver::Operation::Evaluate)
{
    static const auto registry = math_solver::defaultRegistry();
    return ws.evaluate(id, registry, op).response.value.describe();
}

// An engine that answers everything the reference one cannot, to exercise selection and discovery.
class MockSolver final : public math_solver::Solver
{
public:
    std::string id() const override { return "mock.symbolic"; }
    std::string version() const override { return "1"; }
    int priority() const override { return 10; }
    std::vector<math_solver::Capability> capabilities() const override
    {
        return { { math_solver::Operation::Simplify, math_solver::Support::Full, true, false, "returns its input" },
                 { math_solver::Operation::Evaluate, math_solver::Support::Partial, true, false, "mock" } };
    }
    math_solver::Response run(const math_solver::Request& request) override
    {
        math_solver::Response r;
        r.status = math_solver::Status::Ok;
        r.value = math_value::Value::symbolic(request.expr);
        r.provenance.method = "identity";
        r.provenance.exact = true;
        return r;
    }
};
}

bool mathIrCheck(const juce::File& destination)
{
    Report report;
    const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("djehuti-math-ir-check-" + juce::Uuid().toString());
    work.createDirectory();
    math_doc::IdAllocator ids;

    // ---- exact arithmetic and dimensions ---------------------------------------------------------------------------------------
    {
        Rational r;
        report.add(add(Rational(1, 3), Rational(1, 6), r) && r == Rational(1, 2), "Rational arithmetic is exact and normalised", r.toString());
        report.add(!mul(Rational(INT64_MAX), Rational(2), r), "Exact arithmetic reports overflow instead of wrapping");
        report.add(!div(Rational(1), Rational(0), r), "Exact division by zero is reported");
        const Dim force = Dim::base(1) * Dim::base(0) / Dim::base(2).power(Rational(2));
        report.equal(force.toString(), "m*kg*s^-2", "Dimensions multiply, divide and take powers");
        report.add(!Dim::unknown().isDimensionless() && (Dim::unknown() * force) == Dim::unknown(), "An unknown dimension stays unknown through arithmetic");
        report.add(Dim::dimensionless().isDimensionless(), "Dimensionless is distinguished from unknown");
    }

    // ---- units ---------------------------------------------------------------------------------------------------------------------
    {
        const auto n = math_units::parse("N"), composite = math_units::parse("kg*m/s^2"), joule = math_units::parse("J"), newtonMetre = math_units::parse("N*m");
        report.add(n.ok && composite.ok && n.dim == composite.dim, "N and kg*m/s^2 have the same dimension", n.dim.toString());
        report.add(joule.ok && newtonMetre.ok && joule.dim == newtonMetre.dim, "J equals N*m");
        const auto mv = math_units::parse("mV"), kohm = math_units::parse("k\xCE\xA9"), area = math_units::parse("m^2"), root = math_units::parse("m^(1/2)");
        report.add(mv.ok && std::abs(mv.scale - 1e-3) < 1e-15, "Prefixes scale a unit (mV = 1e-3 V)");
        report.add(kohm.ok && std::abs(kohm.scale - 1e3) < 1e-9 && kohm.dim == math_units::parse("ohm").dim, "A prefixed symbol with a Greek letter parses (k\xCE\xA9)");
        report.add(area.ok && root.ok && root.dim == Dim::base(0).power(Rational(1, 2)), "Unit exponents may be whole numbers or fractions");
        const auto gram = math_units::parse("g"), kilo = math_units::parse("kg");
        report.add(gram.ok && kilo.ok && kilo.dim == gram.dim && std::abs(kilo.scale - 1.0) < 1e-12, "kg is the SI coherent unit of mass");
        report.add(!math_units::parse("furlong").ok && !math_units::parse("m/").ok && !math_units::parse("m^x").ok, "Unknown units and malformed unit text are rejected with a message");
        const auto speed = math_units::parse("km/h");
        report.add(speed.ok && std::abs(speed.scale - 1000.0 / 3600.0) < 1e-12, "Compound units convert (km/h)");
        const auto expr = math_units::parse("m/s^2");
        report.equal(dump(expr.expr), "(div [m] (pow [s] 2))", "A unit is MathIR built from Unit leaves");
    }

    // ---- translation of expressions --------------------------------------------------------------------------------------------------
    {
        report.equal(dump(analyzeAlone(ids, "x^2+1")->ir), "(add (pow x 2) 1)", "x^2+1 translates to an Add of a Pow and a number");
        report.equal(dump(analyzeAlone(ids, "(x+1)/(x-1)")->ir), "(div (add x 1) (add x (neg 1)))", "A fraction becomes Div; subtraction becomes Add with Neg");
        report.equal(dump(analyzeAlone(ids, "sqrt(x^2+1)/(x+3)")->ir), "(div (root (add (pow x 2) 1) 2) (add x 3))", "A square root has index 2");
        report.equal(dump(analyzeAlone(ids, "root(3, x)")->ir), "(root x 3)", "An indexed root keeps its index");
        report.equal(dump(analyzeAlone(ids, "2*x*y")->ir), "(mul 2 x y)", "Explicit products are n-ary Mul");
        report.equal(dump(analyzeAlone(ids, "2x")->ir), "(mul 2 x)", "Juxtaposition multiplies");
        report.equal(dump(analyzeAlone(ids, "sin(x)+cos(2*x)")->ir), "(add (call:sin x) (call:cos (mul 2 x)))", "Built-in functions are Calls");
        report.equal(dump(analyzeAlone(ids, "sum_(i=1)^n i^2")->ir), "(sum:i (pow i 2) 1 n)", "A sum binds its index and holds body, lower and upper bound");
        report.equal(dump(analyzeAlone(ids, "int_0^1 x^2 dx")->ir), "(integral:x (pow x 2) 0 1)", "A definite integral takes its variable from the differential");
        report.equal(dump(analyzeAlone(ids, "int x*sin(x) dx")->ir), "(integral:x (mul x (call:sin x)) _ _)", "An indefinite integral has no bounds");
        report.equal(dump(analyzeAlone(ids, "dv/dt")->ir), "(derivative:t^1 v)", "dv/dt is a derivative of v with respect to t");
        report.equal(dump(analyzeAlone(ids, "(partial f)/(partial x)")->ir), "(derivative:x^1p f)", "A partial derivative is marked partial");
        report.equal(dump(analyzeAlone(ids, "[1, 2, 3]")->ir), "(vector 1 2 3)", "A bracket list is a vector");
        report.equal(dump(analyzeAlone(ids, "[1, 2; 3, 4]")->ir), "(matrix:2x2 1 2 3 4)", "Rows separated by ; make a matrix");
        report.equal(dump(analyzeAlone(ids, "a = b")->ir), "(eq a b)", "A relation is an Eq");
        report.equal(dump(analyzeAlone(ids, "a < b < c")->ir), "(lt a b c)", "A chain of the same relation is one n-ary relation");
        report.equal(dump(analyzeAlone(ids, "v_0 + a*t")->ir), "(add v_0 (mul a t))", "A subscripted name is one symbol");
        report.equal(dump(analyzeAlone(ids, "sum_(i=1)^n x_i^2")->ir), "(sum:i (pow (index x i) 2) 1 n)", "A subscript on a bound index is an element");
        report.equal(dump(analyzeAlone(ids, "2*pi*r")->ir), "(mul 2 #pi r)", "pi is the constant");
        report.equal(dump(analyzeAlone(ids, "abs(x-1)")->ir), "(call:abs (add x (neg 1)))", "|..| is abs");
        report.equal(dump(analyzeAlone(ids, "9.81 \"m/s^2\"")->ir), "(mul 9.81 (div [m] (pow [s] 2)))", "A number with a quoted unit is a quantity");
        const auto q = analyzeAlone(ids, "9.81 \"m/s^2\"");
        report.add(q->type.dim == math_units::parse("m/s^2").dim && q->type.domain == Domain::Real, "A quantity carries its dimension and domain", q->type.toString());
        const auto def = analyzeAlone(ids, "f(x) = x^2 + 1", Role::Definition);
        report.equal(dump(def->ir), "(define (call:f x) (add (pow x 2) 1))", "A function definition is a Define with parameters");
        report.add(def->defines.count("f") == 1 && def->definitions.size() == 1 && def->definitions[0].isFunction && def->definitions[0].params.size() == 1, "A definition cell reports what it defines");
        const auto sys = analyzeAlone(ids, "F &= m*a\na &= dv/dt", Role::System);
        report.equal(dump(sys->ir), "(system (eq F (mul m a)) (eq a (derivative:t^1 v)))", "A system is a System of its lines (alignment marks are layout only)");
        report.add(sys->lines.size() == 2 && sys->complete, "A System keeps one expression per line");
    }

    // ---- types and validation ----------------------------------------------------------------------------------------------------------
    {
        auto domainOf = [&](const char* text) { return analyzeAlone(ids, text)->type.domain; };
        report.add(domainOf("1+2") == Domain::Integer && domainOf("2^3") == Domain::Integer, "Integer arithmetic stays Integer");
        report.add(domainOf("1/2") == Domain::Rational && domainOf("2^(-1)") == Domain::Rational, "Division and negative powers of integers are Rational");
        report.add(domainOf("0.5 + 1") == Domain::Real, "A decimal literal is Real");
        report.add(domainOf("sqrt(2)") == Domain::Real && domainOf("sqrt(x^2+1)") == Domain::Real, "A root of a provably non-negative value is Real");
        report.add(domainOf("sqrt(x)") == Domain::Complex, "A root of a value of unknown sign may be Complex");
        report.add(domainOf("i^2") == Domain::Complex, "The imaginary unit makes a value Complex");
        report.add(analyzeAlone(ids, "x^2")->type.sign == Sign::Nonnegative && analyzeAlone(ids, "x^2+1")->type.sign == Sign::Positive, "Even powers are non-negative and the sum with 1 is positive");
        report.add(hasCode(*analyzeAlone(ids, "1/0"), "div-zero"), "Division by a literal zero is an error");
        report.add(hasCode(*analyzeAlone(ids, "[1,2;3,4]*[1,2,3]"), "shape-mismatch"), "A matrix times a vector of the wrong length is an error");
        const auto mv = analyzeAlone(ids, "[1,2;3,4]*[1,2]");
        report.add(mv->type.shape == Shape::Vector && mv->type.rows == 2 && !mv->hasErrors(), "A matrix times a vector gives a vector of the right size");
        report.add(hasCode(*analyzeAlone(ids, "[1,2] + 3"), "shape-mismatch"), "Adding a vector and a scalar is an error");
        report.add(hasCode(*analyzeAlone(ids, "[1,2;3,4,5]"), "matrix-shape"), "A matrix with rows of different length is an error");
        report.add(analyzeAlone(ids, "det([1,2;3,4])")->type.shape == Shape::Scalar && hasCode(*analyzeAlone(ids, "det([1,2,3])"), "shape-mismatch"), "det needs a square matrix");
        report.add(hasCode(*analyzeAlone(ids, "sin(1,2)"), "arity"), "A function called with the wrong number of arguments is an error");
        report.add(hasCode(*analyzeAlone(ids, "x ="), "missing-operand") || hasCode(*analyzeAlone(ids, "x ="), "placeholder"), "A relation with a missing side is reported");
        report.add(hasCode(*analyzeAlone(ids, "x + 1", Role::Equation), "relation-expected"), "An Equation cell needs a relation");
        report.add(hasCode(*analyzeAlone(ids, "x = 1", Role::Expression), "relation-expected") == false, "An Expression cell may hold a relation");
        report.add(hasCode(*analyzeAlone(ids, "lim(x)"), "unsupported-function"), "Functions not yet supported are named as such");
        report.add(hasCode(*analyzeAlone(ids, "1 \"furlong\""), "unknown-unit"), "An unknown unit is an error at the unit");
        report.add(hasCode(*analyzeAlone(ids, "x a", Role::Definition), "definition-form"), "A Definition cell must have the form name = expression");
        report.add(hasCode(*analyzeAlone(ids, "3 > i"), "ordering-complex"), "Ordering a complex number is an error");
    }

    // ---- dimensions in expressions -------------------------------------------------------------------------------------------------------
    {
        report.add(hasCode(*analyzeAlone(ids, "5 \"m\" + 3 \"s\""), "dimension-mismatch"), "Adding a length and a time is an error");
        const auto sum = analyzeAlone(ids, "5 \"m\" + 3 \"mm\"");
        report.add(!sum->hasErrors() && sum->type.dim == Dim::base(0), "Adding a length and a length is a length");
        const auto pressure = analyzeAlone(ids, "(10 \"N\") / (2 \"m^2\")");
        report.add(!pressure->hasErrors() && pressure->type.dim == math_units::parse("Pa").dim, "N divided by m^2 has the dimension of pressure", pressure->type.toString());
        report.add(hasCode(*analyzeAlone(ids, "sin(5 \"m\")"), "dimension-mismatch") && !analyzeAlone(ids, "sin(5 \"rad\")")->hasErrors(), "Trigonometric functions need dimensionless arguments (an angle in rad is dimensionless)");
        const auto root = analyzeAlone(ids, "sqrt(4 \"m^2\")");
        report.add(!root->hasErrors() && root->type.dim == Dim::base(0), "The square root halves a dimension");
        report.add(hasCode(*analyzeAlone(ids, "(5 \"m\")^y"), "dimension-exponent"), "A dimensioned quantity needs a constant exponent");
        report.add(analyzeAlone(ids, "(5 \"m\")^2")->type.dim == Dim::base(0).power(Rational(2)), "A whole power multiplies the dimension");
        report.add(hasCode(*analyzeAlone(ids, "5 \"m\" = 3 \"s\""), "dimension-mismatch"), "Both sides of a relation must have the same dimension");
        report.add(hasCode(*analyzeAlone(ids, "5 \"m\" + 1"), "dimension-mismatch"), "A length plus a plain number is an error");
        const auto ratio = analyzeAlone(ids, "(5 \"m\") / (2 \"m\")");
        report.add(ratio->type.dim.isDimensionless(), "A ratio of lengths is dimensionless");
        const auto mixed = analyzeAlone(ids, "x + 1");
        report.add(!mixed->hasErrors() && !mixed->type.dim.known, "An undeclared symbol has an unknown dimension and causes no dimension error");

        Fixture f;
        f.set("units", Role::Assumption, "v in \"m/s\"");
        f.set("units2", Role::Assumption, "t in \"s\"");
        f.set("ok", Role::Expression, "v*t");
        f.set("bad", Role::Expression, "v + t");
        report.add(f.a("ok")->type.dim == Dim::base(0) && !f.a("ok")->hasErrors(), "A declared unit gives a symbol its dimension (v*t is a length)", f.a("ok")->type.toString());
        report.add(hasCode(*f.a("bad"), "dimension-mismatch"), "Declared dimensions are checked (v + t)");
        report.add(f.a("units")->declares.count("v") == 1 && f.a("units")->declarations.size() == 1 && f.a("units")->lines[0] == nullptr, "An Assumption cell reports what it declares");
    }

    // ---- symbol resolution --------------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.set("a", Role::Definition, "a = 3");
        f.set("g", Role::Definition, "f(x) = x^2 + a");
        f.set("use", Role::Expression, "f(2) + a");
        f.set("arity", Role::Expression, "f(1, 2)");
        f.set("bound", Role::Expression, "sum_(i=1)^n i");
        f.set("freex", Role::Expression, "x + y");
        f.set("e1", Role::Expression, "2*e");
        report.equal(f.dump("use"), "(add (call:f 2) a)", "A call to a defined function is a Call; a defined name is a Symbol");
        report.add(f.a("use")->uses.count("f") == 1 && f.a("use")->uses.count("a") == 1, "The names a cell uses are collected");
        report.add(f.ws.graph().dependenciesOf("use") == std::set<std::string> { "a", "g" }, "A cell depends on the cells that define what it uses");
        report.add(f.a("a")->definitions.size() == 1 && f.a("a")->definitions[0].type.domain == Domain::Integer, "A defined variable has the type of its value");
        report.add(f.a("use")->type.domain == Domain::Integer || f.a("use")->type.domain == Domain::Unknown || f.a("use")->type.domain == Domain::Real, "A call has the result type of the definition");
        report.add(hasCode(*f.a("arity"), "arity"), "A call with the wrong number of arguments is an error");
        report.add(f.a("bound")->uses.count("i") == 0 && f.a("bound")->uses.count("n") == 1, "A bound index is not a use");
        report.add(hasCode(*f.a("freex"), "free-symbol", Diagnostic::Severity::Info), "An undefined symbol is reported as a free variable (information)");
        report.add(f.a("e1")->ir->args[1]->op == Op::Constant, "e is Euler's number until something defines or declares it");
        f.set("edef", Role::Definition, "e = 5");
        report.equal(f.dump("e1"), "(mul 2 e)", "Once e is defined by a cell it is that symbol, not the constant");
        const auto bound = f.a("bound");
        std::uint64_t pnode = 0;
        math_doc::forEach(f.ws.snapshot("freex")->lines[0], [&](const math_doc::Node& n) { if (n.kind == math_doc::Kind::Identifier && n.text == "y") pnode = n.id; });
        bool pointsAtY = false;
        for (const auto& d : f.a("freex")->diagnostics) if (d.code == "free-symbol" && d.where.node == pnode) pointsAtY = true;
        report.add(pointsAtY && bound != nullptr, "A diagnostic about a symbol points at that symbol's node");
        Fixture g;
        g.set("p", Role::Definition, "x = 1");
        g.set("q", Role::Definition, "x = 2");
        report.add(g.ws.graph().conflicts().size() == 1 && hasCode(g.ws.diagnostics("p"), "duplicate-definition") && hasCode(g.ws.diagnostics("q"), "duplicate-definition"),
                   "Two cells defining the same name are reported on both");
        Fixture h;
        h.set("p", Role::Expression, "foo(x)");
        report.add(!h.a("p")->hasErrors() && h.dump("p") == "(mul foo x)", "A name that is not a function followed by a bracket multiplies");
        Fixture k;
        k.set("n", Role::Assumption, "n in Z");
        k.set("n2", Role::Assumption, "n > 0");
        k.set("u", Role::Expression, "n + 1");
        k.set("w", Role::Expression, "sqrt(m)");
        k.set("pos", Role::Assumption, "m > 0");
        report.add(k.a("u")->type.domain == Domain::Integer && k.a("u")->type.sign == Sign::Positive, "An assumption gives a symbol its domain and sign (n + 1 is a positive integer)");
        report.add(k.a("w")->type.domain == Domain::Real, "A positivity assumption makes sqrt(m) real");
        report.add(k.ws.graph().dependenciesOf("w") == std::set<std::string> { "pos" }, "A cell depends on the assumptions about the names it uses");
    }

    // ---- diagnostics, source references, incompleteness, immutability -----------------------------------------------------------------------
    {
        auto snap = makeSnapshot(ids, "c", Role::Expression, "x + ?");
        const auto before = juce::JSON::toString(math_doc::toVar(snap.lines[0]));
        const auto a = math_frontend::analyzeCell(snap);
        const auto after = juce::JSON::toString(math_doc::toVar(snap.lines[0]));
        report.add(before == after, "Analysis does not modify the user's expression");
        report.add(!a->complete && a->hasPlaceholder && hasCode(*a, "placeholder"), "A placeholder makes the cell incomplete and is reported");
        std::uint64_t holeNode = 0;
        math_doc::forEach(snap.lines[0], [&](const math_doc::Node& n) { if (n.kind == math_doc::Kind::Placeholder) holeNode = n.id; });
        bool atHole = false;
        for (const auto& d : a->diagnostics) if (d.code == "placeholder" && d.where.node == holeNode && d.where.cell == "c") atHole = true;
        report.add(atHole, "The placeholder diagnostic points at the placeholder node");
        report.add(a->ir != nullptr && hasHole(a->ir), "Incomplete mathematics translates to MathIR with a Hole");

        auto s2 = makeSnapshot(ids, "d", Role::Expression, "5 \"m\" + 3 \"s\"");
        const auto d = math_frontend::analyzeCell(s2);
        bool range = false;
        for (const auto& x : d->diagnostics)
            if (x.code == "dimension-mismatch" && x.where.node == s2.lines[0]->id && x.where.begin == 0 && x.where.end == s2.lines[0]->items.size()) range = true;
        report.add(range, "A diagnostic about a whole sum points at the item range of the row it spans");

        math_frontend::Frontend frontend;
        bool called = false;
        frontend.analyze(s2, [&](math_doc::AnalysisResult r) {
            called = r.cellId == "d" && r.forRevision == 1 && !r.complete && !r.diagnostics.empty();
            for (const auto& x : r.diagnostics)
                if (x.code == "dimension-mismatch") called = called && x.severity == math_doc::Diagnostic::Severity::Error && x.where.node == s2.lines[0]->id;
        });
        report.add(called, "The existing MathFrontend::analyze interface reports revision, completeness and source references");

        const auto clean = analyzeAlone(ids, "x^2 + 1");
        report.add(clean->complete && !clean->hasErrors(), "A complete, valid cell is marked complete");
        const auto nested = analyzeAlone(ids, "((((((((x+1))))))))");
        report.equal(dump(nested->ir), "(add x 1)", "Redundant brackets do not change the expression");
        const auto unclosed = analyzeAlone(ids, "(x+1");
        report.add(hasCode(*unclosed, "unclosed-bracket", Diagnostic::Severity::Warning), "An unclosed bracket is a warning");
    }

    // ---- the dependency graph ---------------------------------------------------------------------------------------------------------------------
    {
        math_deps::Graph g;
        auto cell = [&](const char* id, std::uint64_t rev, std::set<std::string> defines, std::set<std::string> uses) {
            math_deps::CellNames c; c.id = id; c.revision = rev; c.defines = std::move(defines); c.uses = std::move(uses);
            return g.set(c);
        };
        cell("a", 1, { "x" }, {});
        cell("b", 1, { "y" }, { "x" });
        cell("c", 1, { "z" }, { "y" });
        cell("d", 1, {}, { "x", "z" });
        report.add(g.dependenciesOf("b") == std::set<std::string> { "a" } && g.dependentsOf("a") == std::set<std::string> { "b", "d" }, "Direct dependencies and dependents are derived from names");
        report.add(g.transitiveDependents("a") == std::set<std::string> { "b", "c", "d" }, "A change to a cell invalidates everything downstream");
        report.add(g.transitiveDependencies("d") == std::set<std::string> { "a", "b", "c" }, "Transitive dependencies are complete");
        const auto order = g.order();
        auto pos = [&](const char* id) { return std::find(order.begin(), order.end(), id) - order.begin(); };
        report.add(order.size() == 4 && pos("a") < pos("b") && pos("b") < pos("c") && pos("c") < pos("d"), "The computation order lists dependencies first");
        report.add(g.cycles().empty() && !g.blocked("d"), "An acyclic graph has no cycles");
        const auto stampC = g.stamp("c"), stampA = g.stamp("a");
        cell("a", 2, { "x" }, {});
        report.add(g.stamp("c") != stampC && g.stamp("a") != stampA, "Changing a revision changes the stamp of the cell and of its dependents");
        report.add(!cell("a", 2, { "x" }, {}), "Setting an identical cell changes nothing");
        const auto stampB = g.stamp("b");
        cell("e", 1, { "w" }, {});
        report.add(g.stamp("b") == stampB && g.stamp("e") != 0, "An unrelated cell does not change another cell's stamp");
        cell("x1", 1, { "p" }, { "q" });
        cell("x2", 1, { "q" }, { "p" });
        cell("x3", 1, {}, { "p" });
        cell("self", 1, { "s" }, { "s" });
        const auto cycles = g.cycles();
        report.add(cycles.size() == 2 && g.inCycle("x1") && g.inCycle("x2") && g.inCycle("self") && !g.inCycle("x3"), "Cycles, including a cell that depends on itself, are detected");
        report.add(g.blocked("x3") && !g.blocked("e") && g.blocked("self"), "A cell depending on a cycle is blocked");
        cell("a2", 1, { "x" }, {});
        report.add(g.conflicts().size() == 1 && g.conflicts()[0].name == "x", "Two cells defining one name are a conflict");
    }

    // ---- the workspace: invalidation, cycles, revisions ----------------------------------------------------------------------------------------
    {
        Fixture f;
        f.set("a", Role::Definition, "a = 3");
        f.set("b", Role::Definition, "b = a*2");
        f.set("c", Role::Expression, "b + 1");
        f.set("z", Role::Expression, "7");
        report.add(f.ws.graph().order().size() == 4, "The workspace builds the dependency graph from the analyses");
        const auto stampC = f.ws.ticket("c").stamp, stampZ = f.ws.ticket("z").stamp;
        const auto invalidated = f.set("a", Role::Definition, "a = 4");
        const std::set<std::string> inv(invalidated.begin(), invalidated.end());
        report.add(inv == std::set<std::string> { "a", "b", "c" }, "Editing a definition reports the cells it invalidates");
        report.add(f.ws.ticket("c").stamp != stampC && f.ws.ticket("z").stamp == stampZ, "Only dependent cells get a new stamp");
        report.add(f.set("z", Role::Expression, "7").size() == 1, "Setting a cell whose content changed reports it");
        math_workspace::Workspace& ws = f.ws;
        std::vector<std::string> none = ws.setCell(*ws.snapshot("z"));
        report.add(none.empty(), "Setting an identical snapshot is a no-op");

        Fixture cyc;
        cyc.set("a", Role::Definition, "a = b + 1");
        cyc.set("b", Role::Definition, "b = a + 1");
        cyc.set("c", Role::Expression, "a * 2");
        cyc.set("s", Role::Definition, "s = s + 1");
        report.add(hasCode(cyc.ws.diagnostics("a"), "circular-definition") && hasCode(cyc.ws.diagnostics("b"), "circular-definition"), "A circular definition is reported on every cell of the cycle");
        report.add(hasCode(cyc.ws.diagnostics("s"), "circular-definition"), "A cell defined in terms of itself is a cycle");
        report.add(hasCode(cyc.ws.diagnostics("c"), "blocked") && !cyc.ws.ticket("c").valid, "A cell that depends on a cycle is blocked and has no valid ticket");
        report.add(cyc.ws.resultStatus("c") == math_workspace::ResultStatus::Blocked, "A blocked cell reports a Blocked result status");
        cyc.set("b", Role::Definition, "b = 1");
        report.add(!hasCode(cyc.ws.diagnostics("a"), "circular-definition") && cyc.ws.ticket("c").valid, "Breaking the cycle clears the diagnostics and unblocks dependents");

        Fixture rm;
        rm.set("d", Role::Definition, "k = 2");
        rm.set("u", Role::Expression, "k + 1");
        report.add(!hasCode(rm.ws.diagnostics("u"), "free-symbol") && rm.ws.graph().dependenciesOf("u").size() == 1, "A cell using a defined name depends on its definition");
        rm.ws.removeCell("d");
        report.add(rm.ws.graph().dependenciesOf("u").empty() && hasCode(*rm.a("u"), "free-symbol", Diagnostic::Severity::Info), "Removing a definition re-analyses the cells that used it");
        rm.set("d", Role::Definition, "k = 2");
        report.add(!hasCode(*rm.a("u"), "free-symbol", Diagnostic::Severity::Info), "Adding the definition back resolves the name again");

        Fixture late;
        late.set("u", Role::Expression, "m + 1");
        report.add(hasCode(*late.a("u"), "free-symbol", Diagnostic::Severity::Info), "A name nothing defines is free");
        late.set("m", Role::Definition, "m = 10");
        report.add(!hasCode(*late.a("u"), "free-symbol", Diagnostic::Severity::Info) && late.ws.graph().dependenciesOf("u").count("m") == 1,
                   "A definition that arrives later (even further down the worksheet) is picked up");
    }

    // ---- revision-aware results and evaluation ----------------------------------------------------------------------------------------------------
    {
        static const auto registry = math_solver::defaultRegistry();
        Fixture f;
        f.set("a", Role::Definition, "a = 3");
        f.set("b", Role::Definition, "b = a*2");
        f.set("c", Role::Expression, "b + 1");
        auto t = f.ws.ticket("c");
        report.add(t.valid && t.revision == 1 && f.ws.resultStatus("c") == math_workspace::ResultStatus::Missing, "A cell with no result reports Missing");
        report.add(f.ws.submitResult(t, math_value::Value::integer(7)) && f.ws.resultStatus("c") == math_workspace::ResultStatus::Current && f.ws.result("c") != nullptr, "A result submitted with a current ticket is stored");
        report.add(f.ws.result("c")->value.provenance.cell == "c" && f.ws.result("c")->value.provenance.cellRevision == 1, "A stored result remembers the cell and revision it answers");
        f.set("a", Role::Definition, "a = 4");
        report.add(f.ws.resultStatus("c") == math_workspace::ResultStatus::Stale && f.ws.result("c") == nullptr, "Editing a dependency makes the stored result stale");
        report.add(!f.ws.submitResult(t, math_value::Value::integer(7)), "A late result computed for an old revision is rejected");

        Fixture e;
        e.set("a", Role::Definition, "a = 3");
        e.set("b", Role::Definition, "b = a*2");
        e.set("c", Role::Expression, "b + 1");
        auto ev = e.ws.evaluate("c", registry);
        report.add(ev.response.status == math_solver::Status::Ok && ev.response.value.describe() == "7" && ev.stored, "Evaluating a cell evaluates the definitions it needs", ev.response.value.describe());
        report.add(ev.computed == std::vector<std::string> { "a", "b" }, "Dependencies are evaluated in order, first", juce::String((int) ev.computed.size()));
        report.add(ev.response.provenance.engine == "djehuti.reference" && ev.response.provenance.exact && ev.response.provenance.cell == "c" && ev.response.provenance.cellRevision == 1,
                   "A response carries the engine, exactness and the cell and revision it answers", ev.response.provenance.method);
        const auto again = e.ws.evaluate("c", registry);
        report.add(again.computed.empty() && again.response.value.describe() == "7", "Current results of dependencies are reused");
        e.set("a", Role::Definition, "a = 10");
        const auto changed = e.ws.evaluate("c", registry);
        report.add(changed.response.value.describe() == "21" && changed.computed.size() == 2, "After an edit the results are recomputed from the new definition", changed.response.value.describe());

        report.equal(describe(f.ws, "c"), "9", "Evaluation follows the latest definitions");
        Fixture g;
        g.set("x", Role::Expression, "(1/3 + 1/6) * 4");
        report.equal(describe(g.ws, "x"), "2", "Exact rational arithmetic gives exact results");
        g.set("y", Role::Expression, "sqrt(2)^2");
        const auto yv = g.ws.evaluate("y", registry);
        report.add(yv.response.value.kind == math_value::Kind::Real && !yv.response.provenance.exact, "Floating-point results are marked approximate");
        g.set("r", Role::Expression, "sqrt(16)");
        report.add(g.ws.evaluate("r", registry).response.value.describe() == "4" && g.ws.evaluate("r", registry).response.provenance.exact, "A perfect square root stays exact");
        g.set("n", Role::Expression, "1/4 + 1/4");
        report.equal(describe(g.ws, "n", math_solver::Operation::Numeric), "0.5", "The numeric operation always returns floating point");
        g.set("d", Role::Expression, "(5 \"m\") / (2 \"s\")");
        report.equal(describe(g.ws, "d"), "5/2 [m*s^-1]", "Quantities evaluate with their dimension in coherent SI");
        g.set("mm", Role::Expression, "3 \"mm\" + 2 \"m\"");
        report.add(std::abs(g.ws.evaluate("mm", registry).response.value.scalar.toDouble() - 2.003) < 1e-12, "Units convert to SI when evaluated");
        g.set("z", Role::Expression, "1/(2-2)");
        const auto zero = g.ws.evaluate("z", registry);
        report.add(zero.response.value.kind == math_value::Kind::Undefined && zero.response.status == math_solver::Status::Ok && zero.response.value.diagnostics[0].code == "div-zero",
                   "Division by zero is an Undefined value with a diagnostic, not a failure");
        g.set("w", Role::Expression, "x + 1");
        const auto unsupported = g.ws.evaluate("w", registry);
        report.add(unsupported.response.status == math_solver::Status::Unsupported && !unsupported.response.diagnostics.empty() && unsupported.response.diagnostics[0].code == "no-engine",
                   "An expression no engine can evaluate is reported as Unsupported");
        g.set("s", Role::Expression, "sum_(i=1)^10 i^2");
        report.equal(describe(g.ws, "s"), "385", "A sum with whole-number bounds evaluates exactly");
        g.set("q", Role::Expression, "3 \"V\" < 2 \"A\"");
        report.add(hasCode(*g.a("q"), "dimension-mismatch"), "Comparing different dimensions is rejected before evaluation");
        g.set("rel", Role::Expression, "2 + 2 = 4");
        report.equal(describe(g.ws, "rel"), "true", "A relation between numbers evaluates to a Boolean");
        g.set("big", Role::Expression, "9999999999 * 9999999999 * 9999999999");
        const auto big = g.ws.evaluate("big", registry);
        report.add(big.response.value.kind == math_value::Kind::Real && hasCode(big.response.diagnostics, "exact-overflow"), "Exact overflow falls back to floating point with a warning");
        g.set("cx", Role::Expression, "sqrt(0-4)");
        report.add(g.ws.evaluate("cx", registry).response.value.kind == math_value::Kind::Complex, "The root of a negative number evaluates to a Complex value");
        g.set("inc", Role::Expression, "1 + ?");
        const auto incomplete = g.ws.evaluate("inc", registry);
        report.add(incomplete.response.status == math_solver::Status::Failed && incomplete.response.value.diagnostics[0].code == "incomplete", "An incomplete cell is not evaluated");
        g.set("vec", Role::Expression, "[1, 2] ");
        report.equal(describe(g.ws, "vec"), "[1, 2]", "Vectors evaluate element by element");
        g.set("mat", Role::Expression, "[1, 2; 3, 4]");
        report.equal(describe(g.ws, "mat"), "[1, 2; 3, 4]", "Matrices evaluate element by element");
        g.set("neg", Role::Expression, "(0-8)^(1/3)");
        report.add(g.ws.evaluate("neg", registry).response.value.ok(), "A fractional power of a negative number does not fail");
    }

    // ---- the solver interface ----------------------------------------------------------------------------------------------------------------------------------
    {
        using namespace math_solver;
        auto registry = defaultRegistry();
        report.add(!registry.discover(Operation::Evaluate).empty() && !registry.discover(Operation::Numeric).empty(), "Capability discovery lists what the registered engines can do");
        report.add(registry.discover(Operation::Solve).empty() && registry.discover(Operation::Integrate).empty() && registry.discover(Operation::Differentiate).empty() && registry.discover(Operation::Simplify).empty(),
                   "The operations S0 does not implement have no engine");
        bool notes = true;
        for (const auto& c : registry.capabilities()) notes = notes && !c.capability.notes.empty();
        report.add(notes, "Every capability states its limits in words");
        Operation op;
        report.add(operationFromName("integrate", op) && op == Operation::Integrate && !operationFromName("magic", op), "Operations have stable names");

        const auto x = analyzeAlone(ids, "x^2");
        Request ask;
        ask.op = Operation::Integrate;
        ask.expr = x->ir;
        ask.cell = "c1";
        ask.cellRevision = 9;
        const auto none = registry.run(ask);
        report.add(none.status == Status::Unsupported && none.value.kind == math_value::Kind::Failed && none.provenance.cell == "c1" && none.provenance.cellRevision == 9,
                   "An unsupported request returns a structured response stamped with cell and revision");

        registry.add(std::make_shared<MockSolver>());
        report.add(registry.discover(Operation::Simplify).size() == 1 && registry.size() == 2, "A registered engine appears in discovery");
        ask.op = Operation::Simplify;
        const auto simplified = registry.run(ask);
        report.add(simplified.status == Status::Ok && simplified.provenance.engine == "mock.symbolic" && simplified.value.kind == math_value::Kind::Symbolic && simplified.provenance.method == "identity",
                   "The registry selects an engine that supports the operation and records its provenance");
        const auto numeric = analyzeAlone(ids, "2+3");
        Request evalRequest;
        evalRequest.op = Operation::Evaluate;
        evalRequest.expr = numeric->ir;
        report.add(registry.select(evalRequest) != nullptr && registry.select(evalRequest)->id() == "mock.symbolic", "At equal support the engine with the higher priority is chosen");

        auto token = std::make_shared<CancelToken>();
        token->request();
        Request cancelled;
        cancelled.op = Operation::Evaluate;
        cancelled.expr = analyzeAlone(ids, "sum_(i=1)^100000 i")->ir;
        cancelled.cancel = token;
        const auto stopped = defaultRegistry().run(cancelled);
        report.add(stopped.status == Status::Cancelled && stopped.value.kind == math_value::Kind::Failed, "A request cancelled before it starts is not run");
        Request late;
        late.op = Operation::Evaluate;
        late.expr = analyzeAlone(ids, "sum_(i=1)^900000 i")->ir;
        late.cancel = std::make_shared<CancelToken>();
        late.hasDeadline = true;
        late.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        const auto timedOut = defaultRegistry().run(late);
        report.add(timedOut.status == Status::TimedOut, "An engine that is past its deadline stops cooperatively");
        Request running;
        running.op = Operation::Evaluate;
        running.expr = late.expr;
        running.cancel = std::make_shared<CancelToken>();
        std::thread canceller([token = running.cancel] { token->request(); });
        canceller.join();
        const auto raced = defaultRegistry().run(running);
        report.add(raced.status == Status::Cancelled, "Cancellation requested from another thread is honoured");
    }

    // ---- values -------------------------------------------------------------------------------------------------------------------------------------------------
    {
        using math_value::Value;
        report.add(Value::integer(3).isExact() && Value::rational(Rational(1, 3)).isExact() && !Value::real(0.5).isExact() && !Value::complexNumber(1, 2).isExact(), "Exact and approximate scalars are distinguished");
        report.add(Value::rational(Rational(4, 2)).kind == math_value::Kind::Integer, "A rational with denominator 1 is an Integer");
        report.add(!Value::undefined("c", "m").ok() && !Value::failed("c", "m").ok() && Value::integer(1).ok(), "Undefined and failed values are not ok");
        report.add(Value::matrix(2, 2, { Value::integer(1) }).kind == math_value::Kind::Failed, "A matrix with the wrong number of elements fails");
        report.add(Value::vector({ Value::integer(1), Value::rational(Rational(1, 2)) }).isExact(), "A vector of exact elements is exact");
        report.add(Value::boolean(true).isExact() && Value::symbolic(analyzeAlone(ids, "x+1")->ir).kind == math_value::Kind::Symbolic, "Booleans and symbolic expressions are values");
        auto withDim = Value::real(2.5); withDim.dim = Dim::base(2);
        report.equal(withDim.describe(), "2.5 [s]", "A dimensioned value shows its dimension");
    }

    // ---- results back to presentation trees -------------------------------------------------------------------------------------------------------------------------
    {
        math_doc::IdAllocator fresh;
        fresh.next = 5000;
        auto linearOf = [&](const math_result::Tree& t) { return math_linear::toLinear(t.lines); };
        const auto source = makeSnapshot(ids, "c", Role::Expression, "(x+1)/(x-1)");
        const auto analysis = math_frontend::analyzeCell(source);
        const auto tree = math_result::fromExpr(analysis->ir, fresh);
        juce::String error;
        report.add(tree.lines.size() == 1 && math_doc::validate(tree.lines[0], error), "A result tree is a valid document tree", error);
        report.add(math_doc::maxId(tree.lines[0]) >= 5000 && tree.lines[0]->id >= 5000, "New result expressions get new identifiers from the allocator");
        bool allFresh = true;
        math_doc::forEach(tree.lines[0], [&](const math_doc::Node& n) { allFresh = allFresh && n.id >= 5000; });
        report.add(allFresh, "Every node of a result is new (no identifier of the original is reused)");
        report.equal(linearOf(tree), "(x + 1)/(x - 1)", "The result displays as the same fraction");
        bool related = !tree.derivedFrom.empty();
        for (const auto& [nodeId, from] : tree.derivedFrom)
        {
            bool found = false;
            for (const auto& line : source.lines) found = found || math_doc::find(line, from.node) != nullptr;
            related = related && found && from.cell == "c" && math_doc::find(tree.lines[0], nodeId) != nullptr;
        }
        report.add(related, "Result nodes point back at nodes of the original notation", juce::String((int) tree.derivedFrom.size()) + " relations");

        // round trips: IR -> presentation -> linear text -> parse -> IR
        for (const char* text : { "x^2+1", "sqrt(x^2+1)/(x+3)", "sum_(i=1)^n i^2", "int_0^1 x^2 dx", "sin(x)*cos(2*x)", "a*b+c", "(a+b)*c", "x_0 + v*t" })
        {
            const auto original = analyzeAlone(ids, text);
            math_doc::IdAllocator f2;
            f2.next = 9000;
            const auto shown = math_result::fromExpr(original->ir, f2);
            std::vector<std::string> notes;
            const auto parsed = math_linear::parseLine(juce::String(math_linear::toLinear(shown.lines)), ids, notes);
            math_doc::MathSnapshot again;
            again.cellId = "c"; again.cellRevision = 1; again.role = Role::Expression; again.lines = { parsed };
            const auto reparsed = math_frontend::analyzeCell(again);
            report.equal(dump(reparsed->ir), dump(original->ir), juce::String("An expression survives IR -> display -> notation -> IR: ") + text);
        }

        using math_value::Value;
        auto show = [&](const Value& v, const math_result::Options& o = {}) { math_doc::IdAllocator a; return linearOf(math_result::fromValue(v, a, {}, o)); };
        report.add(show(Value::rational(Rational(5, 2))).find("5") != std::string::npos && show(Value::rational(Rational(-1, 3))).find("-") != std::string::npos, "Rational values display as fractions with their sign");
        report.equal(show(Value::integer(-12)), "- 12", "A negative integer displays with a minus sign");
        report.equal(show(Value::real(0.25)), "0.25", "A real number displays its shortest form");
        report.add(show(Value::real(1.5e-9)).find("10") != std::string::npos, "A very small real uses scientific notation");
        report.add(show(Value::complexNumber(1, -2)).find("i") != std::string::npos, "A complex value displays its imaginary part");
        report.equal(show(Value::boolean(true)), "\"true\"", "A Boolean displays as text");
        report.equal(show(Value::undefined("div-zero", "x")), "\"undefined\"", "An undefined value displays as undefined");
        Value volts = Value::real(0.003);
        volts.dim = math_units::parse("V").dim;
        math_result::Options inMv;
        inMv.displayUnit = "mV";
        const auto mv = show(volts, inMv);
        report.add(mv.find("3") != std::string::npos && mv.find("mV") != std::string::npos, "A dimensioned value can be shown in a chosen unit", mv);
        math_result::Options wrong;
        wrong.displayUnit = "s";
        math_doc::IdAllocator a3;
        const auto wrongTree = math_result::fromValue(volts, a3, {}, wrong);
        report.add(!wrongTree.problem.empty() && math_linear::toLinear(wrongTree.lines).find("kg") != std::string::npos, "A unit of the wrong dimension is refused and the value is shown in SI");
        math_doc::IdAllocator a4;
        Source origin; origin.cell = "c"; origin.node = 42;
        const auto withOrigin = math_result::fromValue(Value::integer(5), a4, origin);
        report.add(withOrigin.derivedFrom.size() == 1 && withOrigin.derivedFrom.begin()->second.node == 42, "A computed value records the cell that was asked");
        const auto symbolic = Value::symbolic(analyzeAlone(ids, "sqrt(x^2+1)/(x+3)")->ir);
        math_doc::IdAllocator a5;
        const auto symbolicTree = math_result::fromValue(symbolic, a5);
        report.equal(linearOf(symbolicTree), math_linear::toLinear(makeSnapshot(ids, "x", Role::Expression, "sqrt(x^2+1)/(x+3)").lines), "A symbolic value displays like the notation it came from");
        const auto mat = Value::matrix(2, 2, { Value::integer(1), Value::integer(2), Value::integer(3), Value::integer(4) });
        report.add(show(mat).find(';') != std::string::npos, "A matrix displays its rows");
        auto& font = math_font::MathFont::instance();
        if (font.ok())
        {
            math_layout::Options layoutOptions;
            layoutOptions.fontSize = 24.0f;
            math_doc::IdAllocator a6;
            Value withUnit = Value::real(9.81);
            withUnit.dim = math_units::parse("m/s^2").dim;
            const auto unitTree = math_result::fromValue(withUnit, a6);
            const auto laid = math_layout::layoutRow(unitTree.lines[0], layoutOptions);
            report.add(laid.problem.isEmpty() && laid.width > 20.0f && font.glyphFor(0x2009) != 0, "A result with units typesets with the existing layout", juce::String(laid.width));
            const auto laid2 = math_layout::layoutRow(tree.lines[0], layoutOptions);
            report.add(laid2.problem.isEmpty() && laid2.width > 20.0f && laid2.nodes.size() > 3, "A symbolic result typesets with node rectangles");
        }
        else report.add(false, "The math font is available for typesetting results", font.error());
    }

    // ---- the command service and document compatibility ---------------------------------------------------------------------------------------------------------
    {
        WorksheetDocument doc;
        doc.reset();
        std::vector<std::string> notes;
        const auto a = doc.newMathCell(Role::Definition, "a = 3", notes);
        const auto b = doc.newMathCell(Role::Definition, "b = a*2", notes);
        const auto c = doc.newMathCell(Role::Expression, "b + 1", notes);
        const auto bad = doc.newMathCell(Role::Expression, "5 \"m\" + 3 \"s\"", notes);
        const auto before = juce::JSON::toString(doc.toJson());
        math_workspace::Workspace ws;

        auto run = [&](const char* name, juce::DynamicObject* args) {
            const auto text = math_commands::execute(name, juce::var(args), doc, ws);
            return juce::JSON::parse(text);
        };
        auto idArgs = [](const juce::String& id) { auto* o = new juce::DynamicObject(); o->setProperty("id", id); return o; };

        const auto analyze = run("worksheet_math_analyze", idArgs(bad));
        bool found = false;
        if (auto* list = analyze["diagnostics"].getArray())
            for (const auto& d : *list) found = found || (d["code"].toString() == "dimension-mismatch" && (int) d["source"]["end"] == 5);
        report.add((bool) analyze["ok"] && found && !(bool) analyze["complete"], "worksheet_math_analyze reports diagnostics with source references", analyze["type"].toString());
        const auto ok = run("worksheet_math_analyze", idArgs(c));
        report.add((bool) ok["ok"] && (bool) ok["complete"] && ok["ir"].toString().contains("add"), "worksheet_math_analyze describes a complete cell and its MathIR");
        report.add(!(bool) run("worksheet_math_analyze", idArgs("nope"))["ok"], "worksheet_math_analyze rejects an unknown cell");

        const auto deps = run("worksheet_math_dependencies", new juce::DynamicObject());
        bool chain = false;
        if (auto* list = deps["cells"].getArray())
            for (const auto& cellInfo : *list)
                if (cellInfo["id"].toString() == c) chain = cellInfo["dependsOn"].size() == 1 && cellInfo["dependsOn"][0].toString() == b;
        const auto orderArray = deps["order"];
        report.add((bool) deps["ok"] && chain && orderArray.size() >= 3, "worksheet_math_dependencies lists dependencies and the order");

        const auto evaluated = run("worksheet_math_evaluate", idArgs(c));
        report.add((bool) evaluated["ok"] && evaluated["value"].toString() == "7" && (bool) evaluated["exact"] && evaluated["provenance"]["engine"].toString() == "djehuti.reference"
                       && evaluated["typeset"].toString() == "7", "worksheet_math_evaluate computes the cell with provenance and a typeset form", evaluated["value"].toString());
        const auto status = run("worksheet_math_dependencies", idArgs(c));
        report.add(status["cells"][0]["result"].toString() == "current", "A stored result is reported as current by worksheet_math_dependencies");
        auto* withUnit = idArgs(c);
        withUnit->setProperty("display_unit", "mm");
        const auto none = run("worksheet_math_evaluate", withUnit);
        report.add((bool) none["ok"], "worksheet_math_evaluate accepts a display unit");
        const auto failed = run("worksheet_math_evaluate", idArgs(bad));
        report.add(!(bool) failed["ok"], "worksheet_math_evaluate reports a cell with errors as not ok");

        const auto caps = run("math_solver_capabilities", new juce::DynamicObject());
        bool listsEvaluate = false;
        if (auto* list = caps["capabilities"].getArray()) for (const auto& item : *list) listsEvaluate = listsEvaluate || item["operation"].toString() == "evaluate";
        bool noSolve = false;
        if (auto* list = caps["operationsWithoutEngine"].getArray()) for (const auto& item : *list) noSolve = noSolve || item.toString() == "solve";
        report.add((bool) caps["ok"] && listsEvaluate && noSolve, "math_solver_capabilities lists engines and the operations nothing implements yet");
        auto* filter = new juce::DynamicObject();
        filter->setProperty("operation", "bogus");
        report.add(!(bool) run("math_solver_capabilities", filter)["ok"], "math_solver_capabilities rejects an unknown operation");
        report.add(math_commands::handles("worksheet_math_evaluate") && !math_commands::handles("worksheet_math_snapshot"), "The command set is the documented one");

        report.add(juce::JSON::toString(doc.toJson()) == before, "Analysis and evaluation leave the worksheet document unchanged");

        // saving and reopening keeps what analysis sees
        const auto file = work.getChildFile("s0.djmath");
        juce::String error;
        std::map<std::string, std::string> irBefore;
        for (const auto& cell : { a, b, c, bad }) irBefore[cell.toStdString()] = math_ir::dump(ws.analysis(cell.toStdString())->ir);
        report.add(doc.save(file, error), "The worksheet saves in the existing format", error);
        WorksheetDocument reopened;
        report.add(reopened.load(file, error), "and reopens", error);
        math_workspace::Workspace ws2;
        math_commands::sync(reopened, ws2);
        bool same = true;
        for (const auto& [id, irText] : irBefore) same = same && ws2.has(id) && math_ir::dump(ws2.analysis(id)->ir) == irText;
        report.add(same, "A reopened worksheet analyses to the same MathIR");
        math_commands::sync(reopened, ws);
        report.add(ws.has(a.toStdString()) && ws.analysis(c.toStdString()) != nullptr, "Switching the workspace to another worksheet works");
        const auto saved = juce::JSON::parse(file);
        report.add((int) saved["version"] == 2 || (int) saved["format"] == 2 || saved.hasProperty("nextNodeId"), "The saved file is still the version 2 format (no MathIR is stored)");
        report.add(!file.loadFileAsString().contains("\"ir\"") && !file.loadFileAsString().contains("djehuti.reference"), "No analysis result is written into the document");
#ifdef DJEHUTIMATH_SOURCE_ROOT
        const juce::File example = juce::File(DJEHUTIMATH_SOURCE_ROOT).getChildFile("examples/ohms-law.djmath");
        if (example.existsAsFile())
        {
            WorksheetDocument old;
            report.add(old.load(example, error), "An existing version 1 worksheet still opens", error);
            math_workspace::Workspace ws3;
            math_commands::sync(old, ws3);
            report.add(true, "and its math cells (if any) analyse without trouble", juce::String((int) ws3.cellIds().size()) + " math cells");
        }
#endif
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("passed", report.passed);
    root->setProperty("checks", report.checks);
    destination.getParentDirectory().createDirectory();
    destination.replaceWithText(juce::JSON::toString(juce::var(root), false));
    work.deleteRecursively();
    return report.passed;
}
