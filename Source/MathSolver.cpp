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
    {
        std::string message = std::string("No registered engine can ") + operationName(request.op) + " this expression.";
        for (const auto& s : solvers)
        {
            bool offers = false;
            for (const auto& c : s->capabilities()) offers = offers || (c.op == request.op && c.support != Support::None);
            const auto why = offers ? s->whyNot(request) : std::string();
            if (!why.empty()) { message = why; break; }
        }
        return stop(Status::Unsupported, "no-engine", message.c_str());
    }
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
//
// Evaluates fully numeric expressions: real, rational and complex scalars with exact arithmetic where it is
// practical (and an exact-to-float fallback with a warning on overflow), vectors and matrices, units and
// dimensions, user-defined functions and the common elementary functions. It is deliberately not a computer
// algebra system: an expression with an unbound symbol is not evaluated, it is reported.
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

// A scalar, a vector (rows x 1) or a matrix.
struct Val
{
    int form = 0;                 // 0 scalar, 1 vector, 2 matrix
    int rows = 1, cols = 1;
    std::vector<Num> e;
    bool scalar() const { return form == 0; }
};

constexpr int kMaxCollection = 4096;       // elements
constexpr int kMaxCallDepth = 200;

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
Val scalarVal(Num n) { Val v; v.e.push_back(std::move(n)); return v; }

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

const char* const kScalarFunctions[] = { "sin", "cos", "tan", "cot", "sec", "csc", "asin", "acos", "atan", "arcsin", "arccos", "arctan",
                                         "sinh", "cosh", "tanh", "coth", "exp", "ln", "log", "abs", "floor", "ceil", "sign", "sgn",
                                         "min", "max", "re", "im", "conj", "arg", "Re", "Im" };
const char* const kCollectionFunctions[] = { "det", "trace", "tr", "transpose", "norm", "dot", "cross" };

bool oneOf(const std::string& name, const char* const* list, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (name == list[i]) return true;
    return false;
}
bool isScalarFunction(const std::string& name) { return oneOf(name, kScalarFunctions, sizeof kScalarFunctions / sizeof *kScalarFunctions); }
bool isCollectionFunction(const std::string& name) { return oneOf(name, kCollectionFunctions, sizeof kCollectionFunctions / sizeof *kCollectionFunctions); }

// ---- linear algebra helpers ---------------------------------------------------------------------------------------------------------
// Exact Gauss-Jordan elimination over rationals. `inverse` (n*n, may be null) receives the inverse.
enum class Elimination { Ok, Singular, Overflow };

Elimination exactEliminate(int n, std::vector<Rational> a, std::vector<Rational>* inverse, Rational& determinant)
{
    std::vector<Rational> inv;
    if (inverse != nullptr)
    {
        inv.assign((size_t) n * (size_t) n, Rational(0));
        for (int i = 0; i < n; ++i) inv[(size_t) i * (size_t) n + (size_t) i] = Rational(1);
    }
    determinant = Rational(1);
    auto at = [n](std::vector<Rational>& m, int r, int c) -> Rational& { return m[(size_t) r * (size_t) n + (size_t) c]; };
    for (int c = 0; c < n; ++c)
    {
        int pivot = -1;
        for (int r = c; r < n; ++r) if (!at(a, r, c).isZero()) { pivot = r; break; }
        if (pivot < 0) { determinant = Rational(0); return Elimination::Singular; }
        if (pivot != c)
        {
            for (int k = 0; k < n; ++k) { std::swap(at(a, pivot, k), at(a, c, k)); if (inverse != nullptr) std::swap(at(inv, pivot, k), at(inv, c, k)); }
            determinant = Rational(-determinant.num, determinant.den);
        }
        const Rational p = at(a, c, c);
        if (!mul(determinant, p, determinant)) return Elimination::Overflow;
        for (int k = 0; k < n; ++k)
        {
            if (!div(at(a, c, k), p, at(a, c, k))) return Elimination::Overflow;
            if (inverse != nullptr && !div(at(inv, c, k), p, at(inv, c, k))) return Elimination::Overflow;
        }
        for (int r = 0; r < n; ++r)
        {
            if (r == c || at(a, r, c).isZero()) continue;
            const Rational factor = at(a, r, c);
            for (int k = 0; k < n; ++k)
            {
                Rational t;
                if (!mul(factor, at(a, c, k), t) || !sub(at(a, r, k), t, at(a, r, k))) return Elimination::Overflow;
                if (inverse != nullptr && (!mul(factor, at(inv, c, k), t) || !sub(at(inv, r, k), t, at(inv, r, k)))) return Elimination::Overflow;
            }
        }
    }
    if (inverse != nullptr) *inverse = std::move(inv);
    return Elimination::Ok;
}

