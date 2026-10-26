#include "MathSolver.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace math_solver
{
using namespace math_ir;
using math_value::Value;

const char* operationName(Operation op)
{
    switch (op)
    {
        case Operation::Evaluate: return "evaluate";
        case Operation::Simplify: return "simplify";
        case Operation::Solve: return "solve";
        case Operation::Differentiate: return "differentiate";
        case Operation::Integrate: return "integrate";
        default: return "numeric";
    }
}

bool operationFromName(const std::string& name, Operation& out)
{
    for (auto op : { Operation::Evaluate, Operation::Simplify, Operation::Solve, Operation::Differentiate, Operation::Integrate, Operation::Numeric })
        if (name == operationName(op)) { out = op; return true; }
    return false;
}

const char* supportName(Support s)
{
    return s == Support::Full ? "full" : s == Support::Partial ? "partial" : "none";
}

const char* statusName(Status s)
{
    switch (s)
    {
        case Status::Ok: return "ok";
        case Status::Unsupported: return "unsupported";
        case Status::Failed: return "failed";
        case Status::Cancelled: return "cancelled";
        default: return "timed-out";
    }
}

bool Request::interrupted() const
{
    if (cancel != nullptr && cancel->requested()) return true;
    return hasDeadline && std::chrono::steady_clock::now() >= deadline;
}

Support Solver::assess(const Request& request) const
{
    Support best = Support::None;
    for (const auto& c : capabilities())
        if (c.op == request.op && (std::uint8_t) c.support > (std::uint8_t) best) best = c.support;
    return best;
}

// ---- the registry -----------------------------------------------------------------------------------------------------------
void Registry::add(std::shared_ptr<Solver> solver)
{
    if (solver != nullptr) solvers.push_back(std::move(solver));
}

std::vector<EngineCapability> Registry::capabilities() const
{
    std::vector<EngineCapability> out;
    for (const auto& s : solvers)
        for (const auto& c : s->capabilities())
            out.push_back({ s->id(), s->version(), c });
    return out;
}

std::vector<EngineCapability> Registry::discover(Operation op) const
{
    std::vector<EngineCapability> out;
    for (const auto& c : capabilities())
        if (c.capability.op == op && c.capability.support != Support::None) out.push_back(c);
    return out;
}

Solver* Registry::select(const Request& request) const
{
    Solver* best = nullptr;
    Support bestSupport = Support::None;
    for (const auto& s : solvers)
    {
        const auto support = s->assess(request);
        if (support == Support::None) continue;
        if (best == nullptr || (std::uint8_t) support > (std::uint8_t) bestSupport
            || (support == bestSupport && s->priority() > best->priority()))
        {
            best = s.get();
            bestSupport = support;
        }
    }
    return best;
}

Response Registry::run(const Request& request) const
{
    const auto start = std::chrono::steady_clock::now();
    Response response;
    auto stamp = [&](Response& r) {
        r.provenance.cell = request.cell;
        r.provenance.cellRevision = request.cellRevision;
        r.provenance.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        r.value.provenance = r.provenance;
    };
    auto stop = [&](Status status, const char* code, const char* message) {
        response.status = status;
        response.diagnostics.push_back({ Diagnostic::Severity::Error, code, message, {} });
        response.value = Value::failed(code, message);
        stamp(response);
        return response;
    };
    if (request.cancel != nullptr && request.cancel->requested()) return stop(Status::Cancelled, "cancelled", "The computation was cancelled before it started.");
    auto* solver = select(request);
    if (solver == nullptr)
        return stop(Status::Unsupported, "no-engine",
                    (std::string("No registered engine can ") + operationName(request.op) + " this expression.").c_str());
    response = solver->run(request);
    if (response.provenance.engine.empty()) response.provenance.engine = solver->id();
    if (response.provenance.version.empty()) response.provenance.version = solver->version();
    if (response.status == Status::Ok && request.cancel != nullptr && request.cancel->requested())
    {
        response.status = Status::Cancelled;
        response.value = Value::failed("cancelled", "The computation was cancelled.");
    }
    stamp(response);
    return response;
}

// ---- the reference evaluator ---------------------------------------------------------------------------------------------------
namespace
{
struct Stop
{
    Status status = Status::Failed;
    bool undefined = false;       // mathematically undefined (Status::Ok with an Undefined value)
    std::string code, message;
    Source where;
};

struct Num
{
    Scalar s;
    Dim dim = Dim::dimensionless();
};

Num fromRational(const Rational& q, const Dim& d) { return { Scalar::rational(q), d }; }
Num fromReal(double v, const Dim& d) { return { Scalar::real(v), d }; }
Num fromComplex(std::complex<double> z, const Dim& d) { return { Scalar::complexNumber(z.real(), z.imag()), d }; }
bool isComplex(const Num& n) { return n.s.domain == Domain::Complex; }
std::complex<double> asComplex(const Num& n) { return isComplex(n) ? std::complex<double>(n.s.re, n.s.im) : std::complex<double>(n.s.toDouble(), 0.0); }
bool isZero(const Num& n)
{
    if (n.s.isExact()) return n.s.q.isZero();
    return n.s.re == 0.0 && n.s.im == 0.0;
}

bool exactIntegerRoot(std::int64_t v, std::int64_t n, std::int64_t& root)
{
    if (v < 0 || n < 1) return false;
    if (v < 2 || n == 1) { root = v; return true; }
    const auto guess = (std::int64_t) std::llround(std::pow((double) v, 1.0 / (double) n));
    for (std::int64_t r = std::max<std::int64_t>(0, guess - 1); r <= guess + 1; ++r)
    {
        Rational p;
        if (pow(Rational(r), n, p) && p == Rational(v)) { root = r; return true; }
    }
    return false;
}

const char* const kRealFunctions[] = { "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh", "tanh", "exp", "ln", "log",
                                       "abs", "floor", "ceil", "sign", "min", "max", "arcsin", "arccos", "arctan" };

bool knownFunction(const std::string& name)
{
    for (const auto* f : kRealFunctions) if (name == f) return true;
    return false;
}

struct Evaluator
{
    const Request& req;
    std::vector<Diagnostic> warnings;
    std::map<std::string, Num> env;
    bool approximate = false;
    size_t steps = 0;

    explicit Evaluator(const Request& r) : req(r) {}

    [[noreturn]] void fail(const std::string& code, const std::string& message, const ExprPtr& at, bool undefined = false)
    {
        Stop s;
        s.status = Status::Failed; s.undefined = undefined; s.code = code; s.message = message;
        if (at != nullptr) s.where = at->src;
        throw s;
    }

    void poll()
    {
        if ((++steps & 1023u) == 0 && req.interrupted())
        {
            Stop s;
            s.status = (req.cancel != nullptr && req.cancel->requested()) ? Status::Cancelled : Status::TimedOut;
            s.code = s.status == Status::Cancelled ? "cancelled" : "timed-out";
            s.message = s.status == Status::Cancelled ? "The computation was cancelled." : "The computation ran out of time.";
            throw s;
        }
    }

    void warnOverflow(const ExprPtr& at)
    {
        approximate = true;
        for (const auto& w : warnings) if (w.code == "exact-overflow") return;
        warnings.push_back({ Diagnostic::Severity::Warning, "exact-overflow",
                             "Exact arithmetic exceeded 64 bits; the result was computed in floating point.", at != nullptr ? at->src : Source() });
    }

    void requireSame(const Num& a, const Num& b, const ExprPtr& at)
    {
        if (a.dim.known && b.dim.known && a.dim != b.dim)
            fail("dimension-mismatch", "The dimensions " + a.dim.toString() + " and " + b.dim.toString() + " cannot be combined.", at);
    }

    Num addNums(const Num& a, const Num& b, bool subtract, const ExprPtr& at)
    {
        requireSame(a, b, at);
        const Dim d = a.dim.known ? a.dim : b.dim;
        if (a.s.isExact() && b.s.isExact())
        {
            Rational r;
            if (subtract ? sub(a.s.q, b.s.q, r) : add(a.s.q, b.s.q, r)) return fromRational(r, d);
            warnOverflow(at);
            return fromReal(subtract ? a.s.q.toDouble() - b.s.q.toDouble() : a.s.q.toDouble() + b.s.q.toDouble(), d);
        }
        approximate = true;
        if (isComplex(a) || isComplex(b)) return fromComplex(subtract ? asComplex(a) - asComplex(b) : asComplex(a) + asComplex(b), d);
        return fromReal(subtract ? a.s.toDouble() - b.s.toDouble() : a.s.toDouble() + b.s.toDouble(), d);
    }

    Num mulNums(const Num& a, const Num& b, const ExprPtr& at)
    {
        const Dim d = a.dim * b.dim;
        if (a.s.isExact() && b.s.isExact())
        {
            Rational r;
            if (mul(a.s.q, b.s.q, r)) return fromRational(r, d);
            warnOverflow(at);
            return fromReal(a.s.q.toDouble() * b.s.q.toDouble(), d);
        }
        approximate = true;
        if (isComplex(a) || isComplex(b)) return fromComplex(asComplex(a) * asComplex(b), d);
        return fromReal(a.s.toDouble() * b.s.toDouble(), d);
    }

    Num divNums(const Num& a, const Num& b, const ExprPtr& at)
    {
        if (isZero(b)) fail("div-zero", "Division by zero.", at, true);
        const Dim d = a.dim / b.dim;
        if (a.s.isExact() && b.s.isExact())
        {
            Rational r;
            if (div(a.s.q, b.s.q, r)) return fromRational(r, d);
            warnOverflow(at);
            return fromReal(a.s.q.toDouble() / b.s.q.toDouble(), d);
        }
        approximate = true;
        if (isComplex(a) || isComplex(b)) return fromComplex(asComplex(a) / asComplex(b), d);
        return fromReal(a.s.toDouble() / b.s.toDouble(), d);
    }

    Num powNums(const Num& base, const Num& exponent, const ExprPtr& at)
    {
        if (exponent.dim.known && !exponent.dim.isDimensionless())
            fail("dimension-mismatch", "An exponent must be dimensionless.", at);
        const bool exactExponent = exponent.s.isExact();
        Dim d = Dim::dimensionless();
        if (base.dim.known && !base.dim.isDimensionless())
        {
            if (!exactExponent) fail("dimension-mismatch", "A quantity with a dimension needs an exact exponent.", at);
            d = base.dim.power(exponent.s.q);
        }
        else if (!base.dim.known)
            d = Dim::unknown();
        if (isZero(base) && ((exactExponent && exponent.s.q.num <= 0) || (!exactExponent && !isComplex(exponent) && exponent.s.re <= 0.0)))
            fail("undefined-power", "Zero cannot be raised to a power that is not positive.", at, true);
        if (base.s.isExact() && exactExponent && exponent.s.q.isInteger())
        {
            Rational r;
            if (pow(base.s.q, exponent.s.q.num, r)) return fromRational(r, d);
            warnOverflow(at);
            return fromReal(std::pow(base.s.q.toDouble(), exponent.s.q.toDouble()), d);
        }
        approximate = true;
        if (!isComplex(base) && !isComplex(exponent))
        {
            const double b = base.s.toDouble(), e = exponent.s.toDouble();
            if (b >= 0.0 || std::floor(e) == e) return fromReal(std::pow(b, e), d);
        }
        return fromComplex(std::pow(asComplex(base), asComplex(exponent)), d);
    }

    Num rootNums(const Num& x, const Num& index, const ExprPtr& at)
    {
        if (!index.s.isExact() || !index.s.q.isInteger() || index.s.q.num < 1)
            fail("unsupported-root", "The index of a root must be a positive whole number.", at);
        const auto n = index.s.q.num;
        const Dim d = x.dim.known ? x.dim.power(Rational(1, n)) : Dim::unknown();
        if (x.s.isExact())
        {
            const bool negative = x.s.q.num < 0;
            if (!negative || n % 2 == 1)
            {
                std::int64_t rn = 0, rd = 0;
                const auto an = negative ? -x.s.q.num : x.s.q.num;
                if (exactIntegerRoot(an, n, rn) && exactIntegerRoot(x.s.q.den, n, rd))
                    return fromRational(Rational(negative ? -rn : rn, rd), d);
            }
        }
        approximate = true;
        if (!isComplex(x))
        {
            const double v = x.s.toDouble();
            if (v >= 0.0) return fromReal(std::pow(v, 1.0 / (double) n), d);
            if (n % 2 == 1) return fromReal(-std::pow(-v, 1.0 / (double) n), d);
        }
        return fromComplex(std::pow(asComplex(x), 1.0 / (double) n), d);
    }

    Num realArgument(const Num& x, const ExprPtr& at)
    {
        if (isComplex(x)) fail("unsupported-complex", "This function is evaluated for real arguments only.", at);
        return x;
    }

    Num call(const ExprPtr& e)
    {
        const auto& name = e->name;
        std::vector<Num> a;
        for (const auto& arg : e->args) a.push_back(eval(arg));
        auto needDimensionless = [&](const Num& x) {
            if (x.dim.known && !x.dim.isDimensionless()) fail("dimension-mismatch", "The argument of " + name + " must be dimensionless.", e);
        };
        auto arity = [&](size_t n) { if (a.size() != n) fail("arity", name + " takes " + std::to_string(n) + " argument(s).", e); };
        const Dim none = Dim::dimensionless();
        if (name == "min" || name == "max")
        {
            if (a.size() < 2) fail("arity", name + " needs at least two arguments.", e);
            Num best = realArgument(a[0], e);
            for (size_t i = 1; i < a.size(); ++i)
            {
                requireSame(best, a[i], e);
                const auto& c = realArgument(a[i], e);
                if (name == "min" ? c.s.toDouble() < best.s.toDouble() : c.s.toDouble() > best.s.toDouble()) best = c;
            }
            return best;
        }
        arity(1);
        const Num& x = a[0];
        if (name == "abs")
        {
            if (x.s.isExact()) return fromRational(Rational(x.s.q.num < 0 ? -x.s.q.num : x.s.q.num, x.s.q.den), x.dim);
            approximate = true;
            return fromReal(std::abs(asComplex(x)), x.dim);
        }
        if (name == "sign")
        {
            const auto v = realArgument(x, e).s.toDouble();
            return fromRational(Rational(v > 0 ? 1 : v < 0 ? -1 : 0), none);
        }
        if (name == "floor" || name == "ceil")
        {
            const auto& r = realArgument(x, e);
            if (r.s.isExact())
            {
                std::int64_t f = r.s.q.num / r.s.q.den;
                if (r.s.q.num % r.s.q.den != 0 && r.s.q.num < 0) --f;
                if (name == "ceil" && r.s.q.num % r.s.q.den != 0) ++f;
                return fromRational(Rational(f), x.dim);
            }
            approximate = true;
            return fromReal(name == "floor" ? std::floor(r.s.toDouble()) : std::ceil(r.s.toDouble()), x.dim);
        }
        needDimensionless(x);
        approximate = true;
        if (isComplex(x))
        {
            const auto z = asComplex(x);
            if (name == "sin") return fromComplex(std::sin(z), none);
            if (name == "cos") return fromComplex(std::cos(z), none);
            if (name == "tan") return fromComplex(std::tan(z), none);
            if (name == "sinh") return fromComplex(std::sinh(z), none);
            if (name == "cosh") return fromComplex(std::cosh(z), none);
            if (name == "tanh") return fromComplex(std::tanh(z), none);
            if (name == "exp") return fromComplex(std::exp(z), none);
            if (name == "ln") return fromComplex(std::log(z), none);
            fail("unsupported-complex", name + " is evaluated for real arguments only.", e);
        }
        const double v = x.s.toDouble();
        if (name == "sin") return fromReal(std::sin(v), none);
        if (name == "cos") return fromReal(std::cos(v), none);
        if (name == "tan") return fromReal(std::tan(v), none);
        if (name == "sinh") return fromReal(std::sinh(v), none);
        if (name == "cosh") return fromReal(std::cosh(v), none);
        if (name == "tanh") return fromReal(std::tanh(v), none);
        if (name == "exp") return fromReal(std::exp(v), none);
        if (name == "atan" || name == "arctan") return fromReal(std::atan(v), none);
        if (name == "asin" || name == "arcsin" || name == "acos" || name == "arccos")
        {
            if (v < -1.0 || v > 1.0) fail("domain-error", name + " is undefined outside [-1, 1] for real arguments.", e, true);
            return fromReal(name == "asin" || name == "arcsin" ? std::asin(v) : std::acos(v), none);
        }
        if (name == "ln" || name == "log")
        {
            if (v == 0.0) fail("domain-error", "The logarithm of zero is undefined.", e, true);
            if (v < 0.0)
            {
                const auto z = std::log(std::complex<double>(v, 0.0));
                return fromComplex(name == "log" ? z / std::log(10.0) : z, none);
            }
            return fromReal(name == "ln" ? std::log(v) : std::log10(v), none);
        }
        fail("unsupported-function", "The reference evaluator does not know " + name + ".", e);
    }

    Num eval(const ExprPtr& e)
    {
        if (e == nullptr) fail("missing", "An operand is missing.", e);
        poll();
        switch (e->op)
        {
            case Op::Number: return { e->value, Dim::dimensionless() };
            case Op::Constant:
                if (e->name == "pi") return fromReal(3.14159265358979323846, Dim::dimensionless());
                if (e->name == "e") return fromReal(2.71828182845904523536, Dim::dimensionless());
                if (e->name == "i") return fromComplex({ 0.0, 1.0 }, Dim::dimensionless());
                fail("unsupported-constant", "The constant " + e->name + " cannot be evaluated.", e);
            case Op::Unit:
                return { e->value.re == 1.0 ? Scalar::integer(1) : Scalar::real(e->value.re), e->type.dim };
            case Op::Symbol:
            {
                const auto local = env.find(e->name);
                if (local != env.end()) return local->second;
                const auto bound = req.bindings.find(e->name);
                if (bound == req.bindings.end() || !bound->second.isScalar())
                    fail("free-symbol", "The symbol " + e->name + " has no numeric value.", e);
                return { bound->second.scalar, bound->second.dim };
            }
            case Op::Hole: fail("incomplete", "The expression is incomplete.", e);
            case Op::Add:
            {
                Num acc = eval(e->args.at(0));
                for (size_t i = 1; i < e->args.size(); ++i) acc = addNums(acc, eval(e->args[i]), false, e);
                return acc;
            }
            case Op::Mul:
            {
                Num acc = eval(e->args.at(0));
                for (size_t i = 1; i < e->args.size(); ++i) acc = mulNums(acc, eval(e->args[i]), e);
                return acc;
            }
            case Op::Div: return divNums(eval(e->args.at(0)), eval(e->args.at(1)), e);
            case Op::Neg: { const auto x = eval(e->args.at(0)); return mulNums(x, fromRational(Rational(-1), Dim::dimensionless()), e); }
            case Op::Pow: return powNums(eval(e->args.at(0)), eval(e->args.at(1)), e);
            case Op::Root: return rootNums(eval(e->args.at(0)), eval(e->args.at(1)), e);
            case Op::Call: return call(e);
            case Op::Sum: case Op::Product:
            {
                const auto lo = eval(e->args.at(1)), hi = eval(e->args.at(2));
                if (!lo.s.isExact() || !hi.s.isExact() || !lo.s.q.isInteger() || !hi.s.q.isInteger())
                    fail("unsupported-bounds", "The bounds of a sum or product must be whole numbers.", e);
                if (hi.s.q.num - lo.s.q.num > 1000000) fail("too-large", "The sum has more than a million terms.", e);
                Num acc = e->op == Op::Sum ? fromRational(Rational(0), Dim::dimensionless()) : fromRational(Rational(1), Dim::dimensionless());
                bool first = true;
                const auto saved = env.find(e->name) != env.end() ? std::make_shared<Num>(env[e->name]) : nullptr;
                for (std::int64_t i = lo.s.q.num; i <= hi.s.q.num; ++i)
                {
                    env[e->name] = fromRational(Rational(i), Dim::dimensionless());
                    const auto term = eval(e->args[0]);
                    acc = e->op == Op::Sum ? (first ? term : addNums(acc, term, false, e)) : (first ? term : mulNums(acc, term, e));
                    first = false;
                }
                if (saved != nullptr) env[e->name] = *saved; else env.erase(e->name);
                return acc;
            }
            default:
                fail("unsupported-operation", std::string("The reference evaluator does not support ") + opName(e->op) + ".", e);
        }
    }

    Value toValue(const Num& n) const
    {
        Value v;
        switch (n.s.domain)
        {
            case Domain::Integer: case Domain::Rational: v = Value::rational(n.s.q); break;
            case Domain::Complex: v = Value::complexNumber(n.s.re, n.s.im); break;
            default: v = Value::real(n.s.re); break;
        }
        v.dim = n.dim;
        return v;
    }

    bool compare(Op op, const Num& a, const Num& b, const ExprPtr& at)
    {
        requireSame(a, b, at);
        if (isComplex(a) || isComplex(b))
        {
            if (op != Op::Eq && op != Op::Ne) fail("unsupported-complex", "Complex numbers are not ordered.", at);
            const bool same = asComplex(a) == asComplex(b);
            return op == Op::Eq ? same : !same;
        }
        int order = 0;
        if (a.s.isExact() && b.s.isExact()) order = a.s.q < b.s.q ? -1 : (a.s.q == b.s.q ? 0 : 1);
        else
        {
            const double x = a.s.toDouble(), y = b.s.toDouble();
            const double tol = op == Op::Approx ? req.numeric.relativeTolerance : 1e-12;
            const double scale = std::max(std::abs(x), std::abs(y));
            order = std::abs(x - y) <= tol * scale ? 0 : (x < y ? -1 : 1);
        }
        switch (op)
        {
            case Op::Eq: case Op::Approx: return order == 0;
            case Op::Ne: return order != 0;
            case Op::Lt: return order < 0;
            case Op::Le: return order <= 0;
            case Op::Gt: return order > 0;
            default: return order >= 0;
        }
    }

    Value evalValue(const ExprPtr& e)
    {
        if (e != nullptr && isRelation(e->op) && e->args.size() >= 2)
        {
            bool all = true;
            Num left = eval(e->args[0]);
            for (size_t i = 1; i < e->args.size(); ++i)
            {
                const Num right = eval(e->args[i]);
                all = all && compare(e->op, left, right, e);
                left = right;
            }
            return Value::boolean(all);
        }
        if (e != nullptr && e->op == Op::Vector)
        {
            std::vector<Value> elements;
            for (const auto& a : e->args) elements.push_back(evalValue(a));
            return Value::vector(std::move(elements));
        }
        if (e != nullptr && e->op == Op::Matrix)
        {
            std::vector<Value> elements;
            for (const auto& a : e->args) elements.push_back(evalValue(a));
            return Value::matrix(e->rows, e->cols, std::move(elements));
        }
        return toValue(eval(e));
    }
};

bool numericOnly(const ExprPtr& e, const std::map<std::string, Value>& bindings, std::set<std::string>& bound)
{
    if (e == nullptr) return false;
    switch (e->op)
    {
        case Op::Number: case Op::Unit: return true;
        case Op::Constant: return e->name != "infty";
        case Op::Symbol:
        {
            if (bound.count(e->name) > 0) return true;
            const auto it = bindings.find(e->name);
            return it != bindings.end() && it->second.isScalar();
        }
        case Op::Call: if (!knownFunction(e->name)) return false; break;
        case Op::Sum: case Op::Product:
        {
            if (e->args.size() != 3 || !numericOnly(e->args[1], bindings, bound) || !numericOnly(e->args[2], bindings, bound)) return false;
            const bool had = bound.count(e->name) > 0;
            bound.insert(e->name);
            const bool ok = numericOnly(e->args[0], bindings, bound);
            if (!had) bound.erase(e->name);
            return ok;
        }
        case Op::Add: case Op::Mul: case Op::Div: case Op::Neg: case Op::Pow: case Op::Root:
        case Op::Vector: case Op::Matrix: break;
        default:
            if (!isRelation(e->op)) return false;
            break;
    }
    for (const auto& a : e->args)
        if (!numericOnly(a, bindings, bound)) return false;
    return true;
}

class ReferenceEvaluator final : public Solver
{
public:
    std::string id() const override { return "djehuti.reference"; }
    std::string version() const override { return "S0"; }
    int priority() const override { return -100; }    // any real engine outranks the reference one at equal support

    std::vector<Capability> capabilities() const override
    {
        return {
            { Operation::Evaluate, Support::Partial, true, true,
              "Fully numeric expressions only: + - * / powers roots, sums and products with whole-number bounds, common functions, units, relations, vectors and matrices of these. Exact when every step is exact; no free symbols." },
            { Operation::Numeric, Support::Partial, false, true, "The same expressions, always evaluated in floating point." },
        };
    }

    Support assess(const Request& request) const override
    {
        if (request.op != Operation::Evaluate && request.op != Operation::Numeric) return Support::None;
        std::set<std::string> bound;
        return numericOnly(request.expr, request.bindings, bound) ? Support::Partial : Support::None;
    }

    Response run(const Request& request) override
    {
        Response response;
        response.provenance.engine = id();
        response.provenance.version = version();
        Evaluator evaluator(request);
        try
        {
            response.value = evaluator.evalValue(request.expr);
            response.status = Status::Ok;
        }
        catch (const Stop& s)
        {
            response.status = s.undefined ? Status::Ok : s.status;
            response.value = s.undefined ? Value::undefined(s.code, s.message, s.where) : Value::failed(s.code, s.message, s.where);
            response.diagnostics.push_back({ Diagnostic::Severity::Error, s.code, s.message, s.where });
        }
        const bool numericRequested = request.op == Operation::Numeric;
        if (response.status == Status::Ok && response.value.ok() && numericRequested)
        {
            std::function<void(Value&)> toReal = [&](Value& v) {
                if (v.kind == math_value::Kind::Integer || v.kind == math_value::Kind::Rational)
                {
                    const auto d = v.dim;
                    v = Value::real(v.scalar.toDouble());
                    v.dim = d;
                }
                for (auto& element : v.elements) toReal(element);
            };
            toReal(response.value);
            evaluator.approximate = true;
        }
        for (const auto& w : evaluator.warnings)
        {
            response.diagnostics.push_back(w);
            response.value.diagnostics.push_back(w);
        }
        response.provenance.exact = response.status == Status::Ok && response.value.ok() && response.value.isExact() && !evaluator.approximate;
        response.provenance.method = response.provenance.exact ? "exact rational arithmetic" : "IEEE double arithmetic";
        return response;
    }
};
}

std::shared_ptr<Solver> makeReferenceEvaluator() { return std::make_shared<ReferenceEvaluator>(); }

Registry defaultRegistry()
{
    Registry registry;
    registry.add(makeReferenceEvaluator());
    return registry;
}
}
