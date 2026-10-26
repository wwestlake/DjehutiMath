#include "MathIR.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace math_ir
{
namespace
{
constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

bool addOverflows(std::int64_t a, std::int64_t b, std::int64_t& r)
{
    if ((b > 0 && a > kMax - b) || (b < 0 && a < kMin - b))
        return true;
    r = a + b;
    return false;
}

bool mulOverflows(std::int64_t a, std::int64_t b, std::int64_t& r)
{
    if (a == 0 || b == 0) { r = 0; return false; }
    if ((a == -1 && b == kMin) || (b == -1 && a == kMin))
        return true;
    if (a > 0) { if (b > 0 ? a > kMax / b : b < kMin / a) return true; }
    else { if (b > 0 ? a < kMin / b : b < kMax / a) return true; }
    r = a * b;
    return false;
}

std::int64_t gcd64(std::int64_t a, std::int64_t b)
{
    std::uint64_t x = a < 0 ? 0 - (std::uint64_t) a : (std::uint64_t) a, y = b < 0 ? 0 - (std::uint64_t) b : (std::uint64_t) b;
    while (y != 0) { const auto t = x % y; x = y; y = t; }
    return (std::int64_t) x;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

std::uint64_t hashString(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

std::uint64_t hashDouble(double v)
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return bits;
}

const char* const kDimSymbols[7] = { "m", "kg", "s", "A", "K", "mol", "cd" };

struct OpName { Op op; const char* name; };
const OpName kOpNames[] = {
    { Op::Number, "num" }, { Op::Symbol, "sym" }, { Op::Constant, "const" }, { Op::Unit, "unit" }, { Op::Hole, "hole" },
    { Op::Add, "add" }, { Op::Mul, "mul" }, { Op::Div, "div" }, { Op::Neg, "neg" }, { Op::Pow, "pow" }, { Op::Root, "root" },
    { Op::Call, "call" }, { Op::Sum, "sum" }, { Op::Product, "product" }, { Op::Integral, "integral" },
    { Op::Derivative, "derivative" }, { Op::Limit, "limit" }, { Op::Index, "index" },
    { Op::Eq, "eq" }, { Op::Ne, "ne" }, { Op::Lt, "lt" }, { Op::Le, "le" }, { Op::Gt, "gt" }, { Op::Ge, "ge" },
    { Op::Approx, "approx" }, { Op::Define, "define" },
    { Op::Vector, "vector" }, { Op::Matrix, "matrix" }, { Op::Tuple, "tuple" }, { Op::System, "system" },
};

std::uint64_t computeHash(const Expr& e)
{
    std::uint64_t h = mix(0, (std::uint64_t) e.op);
    h = mix(h, hashString(e.name));
    h = mix(h, (std::uint64_t) e.value.domain);
    h = mix(h, (std::uint64_t) e.value.q.num);
    h = mix(h, (std::uint64_t) e.value.q.den);
    h = mix(h, hashDouble(e.value.re));
    h = mix(h, hashDouble(e.value.im));
    h = mix(h, (std::uint64_t) (std::int64_t) e.rows);
    h = mix(h, (std::uint64_t) (std::int64_t) e.cols);
    for (const auto& a : e.args)
        h = mix(h, a != nullptr ? a->hash : 0x51ull);
    return h;
}

ExprPtr finish(std::shared_ptr<Expr> e)
{
    e->hash = computeHash(*e);
    return e;
}

void collectFree(const ExprPtr& e, std::set<std::string>& bound, std::set<std::string>& out)
{
    if (e == nullptr)
        return;
    if (e->op == Op::Symbol)
    {
        if (bound.count(e->name) == 0)
            out.insert(e->name);
        return;
    }
    if (isBinder(e->op) && e->args.size() == 3)
    {
        collectFree(e->args[1], bound, out);
        collectFree(e->args[2], bound, out);
        const bool already = bound.count(e->name) > 0;
        bound.insert(e->name);
        collectFree(e->args[0], bound, out);
        if (!already)
            bound.erase(e->name);
        return;
    }
    if (e->op == Op::Define && e->args.size() == 2)
    {
        std::set<std::string> params;
        if (e->args[0] != nullptr && e->args[0]->op == Op::Call)
            for (const auto& p : e->args[0]->args)
                if (p != nullptr && p->op == Op::Symbol && bound.count(p->name) == 0)
                    params.insert(p->name);
        for (const auto& p : params) bound.insert(p);
        collectFree(e->args[1], bound, out);
        for (const auto& p : params) bound.erase(p);
        return;
    }
    for (const auto& a : e->args)
        collectFree(a, bound, out);
}

ExprPtr substituteIn(const ExprPtr& e, const std::map<std::string, ExprPtr>& values)
{
    if (e == nullptr || values.empty())
        return e;
    if (e->op == Op::Symbol)
    {
        const auto it = values.find(e->name);
        return it != values.end() ? it->second : e;
    }
    std::vector<ExprPtr> args;
    args.reserve(e->args.size());
    bool changed = false;
    for (size_t i = 0; i < e->args.size(); ++i)
    {
        ExprPtr next;
        if (isBinder(e->op) && i == 0)
        {
            auto inner = values;
            inner.erase(e->name);
            next = substituteIn(e->args[i], inner);
        }
        else if (e->op == Op::Define && i == 0)
            next = e->args[i];          // the head names what is defined; it is not substituted
        else
            next = substituteIn(e->args[i], values);
        changed = changed || next != e->args[i];
        args.push_back(next);
    }
    if (!changed)
        return e;
    auto copy = std::make_shared<Expr>(*e);
    copy->args = std::move(args);
    return finish(copy);
}

void dumpInto(const ExprPtr& e, std::string& out)
{
    if (e == nullptr) { out += "_"; return; }
    switch (e->op)
    {
        case Op::Number: out += e->value.toString(); return;
        case Op::Symbol: out += e->name; return;
        case Op::Constant: out += "#" + e->name; return;
        case Op::Unit: out += "[" + e->name + "]"; return;
        case Op::Hole: out += "?"; return;
        default: break;
    }
    out += "(";
    out += opName(e->op);
    if (!e->name.empty())
        out += ":" + e->name;
    if (e->op == Op::Matrix)
        out += ":" + std::to_string(e->rows) + "x" + std::to_string(e->cols);
    if (e->op == Op::Derivative)
        out += "^" + std::to_string(e->rows) + (e->cols != 0 ? "p" : "");
    for (const auto& a : e->args) { out += " "; dumpInto(a, out); }
    out += ")";
}
}

// ---- Rational ---------------------------------------------------------------------------------------------------------
Rational::Rational(std::int64_t n, std::int64_t d)
{
    if (d == 0) { num = 0; den = 0; return; }
    if (d < 0)
    {
        if (n == kMin || d == kMin) { num = 0; den = 0; return; }
        n = -n; d = -d;
    }
    const auto g = gcd64(n, d);
    num = g > 1 ? n / g : n;
    den = g > 1 ? d / g : d;
}

bool Rational::operator<(const Rational& o) const
{
    std::int64_t a = 0, b = 0;
    if (mulOverflows(num, o.den, a) || mulOverflows(o.num, den, b))
        return toDouble() < o.toDouble();
    return a < b;
}

std::string Rational::toString() const
{
    if (!ok()) return "undefined";
    return den == 1 ? std::to_string(num) : std::to_string(num) + "/" + std::to_string(den);
}

bool add(const Rational& a, const Rational& b, Rational& out)
{
    if (!a.ok() || !b.ok()) return false;
    std::int64_t x = 0, y = 0, n = 0, d = 0;
    if (mulOverflows(a.num, b.den, x) || mulOverflows(b.num, a.den, y) || addOverflows(x, y, n) || mulOverflows(a.den, b.den, d))
        return false;
    out = Rational(n, d);
    return out.ok();
}

bool sub(const Rational& a, const Rational& b, Rational& out)
{
    if (b.num == kMin) return false;
    return add(a, Rational(-b.num, b.den), out);
}

bool mul(const Rational& a, const Rational& b, Rational& out)
{
    if (!a.ok() || !b.ok()) return false;
    std::int64_t n = 0, d = 0;
    if (mulOverflows(a.num, b.num, n) || mulOverflows(a.den, b.den, d))
        return false;
    out = Rational(n, d);
    return out.ok();
}

bool div(const Rational& a, const Rational& b, Rational& out)
{
    if (!a.ok() || !b.ok() || b.num == 0) return false;
    return mul(a, Rational(b.den, b.num), out);
}

bool pow(const Rational& base, std::int64_t exponent, Rational& out)
{
    if (!base.ok()) return false;
    if (exponent < 0)
    {
        if (base.num == 0) return false;
        Rational inverse;
        if (!div(Rational(1), base, inverse) || exponent == kMin) return false;
        return pow(inverse, -exponent, out);
    }
    Rational result(1), factor = base;
    for (std::int64_t e = exponent; e > 0; e >>= 1)
    {
        if ((e & 1) != 0 && !mul(result, factor, result)) return false;
        if (e > 1 && !mul(factor, factor, factor)) return false;
    }
    out = result;
    return true;
}

// ---- Dim -------------------------------------------------------------------------------------------------------------------
Dim Dim::base(int index)
{
    Dim d;
    if (index >= 0 && index < 7) d.e[(size_t) index] = Rational(1);
    return d;
}

bool Dim::isDimensionless() const
{
    if (!known) return false;
    for (const auto& x : e) if (!x.isZero()) return false;
    return true;
}

Dim Dim::operator*(const Dim& o) const
{
    if (!known || !o.known) return unknown();
    Dim r;
    for (size_t i = 0; i < 7; ++i)
        if (!add(e[i], o.e[i], r.e[i])) return unknown();
    return r;
}

Dim Dim::operator/(const Dim& o) const
{
    if (!known || !o.known) return unknown();
    Dim r;
    for (size_t i = 0; i < 7; ++i)
        if (!sub(e[i], o.e[i], r.e[i])) return unknown();
    return r;
}

Dim Dim::power(const Rational& p) const
{
    if (!known) return unknown();
    Dim r;
    for (size_t i = 0; i < 7; ++i)
        if (!mul(e[i], p, r.e[i])) return unknown();
    return r;
}

std::string Dim::toString() const
{
    if (!known) return "?";
    std::string out;
    for (size_t i = 0; i < 7; ++i)
    {
        if (e[i].isZero()) continue;
        if (!out.empty()) out += "*";
        out += kDimSymbols[i];
        if (e[i] != Rational(1))
            out += "^" + e[i].toString();
    }
    return out.empty() ? "1" : out;
}

// ---- types -------------------------------------------------------------------------------------------------------------------
const char* domainName(Domain d)
{
    switch (d)
    {
        case Domain::Integer: return "integer";
        case Domain::Rational: return "rational";
        case Domain::Real: return "real";
        case Domain::Complex: return "complex";
        default: return "unknown";
    }
}

const char* shapeName(Shape s)
{
    switch (s)
    {
        case Shape::Scalar: return "scalar";
        case Shape::Vector: return "vector";
        case Shape::Matrix: return "matrix";
        case Shape::Relation: return "relation";
        case Shape::Function: return "function";
        case Shape::System: return "system";
        case Shape::Hole: return "incomplete";
        default: return "unknown";
    }
}

const char* signName(Sign s)
{
    switch (s)
    {
        case Sign::Zero: return "zero";
        case Sign::Positive: return "positive";
        case Sign::Nonnegative: return "nonnegative";
        case Sign::Negative: return "negative";
        case Sign::Nonpositive: return "nonpositive";
        case Sign::Nonzero: return "nonzero";
        default: return "unknown";
    }
}

bool domainFromName(const std::string& name, Domain& out)
{
    if (name == "N" || name == "Z" || name == "integer" || name == "integers") out = Domain::Integer;
    else if (name == "Q" || name == "rational" || name == "rationals") out = Domain::Rational;
    else if (name == "R" || name == "real" || name == "reals") out = Domain::Real;
    else if (name == "C" || name == "complex") out = Domain::Complex;
    else return false;
    return true;
}

Domain join(Domain a, Domain b)
{
    if (a == Domain::Unknown) return b;
    if (b == Domain::Unknown) return a;
    return (std::uint8_t) a >= (std::uint8_t) b ? a : b;
}

bool Type::operator==(const Type& o) const
{
    return shape == o.shape && domain == o.domain && dim == o.dim && sign == o.sign && rows == o.rows && cols == o.cols;
}

std::string Type::toString() const
{
    std::string out = shapeName(shape);
    if (shape == Shape::Scalar || shape == Shape::Vector || shape == Shape::Matrix)
    {
        out += std::string(" ") + domainName(domain);
        if (shape == Shape::Vector) out += "[" + std::to_string(rows) + "]";
        if (shape == Shape::Matrix) out += "[" + std::to_string(rows) + "x" + std::to_string(cols) + "]";
        out += " dim " + dim.toString();
        if (sign != Sign::Unknown) out += std::string(" ") + signName(sign);
    }
    return out;
}

// ---- scalars ------------------------------------------------------------------------------------------------------------------
Scalar Scalar::integer(std::int64_t v) { Scalar s; s.domain = Domain::Integer; s.q = Rational(v); return s; }
Scalar Scalar::rational(const Rational& q) { Scalar s; s.domain = q.isInteger() ? Domain::Integer : Domain::Rational; s.q = q; return s; }
Scalar Scalar::real(double v, std::string text) { Scalar s; s.domain = Domain::Real; s.re = v; s.text = std::move(text); return s; }
Scalar Scalar::complexNumber(double re, double im) { Scalar s; s.domain = Domain::Complex; s.re = re; s.im = im; return s; }

double Scalar::toDouble() const { return isExact() ? q.toDouble() : re; }

bool Scalar::operator==(const Scalar& o) const
{
    if (domain != o.domain) return false;
    if (isExact()) return q == o.q;
    return re == o.re && im == o.im;
}

std::string Scalar::toString() const
{
    char buffer[64];
    if (isExact()) return q.toString();
    if (domain == Domain::Real)
    {
        if (!text.empty()) return text;
        std::snprintf(buffer, sizeof buffer, "%.17g", re);
        return buffer;
    }
    std::snprintf(buffer, sizeof buffer, "%.17g%+.17gi", re, im);
    return buffer;
}

const char* severityName(Diagnostic::Severity s)
{
    switch (s)
    {
        case Diagnostic::Severity::Info: return "info";
        case Diagnostic::Severity::Warning: return "warning";
        default: return "error";
    }
}

// ---- expressions ------------------------------------------------------------------------------------------------------------
const char* opName(Op op)
{
    for (const auto& n : kOpNames) if (n.op == op) return n.name;
    return "?";
}

bool isRelation(Op op) { return op >= Op::Eq && op <= Op::Approx; }
bool isBinder(Op op) { return op == Op::Sum || op == Op::Product || op == Op::Integral; }

ExprPtr makeNumber(const Scalar& value, const Type& type, const Source& src)
{
    auto e = std::make_shared<Expr>();
    e->op = Op::Number; e->value = value; e->type = type; e->src = src;
    return finish(e);
}

ExprPtr makeSymbol(const std::string& name, const Type& type, const Source& src)
{
    auto e = std::make_shared<Expr>();
    e->op = Op::Symbol; e->name = name; e->type = type; e->src = src;
    return finish(e);
}

ExprPtr makeConstant(const std::string& name, const Scalar& value, const Type& type, const Source& src)
{
    auto e = std::make_shared<Expr>();
    e->op = Op::Constant; e->name = name; e->value = value; e->type = type; e->src = src;
    return finish(e);
}

ExprPtr makeUnit(const std::string& name, double scaleToSi, const Dim& dim, const Source& src)
{
    auto e = std::make_shared<Expr>();
    e->op = Op::Unit; e->name = name; e->value = Scalar::real(scaleToSi);
    e->type.shape = Shape::Scalar; e->type.domain = Domain::Real; e->type.dim = dim; e->type.sign = Sign::Positive;
    e->src = src;
    return finish(e);
}

ExprPtr makeHole(const std::string& reason, const Source& src)
{
    auto e = std::make_shared<Expr>();
    e->op = Op::Hole; e->name = reason; e->src = src;
    e->type.shape = Shape::Hole;
    return finish(e);
}

ExprPtr makeNode(Op op, std::vector<ExprPtr> args, const Type& type, const Source& src, const std::string& name,
                 std::int32_t rows, std::int32_t cols)
{
    auto e = std::make_shared<Expr>();
    e->op = op; e->args = std::move(args); e->type = type; e->src = src; e->name = name; e->rows = rows; e->cols = cols;
    return finish(e);
}

ExprPtr withType(const ExprPtr& e, const Type& type)
{
    auto copy = std::make_shared<Expr>(*e);
    copy->type = type;
    return copy;     // structural fields are unchanged, so the hash stays valid
}

ExprPtr withSource(const ExprPtr& e, const Source& src)
{
    auto copy = std::make_shared<Expr>(*e);
    copy->src = src;
    return copy;
}

bool equals(const ExprPtr& a, const ExprPtr& b)
{
    if (a == b) return true;
    if (a == nullptr || b == nullptr) return false;
    if (a->hash != b->hash || a->op != b->op || a->name != b->name || !(a->value == b->value)
        || a->rows != b->rows || a->cols != b->cols || a->args.size() != b->args.size())
        return false;
    for (size_t i = 0; i < a->args.size(); ++i)
        if (!equals(a->args[i], b->args[i])) return false;
    return true;
}

bool hasHole(const ExprPtr& e)
{
    if (e == nullptr) return false;
    if (e->op == Op::Hole) return true;
    for (const auto& a : e->args) if (hasHole(a)) return true;
    return false;
}

int countNodes(const ExprPtr& e)
{
    if (e == nullptr) return 0;
    int n = 1;
    for (const auto& a : e->args) n += countNodes(a);
    return n;
}

std::set<std::string> freeSymbols(const ExprPtr& e)
{
    std::set<std::string> bound, out;
    collectFree(e, bound, out);
    return out;
}

ExprPtr substitute(const ExprPtr& e, const std::map<std::string, ExprPtr>& values) { return substituteIn(e, values); }

std::string dump(const ExprPtr& e)
{
    std::string out;
    dumpInto(e, out);
    return out;
}
}