// The same in complex floating point with partial pivoting.
Elimination floatEliminate(int n, std::vector<std::complex<double>> a, std::vector<std::complex<double>>* inverse, std::complex<double>& determinant)
{
    std::vector<std::complex<double>> inv;
    if (inverse != nullptr)
    {
        inv.assign((size_t) n * (size_t) n, { 0.0, 0.0 });
        for (int i = 0; i < n; ++i) inv[(size_t) i * (size_t) n + (size_t) i] = { 1.0, 0.0 };
    }
    double scale = 0.0;
    for (const auto& x : a) scale = std::max(scale, std::abs(x));
    determinant = { 1.0, 0.0 };
    auto at = [n](std::vector<std::complex<double>>& m, int r, int c) -> std::complex<double>& { return m[(size_t) r * (size_t) n + (size_t) c]; };
    for (int c = 0; c < n; ++c)
    {
        int pivot = c;
        for (int r = c + 1; r < n; ++r) if (std::abs(at(a, r, c)) > std::abs(at(a, pivot, c))) pivot = r;
        if (std::abs(at(a, pivot, c)) <= 1e-14 * scale || std::abs(at(a, pivot, c)) == 0.0) { determinant = { 0.0, 0.0 }; return Elimination::Singular; }
        if (pivot != c)
        {
            for (int k = 0; k < n; ++k) { std::swap(at(a, pivot, k), at(a, c, k)); if (inverse != nullptr) std::swap(at(inv, pivot, k), at(inv, c, k)); }
            determinant = -determinant;
        }
        const auto p = at(a, c, c);
        determinant *= p;
        for (int k = 0; k < n; ++k) { at(a, c, k) /= p; if (inverse != nullptr) at(inv, c, k) /= p; }
        for (int r = 0; r < n; ++r)
        {
            if (r == c) continue;
            const auto factor = at(a, r, c);
            if (factor == std::complex<double>(0.0, 0.0)) continue;
            for (int k = 0; k < n; ++k)
            {
                at(a, r, k) -= factor * at(a, c, k);
                if (inverse != nullptr) at(inv, r, k) -= factor * at(inv, c, k);
            }
        }
    }
    if (inverse != nullptr) *inverse = std::move(inv);
    return Elimination::Ok;
}

struct Evaluator
{
    const Request& req;
    std::vector<Diagnostic> warnings;
    std::map<std::string, Val> env;
    bool approximate = false;
    size_t steps = 0;
    int depth = 0;

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

    // ---- scalars -------------------------------------------------------------------------------------------------------------------
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
        if (isComplex(base) && exactExponent && exponent.s.q.isInteger() && std::abs(exponent.s.q.num) <= 1024)
        {
            // repeated multiplication keeps i^2 exactly -1 instead of -1 + 1e-16 i
            std::complex<double> result(1.0, 0.0), factor = asComplex(base);
            for (std::int64_t k = std::abs(exponent.s.q.num); k > 0; k >>= 1)
            {
                if ((k & 1) != 0) result *= factor;
                factor *= factor;
            }
            if (exponent.s.q.num < 0) result = std::complex<double>(1.0, 0.0) / result;
            return fromComplex(result, d);
        }
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
            if (n == 2) return fromComplex({ 0.0, std::sqrt(-v) }, d);     // exact square root of a negative real
        }
        return fromComplex(std::pow(asComplex(x), 1.0 / (double) n), d);
    }

    Num realArgument(const Num& x, const ExprPtr& at)
    {
        if (isComplex(x)) fail("unsupported-complex", "This function is evaluated for real arguments only.", at);
        return x;
    }

    // ---- collections ----------------------------------------------------------------------------------------------------------------
    void requireSize(int rows, int cols, const ExprPtr& at)
    {
        if ((long long) rows * (long long) cols > kMaxCollection)
            fail("too-large", "A vector or matrix may have at most " + std::to_string(kMaxCollection) + " elements.", at);
    }

    Val shaped(int form, int rows, int cols, std::vector<Num> elements) const
    {
        Val v;
        v.form = form; v.rows = rows; v.cols = cols; v.e = std::move(elements);
        return v;
    }

    Val addVals(const Val& a, const Val& b, bool subtract, const ExprPtr& at)
    {
        if (a.scalar() && b.scalar()) return scalarVal(addNums(a.e[0], b.e[0], subtract, at));
        if (a.form != b.form || a.rows != b.rows || a.cols != b.cols)
            fail("shape-mismatch", "Only values of the same shape can be added or subtracted.", at);
        std::vector<Num> out;
        out.reserve(a.e.size());
        for (size_t i = 0; i < a.e.size(); ++i) out.push_back(addNums(a.e[i], b.e[i], subtract, at));
        return shaped(a.form, a.rows, a.cols, std::move(out));
    }

    Val mulVals(const Val& a, const Val& b, const ExprPtr& at)
    {
        if (a.scalar() && b.scalar()) return scalarVal(mulNums(a.e[0], b.e[0], at));
        if (a.scalar() || b.scalar())
        {
            const Val& collection = a.scalar() ? b : a;
            const Num& factor = a.scalar() ? a.e[0] : b.e[0];
            std::vector<Num> out;
            out.reserve(collection.e.size());
            for (const auto& x : collection.e) out.push_back(a.scalar() ? mulNums(factor, x, at) : mulNums(x, factor, at));
            return shaped(collection.form, collection.rows, collection.cols, std::move(out));
        }
        if (a.form == 2 && (b.form == 1 || b.form == 2))
        {
            if (a.cols != b.rows) fail("shape-mismatch", "The matrix has " + std::to_string(a.cols) + " columns but the other operand has " + std::to_string(b.rows) + " rows.", at);
            requireSize(a.rows, b.cols, at);
            std::vector<Num> out;
            for (int i = 0; i < a.rows; ++i)
                for (int j = 0; j < b.cols; ++j)
                {
                    Num acc = mulNums(a.e[(size_t) i * (size_t) a.cols], b.e[(size_t) j], at);
                    for (int k = 1; k < a.cols; ++k)
                        acc = addNums(acc, mulNums(a.e[(size_t) i * (size_t) a.cols + (size_t) k], b.e[(size_t) k * (size_t) b.cols + (size_t) j], at), false, at);
                    out.push_back(acc);
                    poll();
                }
            return shaped(b.form, a.rows, b.cols, std::move(out));
        }
        fail("vector-product", "This product of vectors is ambiguous; use dot(a, b) or cross(a, b).", at);
    }

    Val divVals(const Val& a, const Val& b, const ExprPtr& at)
    {
        if (!b.scalar()) fail("shape-mismatch", "Only a scalar can divide.", at);
        std::vector<Num> out;
        for (const auto& x : a.e) out.push_back(divNums(x, b.e[0], at));
        return shaped(a.form, a.rows, a.cols, std::move(out));
    }

    Val negVal(const Val& a, const ExprPtr& at)
    {
        std::vector<Num> out;
        for (const auto& x : a.e) out.push_back(mulNums(x, fromRational(Rational(-1), Dim::dimensionless()), at));
        return shaped(a.form, a.rows, a.cols, std::move(out));
    }

    Val identity(int n, const Dim& d) const
    {
        std::vector<Num> out;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) out.push_back(fromRational(Rational(i == j ? 1 : 0), d));
        return shaped(2, n, n, std::move(out));
    }

    bool allExact(const Val& m) const
    {
        for (const auto& x : m.e) if (!x.s.isExact()) return false;
        return true;
    }
    bool anyComplex(const Val& m) const
    {
        for (const auto& x : m.e) if (isComplex(x)) return true;
        return false;
    }
    Dim elementDim(const Val& m) const { return m.e.empty() ? Dim::dimensionless() : m.e[0].dim; }

    void requireSquare(const Val& m, const char* what, const ExprPtr& at)
    {
        if (m.form != 2 || m.rows != m.cols) fail("shape-mismatch", std::string(what) + " needs a square matrix.", at);
    }

    Num determinant(const Val& m, const ExprPtr& at)
    {
        requireSquare(m, "det", at);
        const int n = m.rows;
        const Dim d = elementDim(m).known ? elementDim(m).power(Rational(n)) : Dim::unknown();
        if (allExact(m))
        {
            std::vector<Rational> a;
            for (const auto& x : m.e) a.push_back(x.s.q);
            Rational det;
            const auto r = exactEliminate(n, a, nullptr, det);
            if (r != Elimination::Overflow) return fromRational(det, d);
            warnOverflow(at);
        }
        approximate = true;
        std::vector<std::complex<double>> a;
        for (const auto& x : m.e) a.push_back(asComplex(x));
        std::complex<double> det;
        floatEliminate(n, a, nullptr, det);
        return anyComplex(m) ? fromComplex(det, d) : fromReal(det.real(), d);
    }

    Val inverse(const Val& m, const ExprPtr& at)
    {
        requireSquare(m, "The inverse", at);
        const int n = m.rows;
        const Dim d = elementDim(m).known ? elementDim(m).power(Rational(-1)) : Dim::unknown();
        if (allExact(m))
        {
            std::vector<Rational> a, inv;
            for (const auto& x : m.e) a.push_back(x.s.q);
            Rational det;
            const auto r = exactEliminate(n, a, &inv, det);
            if (r == Elimination::Singular) fail("singular-matrix", "The matrix is singular and has no inverse.", at, true);
            if (r == Elimination::Ok)
            {
                std::vector<Num> out;
                for (const auto& q : inv) out.push_back(fromRational(q, d));
                return shaped(2, n, n, std::move(out));
            }
            warnOverflow(at);
        }
        approximate = true;
        std::vector<std::complex<double>> a, inv;
        for (const auto& x : m.e) a.push_back(asComplex(x));
        std::complex<double> det;
        if (floatEliminate(n, a, &inv, det) == Elimination::Singular) fail("singular-matrix", "The matrix is singular and has no inverse.", at, true);
        std::vector<Num> out;
        const bool complexResult = anyComplex(m);
        for (const auto& z : inv) out.push_back(complexResult ? fromComplex(z, d) : fromReal(z.real(), d));
        return shaped(2, n, n, std::move(out));
    }

    Val powVals(const Val& base, const Val& exponent, const ExprPtr& at)
    {
        if (!exponent.scalar()) fail("shape-mismatch", "An exponent must be a scalar.", at);
        if (base.scalar()) return scalarVal(powNums(base.e[0], exponent.e[0], at));
        if (base.form != 2) fail("shape-mismatch", "A vector cannot be raised to a power.", at);
        requireSquare(base, "A matrix power", at);
        const auto& p = exponent.e[0];
        if (!p.s.isExact() || !p.s.q.isInteger() || std::abs(p.s.q.num) > 64)
            fail("unsupported-power", "A matrix can be raised to a whole-number power of at most 64.", at);
        Val factor = p.s.q.num < 0 ? inverse(base, at) : base;
        Val result = identity(base.rows, elementDim(base).known ? Dim::dimensionless() : Dim::unknown());
        for (std::int64_t k = std::abs(p.s.q.num); k > 0; k >>= 1)
        {
            if ((k & 1) != 0) result = mulVals(result, factor, at);
            if (k > 1) factor = mulVals(factor, factor, at);
        }
        return result;
    }

    // ---- functions --------------------------------------------------------------------------------------------------------------------
    Num scalarArgument(const Val& v, const std::string& name, const ExprPtr& at)
    {
        if (!v.scalar()) fail("shape-mismatch", name + " takes a scalar; use it on the elements of a vector or matrix.", at);
        return v.e[0];
    }

    Val collectionFunction(const std::string& name, const std::vector<Val>& a, const ExprPtr& at)
    {
        auto need = [&](size_t n) { if (a.size() != n) fail("arity", name + " takes " + std::to_string(n) + " argument(s).", at); };
        if (name == "det") { need(1); return scalarVal(determinant(a[0], at)); }
        if (name == "trace" || name == "tr")
        {
            need(1);
            requireSquare(a[0], "trace", at);
            Num acc = a[0].e[0];
            for (int i = 1; i < a[0].rows; ++i) acc = addNums(acc, a[0].e[(size_t) i * (size_t) a[0].cols + (size_t) i], false, at);
            return scalarVal(acc);
        }
        if (name == "transpose")
        {
            need(1);
            if (a[0].form == 0) return a[0];
            std::vector<Num> out;
            for (int j = 0; j < a[0].cols; ++j) for (int i = 0; i < a[0].rows; ++i) out.push_back(a[0].e[(size_t) i * (size_t) a[0].cols + (size_t) j]);
            return shaped(a[0].form == 1 ? 2 : 2, a[0].cols, a[0].rows, std::move(out));
        }
        if (name == "norm")
        {
            need(1);
            if (a[0].scalar()) fail("shape-mismatch", "norm needs a vector or a matrix.", at);
            double sum = 0.0;
            for (const auto& x : a[0].e) sum += std::norm(asComplex(x));
            approximate = true;
            return scalarVal(fromReal(std::sqrt(sum), elementDim(a[0])));
        }
        need(2);
        if (a[0].form != 1 || a[1].form != 1 || a[0].rows != a[1].rows) fail("shape-mismatch", name + " needs two vectors of the same length.", at);
        if (name == "dot")
        {
            Num acc = mulNums(a[0].e[0], a[1].e[0], at);
            for (size_t i = 1; i < a[0].e.size(); ++i) acc = addNums(acc, mulNums(a[0].e[i], a[1].e[i], at), false, at);
            return scalarVal(acc);
        }
        if (a[0].rows != 3) fail("shape-mismatch", "cross needs two vectors of length 3.", at);
        const auto& u = a[0].e;
        const auto& v = a[1].e;
        auto term = [&](int i, int j) { return addNums(mulNums(u[(size_t) i], v[(size_t) j], at), mulNums(u[(size_t) j], v[(size_t) i], at), true, at); };
        std::vector<Num> out { term(1, 2), term(2, 0), term(0, 1) };
        return shaped(1, 3, 1, std::move(out));
    }

    Num scalarCall(const std::string& name, const std::vector<Val>& args, const ExprPtr& e)
    {
        const Dim none = Dim::dimensionless();
        if (name == "min" || name == "max")
        {
            if (args.size() < 2) fail("arity", name + " needs at least two arguments.", e);
            Num best = realArgument(scalarArgument(args[0], name, e), e);
            for (size_t i = 1; i < args.size(); ++i)
            {
                const Num c = realArgument(scalarArgument(args[i], name, e), e);
                requireSame(best, c, e);
                if (name == "min" ? c.s.toDouble() < best.s.toDouble() : c.s.toDouble() > best.s.toDouble()) best = c;
            }
            return best;
        }
        if (args.size() != 1) fail("arity", name + " takes 1 argument.", e);
        const Num x = scalarArgument(args[0], name, e);
        auto needDimensionless = [&] {
            if (x.dim.known && !x.dim.isDimensionless()) fail("dimension-mismatch", "The argument of " + name + " must be dimensionless.", e);
        };
        if (name == "abs")
        {
            if (x.s.isExact()) return fromRational(Rational(x.s.q.num < 0 ? -x.s.q.num : x.s.q.num, x.s.q.den), x.dim);
            approximate = true;
            return fromReal(std::abs(asComplex(x)), x.dim);
        }
        if (name == "re" || name == "Re") return isComplex(x) ? fromReal(x.s.re, x.dim) : x;
        if (name == "im" || name == "Im") return isComplex(x) ? fromReal(x.s.im, x.dim) : fromRational(Rational(0), x.dim);
        if (name == "conj") return isComplex(x) ? fromComplex({ x.s.re, -x.s.im }, x.dim) : x;
        if (name == "arg") { approximate = true; return fromReal(std::atan2(isComplex(x) ? x.s.im : 0.0, x.s.toDouble()), none); }
        if (name == "sign" || name == "sgn")
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
        needDimensionless();
        approximate = true;
        if (name == "cot" || name == "sec" || name == "csc" || name == "coth")
        {
            const std::string base = name == "cot" ? "tan" : name == "sec" ? "cos" : name == "csc" ? "sin" : "tanh";
            std::vector<Val> inner { args[0] };
            const Num f = scalarCall(base, inner, e);
            return divNums(fromRational(Rational(1), none), f, e);
        }
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
            if (name == "log") return fromComplex(std::log(z) / std::log(10.0), none);
            if (name == "asin" || name == "arcsin") return fromComplex(std::asin(z), none);
            if (name == "acos" || name == "arccos") return fromComplex(std::acos(z), none);
            if (name == "atan" || name == "arctan") return fromComplex(std::atan(z), none);
            fail("unsupported-complex", name + " is not evaluated for complex arguments.", e);
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
            if (v < -1.0 || v > 1.0)    // principal value, complex
            {
                const std::complex<double> z(v, 0.0);
                return fromComplex(name == "asin" || name == "arcsin" ? std::asin(z) : std::acos(z), none);
            }
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

    Val call(const ExprPtr& e)
    {
        std::vector<Val> args;
        for (const auto& arg : e->args) args.push_back(evalV(arg));
        const auto user = req.functions.find(e->name);
        if (user != req.functions.end())
        {
            const auto& fn = user->second;
            if (fn.params.size() != args.size()) fail("arity", e->name + " takes " + std::to_string(fn.params.size()) + " argument(s).", e);
            if (++depth > kMaxCallDepth) fail("too-deep", "Function calls are nested too deeply.", e);
            std::map<std::string, Val> saved;
            std::set<std::string> absent;
            for (size_t i = 0; i < fn.params.size(); ++i)
            {
                const auto found = env.find(fn.params[i]);
                if (found != env.end()) saved[fn.params[i]] = found->second; else absent.insert(fn.params[i]);
                env[fn.params[i]] = args[i];
            }
            Val result;
            try { result = evalV(fn.body); }
            catch (...) { restore(saved, absent); --depth; throw; }
            restore(saved, absent);
            --depth;
            return result;
        }
        if (isCollectionFunction(e->name)) return collectionFunction(e->name, args, e);
        return scalarVal(scalarCall(e->name, args, e));
    }

    void restore(const std::map<std::string, Val>& saved, const std::set<std::string>& absent)
    {
        for (const auto& s : saved) env[s.first] = s.second;
        for (const auto& a : absent) env.erase(a);
    }

    Val fromValue(const math_value::Value& v, const ExprPtr& at)
    {
        using K = math_value::Kind;
        auto num = [&](const math_value::Value& x) -> Num {
            if (!x.isScalar()) fail("free-symbol", "A value of this kind cannot be used in a calculation.", at);
            return { x.scalar, x.dim };
        };
        if (v.isScalar()) return scalarVal(num(v));
        if (v.kind == K::Vector || v.kind == K::Matrix)
        {
            std::vector<Num> out;
            for (const auto& x : v.elements) out.push_back(num(x));
            return shaped(v.kind == K::Vector ? 1 : 2, v.rows, v.cols, std::move(out));
        }
        fail("free-symbol", "A value of this kind cannot be used in a calculation.", at);
    }

    Num evalN(const ExprPtr& e)
    {
        const auto v = evalV(e);
        if (!v.scalar()) fail("shape-mismatch", "A scalar is needed here.", e);
        return v.e[0];
    }

    Val evalV(const ExprPtr& e)
    {
        if (e == nullptr) fail("missing", "An operand is missing.", e);
        poll();
        switch (e->op)
        {
            case Op::Number: return scalarVal({ e->value, Dim::dimensionless() });
            case Op::Constant:
                if (e->name == "pi") return scalarVal(fromReal(3.14159265358979323846, Dim::dimensionless()));
                if (e->name == "e") return scalarVal(fromReal(2.71828182845904523536, Dim::dimensionless()));
                if (e->name == "i") return scalarVal(fromComplex({ 0.0, 1.0 }, Dim::dimensionless()));
                fail("unsupported-constant", "The constant " + e->name + " cannot be evaluated.", e);
            case Op::Unit:
                return scalarVal({ e->value.re == 1.0 ? Scalar::integer(1) : Scalar::real(e->value.re), e->type.dim });
            case Op::Symbol:
            {
                const auto local = env.find(e->name);
                if (local != env.end()) return local->second;
                const auto bound = req.bindings.find(e->name);
                if (bound == req.bindings.end()) fail("free-symbol", "The symbol " + e->name + " has no numeric value.", e);
                return fromValue(bound->second, e);
            }
            case Op::Hole: fail("incomplete", "The expression is incomplete.", e);
            case Op::Add:
            {
                Val acc = evalV(e->args.at(0));
                for (size_t i = 1; i < e->args.size(); ++i) acc = addVals(acc, evalV(e->args[i]), false, e);
                return acc;
            }
            case Op::Mul:
            {
                Val acc = evalV(e->args.at(0));
                for (size_t i = 1; i < e->args.size(); ++i) acc = mulVals(acc, evalV(e->args[i]), e);
                return acc;
            }
            case Op::Div: { const auto a = evalV(e->args.at(0)); return divVals(a, evalV(e->args.at(1)), e); }
            case Op::Neg: return negVal(evalV(e->args.at(0)), e);
            case Op::Pow: { const auto b = evalV(e->args.at(0)); return powVals(b, evalV(e->args.at(1)), e); }
            case Op::Root: { const auto x = evalN(e->args.at(0)); return scalarVal(rootNums(x, evalN(e->args.at(1)), e)); }
            case Op::Call: return call(e);
            case Op::Vector:
            {
                std::vector<Num> out;
                for (const auto& a : e->args) out.push_back(evalN(a));
                const int count = (int) out.size();
                requireSize(count, 1, e);
                return shaped(1, count, 1, std::move(out));
            }
            case Op::Matrix:
            {
                std::vector<Num> out;
                requireSize(e->rows, e->cols, e);
                for (const auto& a : e->args) out.push_back(evalN(a));
                return shaped(2, e->rows, e->cols, std::move(out));
            }
            case Op::Index:
            {
                const auto base = evalV(e->args.at(0));
                if (base.scalar()) fail("shape-mismatch", "Only a vector or a matrix can be indexed.", e);
                std::vector<int> at;
                for (size_t i = 1; i < e->args.size(); ++i)
                {
                    const auto n = evalN(e->args[i]);
                    if (!n.s.isExact() || !n.s.q.isInteger()) fail("index-range", "An index must be a whole number.", e);
                    at.push_back((int) n.s.q.num);
                }
                const bool vectorIndex = base.form == 1 && at.size() == 1;
                const bool matrixIndex = base.form == 2 && at.size() == 2;
                if (!vectorIndex && !matrixIndex) fail("index-range", "A vector takes one index and a matrix two.", e);
                const int row = at[0], col = vectorIndex ? 1 : at[1];
                if (row < 1 || row > base.rows || col < 1 || col > base.cols) fail("index-range", "The index is outside the vector or matrix (indices start at 1).", e);
                return scalarVal(base.e[(size_t) (row - 1) * (size_t) base.cols + (size_t) (col - 1)]);
            }
            case Op::Sum: case Op::Product:
            {
                const auto lo = evalN(e->args.at(1)), hi = evalN(e->args.at(2));
                if (!lo.s.isExact() || !hi.s.isExact() || !lo.s.q.isInteger() || !hi.s.q.isInteger())
                    fail("unsupported-bounds", "The bounds of a sum or product must be whole numbers.", e);
                if (hi.s.q.num - lo.s.q.num > 1000000) fail("too-large", "The sum has more than a million terms.", e);
                Num acc = fromRational(Rational(e->op == Op::Sum ? 0 : 1), Dim::dimensionless());
                bool first = true;
                const bool had = env.find(e->name) != env.end();
                const Val saved = had ? env[e->name] : Val();
                for (std::int64_t i = lo.s.q.num; i <= hi.s.q.num; ++i)
                {
                    env[e->name] = scalarVal(fromRational(Rational(i), Dim::dimensionless()));
                    const auto term = evalN(e->args[0]);
                    acc = e->op == Op::Sum ? (first ? term : addNums(acc, term, false, e)) : (first ? term : mulNums(acc, term, e));
                    first = false;
                }
                if (had) env[e->name] = saved; else env.erase(e->name);
                return scalarVal(acc);
            }
            default:
                fail("unsupported-operation", std::string("The reference evaluator does not support ") + opName(e->op) + ".", e);
        }
    }

    Value toValue(const Val& v) const
    {
        auto one = [](const Num& n) {
            Value x;
            switch (n.s.domain)
            {
                case Domain::Integer: case Domain::Rational: x = Value::rational(n.s.q); break;
                case Domain::Complex: x = Value::complexNumber(n.s.re, n.s.im); break;
                default: x = Value::real(n.s.re); break;
            }
            x.dim = n.dim;
            return x;
        };
        if (v.scalar()) return one(v.e[0]);
        std::vector<Value> elements;
        for (const auto& n : v.e) elements.push_back(one(n));
        return v.form == 1 ? Value::vector(std::move(elements)) : Value::matrix(v.rows, v.cols, std::move(elements));
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

    bool compareVals(Op op, const Val& a, const Val& b, const ExprPtr& at)
    {
        if (a.scalar() && b.scalar()) return compare(op, a.e[0], b.e[0], at);
        if (op != Op::Eq && op != Op::Ne) fail("shape-mismatch", "Vectors and matrices can only be compared for equality.", at);
        if (a.form != b.form || a.rows != b.rows || a.cols != b.cols) fail("shape-mismatch", "Both sides must have the same shape.", at);
        bool all = true;
        for (size_t i = 0; i < a.e.size(); ++i) all = all && compare(Op::Eq, a.e[i], b.e[i], at);
        return op == Op::Eq ? all : !all;
    }

    Value evalValue(const ExprPtr& e)
    {
        if (e != nullptr && isRelation(e->op) && e->args.size() >= 2)
        {
            bool all = true;
            Val left = evalV(e->args[0]);
            for (size_t i = 1; i < e->args.size(); ++i)
            {
                const Val right = evalV(e->args[i]);
                all = all && compareVals(e->op, left, right, e);
                left = right;
            }
            return Value::boolean(all);
        }
        return toValue(evalV(e));
    }
};

// What stops an expression from being evaluated here, in words ("" when nothing does).
struct Offender
{
    const Request& req;
    std::set<std::string> bound;
    int depth = 0;

    explicit Offender(const Request& r) : req(r) {}

    std::string find(const ExprPtr& e)
    {
        if (e == nullptr) return "An operand is missing.";
        switch (e->op)
        {
            case Op::Number: case Op::Unit: return {};
            case Op::Constant: return e->name == "infty" ? "Infinity cannot be used in a numeric calculation." : std::string();
            case Op::Hole: return "The expression is incomplete.";
            case Op::Symbol:
            {
                if (bound.count(e->name) > 0) return {};
                const auto it = req.bindings.find(e->name);
                if (it == req.bindings.end())
                    return "'" + e->name + "' has no value: define it in a Definition cell to calculate with it.";
                const auto k = it->second.kind;
                if (k == math_value::Kind::Symbolic || k == math_value::Kind::Boolean || !it->second.ok())
                    return "'" + e->name + "' has no numeric value.";
                return {};
            }
            case Op::Call:
            {
                const auto user = req.functions.find(e->name);
                if (user != req.functions.end())
                {
                    if (depth > 8) return {};
                    ++depth;
                    const auto saved = bound;
                    for (const auto& p : user->second.params) bound.insert(p);
                    auto why = find(user->second.body);
                    bound = saved;
                    --depth;
                    if (!why.empty()) return "in " + e->name + ": " + why;
                }
                else if (!isScalarFunction(e->name) && !isCollectionFunction(e->name))
                    return "The function '" + e->name + "' is not available to the numeric evaluator.";
                break;
            }
            case Op::Sum: case Op::Product:
            {
                if (e->args.size() != 3) return "A malformed sum.";
                auto why = find(e->args[1]);
                if (why.empty()) why = find(e->args[2]);
                if (!why.empty()) return why;
                const bool had = bound.count(e->name) > 0;
                bound.insert(e->name);
                why = find(e->args[0]);
                if (!had) bound.erase(e->name);
                return why;
            }
            case Op::Add: case Op::Mul: case Op::Div: case Op::Neg: case Op::Pow: case Op::Root: case Op::Vector: case Op::Matrix: case Op::Index:
                break;
            case Op::Integral: return "An integral needs a symbolic or numerical integration engine, which is not available yet.";
            case Op::Derivative: return "A derivative needs a symbolic differentiation engine, which is not available yet.";
            case Op::System: return "A system of equations needs an equation solver, which is not available yet.";
            case Op::Define: return "A definition has no value of its own; its right-hand side is evaluated.";
            default:
                if (!isRelation(e->op)) return std::string("The ") + opName(e->op) + " operation is not available to the numeric evaluator.";
                break;
        }
        for (const auto& a : e->args)
        {
            const auto why = find(a);
            if (!why.empty()) return why;
        }
        return {};
    }
};

class ReferenceEvaluator final : public Solver
{
public:
    std::string id() const override { return "djehuti.reference"; }
    std::string version() const override { return "S1"; }
    int priority() const override { return -100; }    // any real engine outranks the reference one at equal support

    std::vector<Capability> capabilities() const override
    {
        return {
            { Operation::Evaluate, Support::Partial, true, true,
              "Fully numeric expressions: real, rational and complex arithmetic (exact where practical), powers and roots, elementary functions, "
              "units and dimensions, vectors and matrices (sum, product, power, inverse, det, trace, transpose, dot, cross), user-defined functions, "
              "sums and products with whole-number bounds, relations. Every symbol must have a value; no symbolic work, integrals or derivatives." },
            { Operation::Numeric, Support::Partial, false, true, "The same expressions, always evaluated in floating point." },
        };
    }

    Support assess(const Request& request) const override
    {
        if (request.op != Operation::Evaluate && request.op != Operation::Numeric) return Support::None;
        return whyNot(request).empty() ? Support::Partial : Support::None;
    }

    std::string whyNot(const Request& request) const override
    {
        if (request.op != Operation::Evaluate && request.op != Operation::Numeric) return {};
        Offender offender(request);
        return offender.find(request.expr);
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
