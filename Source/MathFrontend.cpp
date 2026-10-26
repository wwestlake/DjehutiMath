#include "MathFrontend.h"

#include "MathUnits.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace math_frontend
{
using namespace math_ir;
using math_doc::Kind;
using math_doc::NodePtr;

namespace
{
// ---- notation constants (UTF-8) ---------------------------------------------------------------------------------------------
const char* const kMinus = "\xE2\x88\x92";
const char* const kPlusMinus = "\xC2\xB1";
const char* const kCdot = "\xE2\x8B\x85";
const char* const kMiddleDot = "\xC2\xB7";
const char* const kTimes = "\xC3\x97";
const char* const kDivide = "\xC3\xB7";
const char* const kInfinity = "\xE2\x88\x9E";
const char* const kPartial = "\xE2\x88\x82";
const char* const kDegree = "\xC2\xB0";
const char* const kPrime = "\xE2\x80\xB2";
const char* const kPi = "\xCF\x80";
const char* const kElementOf = "\xE2\x88\x88";
const char* const kSum = "\xE2\x88\x91";
const char* const kProduct = "\xE2\x88\x8F";
const char* const kIntegral = "\xE2\x88\xAB";
const char* const kAssign = "\xE2\x89\x94";

// ---- sign arithmetic -----------------------------------------------------------------------------------------------------------
struct Flags { bool p = false, n = false, z = false; };

Flags flagsOf(Sign s)
{
    switch (s)
    {
        case Sign::Zero: return { false, false, true };
        case Sign::Positive: return { true, false, false };
        case Sign::Nonnegative: return { true, false, true };
        case Sign::Negative: return { false, true, false };
        case Sign::Nonpositive: return { false, true, true };
        case Sign::Nonzero: return { true, true, false };
        default: return { true, true, true };
    }
}

Sign signOf(Flags f)
{
    if (f.p && f.n && f.z) return Sign::Unknown;
    if (f.p && f.n) return Sign::Nonzero;
    if (f.p && f.z) return Sign::Nonnegative;
    if (f.n && f.z) return Sign::Nonpositive;
    if (f.p) return Sign::Positive;
    if (f.n) return Sign::Negative;
    if (f.z) return Sign::Zero;
    return Sign::Unknown;
}

Sign signNeg(Sign s) { auto f = flagsOf(s); std::swap(f.p, f.n); return signOf(f); }

Sign signAdd(Sign a, Sign b)
{
    const auto x = flagsOf(a), y = flagsOf(b);
    Flags r;
    r.n = x.n || y.n;
    r.p = x.p || y.p;
    r.z = (x.z && y.z) || (x.p && y.n) || (x.n && y.p);
    return signOf(r);
}

Sign signMul(Sign a, Sign b)
{
    const auto x = flagsOf(a), y = flagsOf(b);
    Flags r;
    r.p = (x.p && y.p) || (x.n && y.n);
    r.n = (x.p && y.n) || (x.n && y.p);
    r.z = x.z || y.z;
    return signOf(r);
}

bool nonNegative(Sign s) { const auto f = flagsOf(s); return !f.n; }

Domain divDomain(Domain a, Domain b)
{
    const auto j = join(a, b);
    if (j == Domain::Unknown) return Domain::Unknown;
    return join(j, Domain::Rational);
}

Type scalarType(Domain d, const Dim& dim, Sign s = Sign::Unknown)
{
    Type t;
    t.shape = Shape::Scalar; t.domain = d; t.dim = dim; t.sign = s;
    return t;
}

Type holeType() { Type t; t.shape = Shape::Hole; return t; }
bool numericShape(const Type& t) { return t.shape == Shape::Scalar || t.shape == Shape::Vector || t.shape == Shape::Matrix; }

// ---- built-in functions ------------------------------------------------------------------------------------------------------------
enum Rule { Transcendental, Preserve, Floor, MinMax, SignRule, Det, Trace, Transpose, Norm, Dot, Cross, RealPart, ComplexConjugate, Argument };
struct Builtin { const char* name; int arity; Rule rule; };
const Builtin kBuiltins[] = {
    { "sin", 1, Transcendental }, { "cos", 1, Transcendental }, { "tan", 1, Transcendental }, { "cot", 1, Transcendental },
    { "sec", 1, Transcendental }, { "csc", 1, Transcendental }, { "asin", 1, Transcendental }, { "acos", 1, Transcendental },
    { "atan", 1, Transcendental }, { "arcsin", 1, Transcendental }, { "arccos", 1, Transcendental }, { "arctan", 1, Transcendental },
    { "sinh", 1, Transcendental }, { "cosh", 1, Transcendental }, { "tanh", 1, Transcendental }, { "coth", 1, Transcendental },
    { "exp", 1, Transcendental }, { "ln", 1, Transcendental }, { "log", 1, Transcendental },
    { "abs", 1, Preserve }, { "floor", 1, Floor }, { "ceil", 1, Floor }, { "min", -1, MinMax }, { "max", -1, MinMax },
    { "sign", 1, SignRule }, { "sgn", 1, SignRule }, { "det", 1, Det }, { "trace", 1, Trace }, { "tr", 1, Trace },
    { "transpose", 1, Transpose }, { "norm", 1, Norm }, { "dot", 2, Dot }, { "cross", 2, Cross },
    { "re", 1, RealPart }, { "im", 1, RealPart }, { "Re", 1, RealPart }, { "Im", 1, RealPart }, { "conj", 1, ComplexConjugate }, { "arg", 1, Argument },
};
const char* const kUnsupportedFunctions[] = { "lim", "sup", "inf_", "dim", "ker", "gcd", "mod", "rank", "deg", "lg" };

const Builtin* findBuiltin(const std::string& name)
{
    for (const auto& b : kBuiltins) if (name == b.name) return &b;
    return nullptr;
}

bool isUnsupportedFunction(const std::string& name)
{
    for (const auto* f : kUnsupportedFunctions) if (name == f) return true;
    return false;
}

// ---- row segments ------------------------------------------------------------------------------------------------------------------
struct Item { NodePtr node; std::uint32_t index = 0; };
struct Seg
{
    math_doc::NodeId rowId = 0;
    std::vector<Item> items;
    size_t size() const { return items.size(); }
};

Seg segOf(const NodePtr& row)
{
    Seg s;
    if (row == nullptr) return s;
    s.rowId = row->id;
    for (size_t i = 0; i < row->items.size(); ++i) s.items.push_back({ row->items[i], (std::uint32_t) i });
    return s;
}

bool isText(const Item& it, Kind kind, const char* text) { return it.node != nullptr && it.node->kind == kind && it.node->text == text; }
bool isOp(const Item& it, const char* text) { return isText(it, Kind::Operator, text); }
bool isMulOp(const Item& it)
{
    return isOp(it, kCdot) || isOp(it, kMiddleDot) || isOp(it, kTimes) || isOp(it, "*") || isOp(it, "\xE2\x88\x99");
}
bool isAddOp(const Item& it) { return isOp(it, "+") || isOp(it, kMinus) || isOp(it, "-") || isOp(it, kPlusMinus); }
bool isMinus(const Item& it) { return isOp(it, kMinus) || isOp(it, "-"); }

size_t utf8Length(const std::string& s, size_t pos)
{
    const unsigned char c = (unsigned char) s[pos];
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

// ---- the translator ---------------------------------------------------------------------------------------------------------------------
class Translator
{
public:
    Translator(const math_doc::MathSnapshot& s, Resolver* r, const Options& o) : snap(s), resolver(r), options(o), cell(s.cellId)
    {
        out = std::make_shared<CellAnalysis>();
        out->cell = s.cellId;
        out->revision = s.cellRevision;
        out->role = s.role;
    }

    std::shared_ptr<CellAnalysis> run()
    {
        std::vector<ExprPtr> lines;
        for (const auto& line : snap.lines)
        {
            if (line == nullptr) { lines.push_back(nullptr); continue; }
            Seg seg = segOf(line);
            removeAlignment(seg);
            ExprPtr e;
            switch (snap.role)
            {
                case math_doc::Role::Definition: e = definitionLine(seg); break;
                case math_doc::Role::Assumption: e = assumptionLine(seg); break;
                case math_doc::Role::Equation: case math_doc::Role::Constraint: case math_doc::Role::System:
                    e = relationLevel(seg, 0, seg.size());
                    if (e != nullptr && e->type.shape != Shape::Relation && e->type.shape != Shape::Hole)
                        error("relation-expected", std::string(snap.role == math_doc::Role::System ? "A line of a system" : "An equation or constraint")
                              + " needs a relation such as = < > <= >=.", range(seg, 0, seg.size()));
                    break;
                default: e = relationLevel(seg, 0, seg.size()); break;
            }
            lines.push_back(e);
        }
        out->lines = lines;
        std::vector<ExprPtr> present;
        for (const auto& l : lines) if (l != nullptr) present.push_back(l);
        if (snap.role == math_doc::Role::System || (snap.role == math_doc::Role::Assumption && present.size() > 1))
        {
            Type t; t.shape = Shape::System;
            if (!present.empty()) out->ir = makeNode(Op::System, present, t, Source { cell, snap.lines.empty() ? 0 : snap.lines[0]->id, 0, 0 });
        }
        else if (!present.empty())
            out->ir = present[0];
        if (out->ir != nullptr) out->type = out->ir->type;
        out->complete = !out->hasPlaceholder && !out->hasErrors();
        return out;
    }

private:
    const math_doc::MathSnapshot& snap;
    Resolver* resolver;
    Options options;
    std::string cell;
    std::shared_ptr<CellAnalysis> out;
    struct Bound { std::string name; Type type; };
    std::vector<Bound> bound;
    std::set<std::string> freeNoted;
    std::string selfName;       // the name this cell defines, while its body is analysed

    // ---- diagnostics --------------------------------------------------------------------------------------------------------------
    void report(Diagnostic::Severity severity, const std::string& code, const std::string& message, const Source& where)
    {
        for (const auto& d : out->diagnostics)
            if (d.code == code && d.message == message && d.where == where) return;
        out->diagnostics.push_back({ severity, code, message, where });
    }
    void error(const std::string& code, const std::string& message, const Source& where) { report(Diagnostic::Severity::Error, code, message, where); }
    void warn(const std::string& code, const std::string& message, const Source& where) { report(Diagnostic::Severity::Warning, code, message, where); }
    void info(const std::string& code, const std::string& message, const Source& where) { report(Diagnostic::Severity::Info, code, message, where); }

    Source range(const Seg& s, size_t b, size_t e) const
    {
        Source r;
        r.cell = cell;
        if (s.items.empty() || b >= e || b >= s.items.size()) { r.node = s.rowId; return r; }
        e = std::min(e, s.items.size());
        if (e - b == 1) { r.node = s.items[b].node->id; return r; }
        r.node = s.rowId;
        r.begin = s.items[b].index;
        r.end = s.items[e - 1].index + 1;
        return r;
    }
    Source at(const Item& it) const { Source r; r.cell = cell; r.node = it.node->id; return r; }
    Source at(const NodePtr& n) const { Source r; r.cell = cell; r.node = n != nullptr ? n->id : 0; return r; }

    ExprPtr hole(const std::string& code, const std::string& message, const Source& where)
    {
        error(code, message, where);
        return makeHole(code, where);
    }
    ExprPtr placeholder(const Source& where)
    {
        out->hasPlaceholder = true;
        error("placeholder", "This position is empty; complete the expression.", where);
        return makeHole("placeholder", where);
    }
    ExprPtr missing(const Seg& s, size_t b, size_t e)
    {
        const auto where = range(s, b, e);
        return hole("missing-operand", "An operand is missing here.", where);
    }

    void removeAlignment(Seg& s)
    {
        std::vector<Item> kept;
        for (const auto& it : s.items)
        {
            if (it.node->kind == Kind::Symbol && it.node->text == "&")
            {
                if (snap.role != math_doc::Role::System) warn("alignment-ignored", "An alignment mark & only has a meaning in a system of equations.", at(it));
                continue;
            }
            kept.push_back(it);
        }
        s.items = std::move(kept);
    }

    // ---- name resolution ---------------------------------------------------------------------------------------------------------------
    void use(const std::string& name, const Source& where)
    {
        out->uses.insert(name);
        if (out->useSites.count(name) == 0) out->useSites[name] = where;
    }
    bool isBound(const std::string& name) const
    {
        for (const auto& b : bound) if (b.name == name) return true;
        return false;
    }
    bool lookupDefinition(const std::string& name, Definition& def)
    {
        return resolver != nullptr && resolver->definition(name, cell, def);
    }
    bool isFunctionName(const std::string& name)
    {
        if (isBound(name)) return false;
        Definition def;
        if (lookupDefinition(name, def)) return def.isFunction;
        return findBuiltin(name) != nullptr;
    }
    Type freeType(const std::string& name, bool& declared)
    {
        Type t = scalarType(options.defaultDomain, Dim::unknown());
        Declaration d;
        declared = resolver != nullptr && resolver->declaration(name, d);
        if (declared)
        {
            if (d.hasDomain) t.domain = d.domain;
            if (d.hasSign) t.sign = d.sign;
            if (d.hasDim) t.dim = d.dim;
        }
        return t;
    }
    ExprPtr constant(const std::string& name, const Source& where)
    {
        if (name == "pi") return makeConstant("pi", Scalar::real(3.14159265358979323846, "\xCF\x80"), scalarType(Domain::Real, Dim::dimensionless(), Sign::Positive), where);
        if (name == "e") return makeConstant("e", Scalar::real(2.71828182845904523536, "e"), scalarType(Domain::Real, Dim::dimensionless(), Sign::Positive), where);
        return makeConstant("i", Scalar::complexNumber(0, 1), scalarType(Domain::Complex, Dim::dimensionless(), Sign::Nonzero), where);
    }
    ExprPtr symbol(const std::string& name, const Source& where)
    {
        for (auto it = bound.rbegin(); it != bound.rend(); ++it)
            if (it->name == name) return makeSymbol(name, it->type, where);
        Definition def;
        if (lookupDefinition(name, def))
        {
            use(name, where);
            Type t = def.isFunction ? Type { Shape::Function, Domain::Unknown, Dim::unknown(), Sign::Unknown, 0, 0 } : def.type;
            return makeSymbol(name, t, where);
        }
        if (name == selfName)
        {
            use(name, where);
            return makeSymbol(name, scalarType(Domain::Unknown, Dim::unknown()), where);
        }
        bool declared = false;
        Type t = freeType(name, declared);
        use(name, where);
        if (!declared && (name == kPi || name == "e" || name == "i"))
            return constant(name == kPi ? "pi" : name, where);
        if (!declared && freeNoted.insert(name).second)
            info("free-symbol", "'" + name + "' is not defined; it is treated as a free " + domainName(options.defaultDomain) + " variable.", where);
        return makeSymbol(name, t, where);
    }

    // ---- typed constructors --------------------------------------------------------------------------------------------------------------
    static bool anyHole(const std::vector<ExprPtr>& v)
    {
        for (const auto& e : v) if (e != nullptr && e->type.shape == Shape::Hole) return true;
        return false;
    }
    static bool exactNumber(const ExprPtr& e, Rational& q)
    {
        if (e == nullptr) return false;
        if (e->op == Op::Number && e->value.isExact()) { q = e->value.q; return true; }
        if (e->op == Op::Neg && e->args.size() == 1 && exactNumber(e->args[0], q)) { q = Rational(-q.num, q.den); return true; }
        if (e->op == Op::Div && e->args.size() == 2)
        {
            Rational a, b, r;
            if (exactNumber(e->args[0], a) && exactNumber(e->args[1], b) && div(a, b, r)) { q = r; return true; }
        }
        return false;
    }

    ExprPtr mkAdd(std::vector<ExprPtr> terms, const Source& where)
    {
        if (anyHole(terms)) return makeNode(Op::Add, std::move(terms), holeType(), where);
        Type r;
        bool numeric = true;
        for (const auto& t : terms) numeric = numeric && numericShape(t->type);
        if (!numeric) return makeNode(Op::Add, std::move(terms), Type {}, where);
        r = terms[0]->type;
        r.domain = Domain::Unknown;
        bool allKnownDims = true;
        Dim firstDim;
        bool haveDim = false, reported = false;
        for (const auto& t : terms)
        {
            if (t->type.shape != r.shape || (t->type.rows != 0 && r.rows != 0 && t->type.rows != r.rows) || (t->type.cols != 0 && r.cols != 0 && t->type.cols != r.cols))
            {
                error("shape-mismatch", std::string("Cannot add a ") + shapeName(r.shape) + " and a " + shapeName(t->type.shape) + " of a different size.", where);
                return makeNode(Op::Add, std::move(terms), Type {}, where);
            }
            if (!t->type.dim.known) { allKnownDims = false; continue; }
            if (!haveDim) { firstDim = t->type.dim; haveDim = true; }
            else if (t->type.dim != firstDim && !reported)
            {
                error("dimension-mismatch", "Cannot add quantities of different dimension: " + firstDim.toString() + " and " + t->type.dim.toString() + ".", where);
                reported = true;
            }
        }
        r.dim = (allKnownDims && haveDim) ? firstDim : Dim::unknown();
        Sign s = terms[0]->type.sign;
        for (const auto& t : terms) r.domain = join(r.domain, t->type.domain);
        for (size_t i = 1; i < terms.size(); ++i) s = signAdd(s, terms[i]->type.sign);
        r.sign = s;
        return makeNode(Op::Add, std::move(terms), r, where);
    }

    Type mulType(const Type& a, const Type& b, const Source& where)
    {
        if (a.shape == Shape::Hole || b.shape == Shape::Hole) return holeType();
        if (!numericShape(a) || !numericShape(b)) return Type {};
        Type r;
        r.domain = join(a.domain, b.domain);
        r.dim = a.dim * b.dim;
        r.sign = signMul(a.sign, b.sign);
        if (a.shape == Shape::Scalar && b.shape == Shape::Scalar) { r.shape = Shape::Scalar; return r; }
        if (a.shape == Shape::Scalar || b.shape == Shape::Scalar)
        {
            const auto& other = a.shape == Shape::Scalar ? b : a;
            r.shape = other.shape; r.rows = other.rows; r.cols = other.cols;
            r.sign = Sign::Unknown;
            return r;
        }
        r.sign = Sign::Unknown;
        if (a.shape == Shape::Matrix && b.shape == Shape::Vector)
        {
            if (a.cols != 0 && b.rows != 0 && a.cols != b.rows) { error("shape-mismatch", "The matrix has " + std::to_string(a.cols) + " columns but the vector has " + std::to_string(b.rows) + " rows.", where); return Type {}; }
            r.shape = Shape::Vector; r.rows = a.rows;
            return r;
        }
        if (a.shape == Shape::Matrix && b.shape == Shape::Matrix)
        {
            if (a.cols != 0 && b.rows != 0 && a.cols != b.rows) { error("shape-mismatch", "The matrix has " + std::to_string(a.cols) + " columns but the other has " + std::to_string(b.rows) + " rows.", where); return Type {}; }
            r.shape = Shape::Matrix; r.rows = a.rows; r.cols = b.cols;
            return r;
        }
        error("vector-product", "The product of two vectors is ambiguous; dot and cross products are not defined yet.", where);
        return Type {};
    }

    ExprPtr mkMul(std::vector<ExprPtr> factors, const Source& where)
    {
        if (factors.size() == 1) return factors[0];
        Type t = factors[0]->type;
        for (size_t i = 1; i < factors.size(); ++i) t = mulType(t, factors[i]->type, where);
        return makeNode(Op::Mul, std::move(factors), t, where);
    }

    ExprPtr mkDiv(const ExprPtr& num, const ExprPtr& den, const Source& where)
    {
        if (anyHole({ num, den })) return makeNode(Op::Div, { num, den }, holeType(), where);
        Type t;
        if (numericShape(num->type) && numericShape(den->type))
        {
            if (den->type.shape != Shape::Scalar) error("shape-mismatch", "Only a scalar can divide.", where);
            else
            {
                t = num->type;
                t.domain = divDomain(num->type.domain, den->type.domain);
                t.dim = num->type.dim / den->type.dim;
                t.sign = t.shape == Shape::Scalar ? signMul(num->type.sign, den->type.sign) : Sign::Unknown;
            }
        }
        Rational z;
        if (exactNumber(den, z) && z.isZero()) error("div-zero", "Division by zero.", where);
        return makeNode(Op::Div, { num, den }, t, where);
    }

    ExprPtr mkNeg(const ExprPtr& x, const Source& where)
    {
        Type t = x->type;
        if (t.shape == Shape::Scalar) t.sign = signNeg(t.sign);
        else if (t.shape != Shape::Hole && !numericShape(t)) t = Type {};
        return makeNode(Op::Neg, { x }, t, where);
    }

    ExprPtr mkPow(const ExprPtr& base, const ExprPtr& exponent, const Source& where)
    {
        if (anyHole({ base, exponent })) return makeNode(Op::Pow, { base, exponent }, holeType(), where);
        Type t;
        if (!numericShape(base->type) || !numericShape(exponent->type)) return makeNode(Op::Pow, { base, exponent }, t, where);
        if (exponent->type.shape != Shape::Scalar) { error("shape-mismatch", "An exponent must be a scalar.", where); return makeNode(Op::Pow, { base, exponent }, t, where); }
        if (exponent->type.dim.known && !exponent->type.dim.isDimensionless())
            error("dimension-mismatch", "An exponent must be dimensionless, not " + exponent->type.dim.toString() + ".", where);
        Rational q;
        const bool exact = exactNumber(exponent, q);
        const bool integer = exact && q.isInteger();
        if (base->type.shape == Shape::Matrix)
        {
            if (!integer) error("shape-mismatch", "A matrix can only be raised to a whole-number power.", where);
            if (base->type.rows != 0 && base->type.cols != 0 && base->type.rows != base->type.cols) error("shape-mismatch", "Only a square matrix can be raised to a power.", where);
            t = base->type; t.sign = Sign::Unknown;
            return makeNode(Op::Pow, { base, exponent }, t, where);
        }
        if (base->type.shape == Shape::Vector) { error("shape-mismatch", "A vector cannot be raised to a power.", where); return makeNode(Op::Pow, { base, exponent }, t, where); }
        t.shape = Shape::Scalar;
        const auto& bt = base->type;
        if (!bt.dim.known) t.dim = Dim::unknown();
        else if (bt.dim.isDimensionless()) t.dim = Dim::dimensionless();
        else if (exact) t.dim = bt.dim.power(q);
        else { error("dimension-exponent", "A quantity with dimension " + bt.dim.toString() + " needs a constant exponent.", where); t.dim = Dim::unknown(); }
        const auto bf = flagsOf(bt.sign);
        if (bt.domain == Domain::Complex || exponent->type.domain == Domain::Complex) t.domain = Domain::Complex;
        else if (integer)
            t.domain = (q.num >= 0 && bt.domain == Domain::Integer) ? Domain::Integer : (bt.domain == Domain::Integer || bt.domain == Domain::Rational ? Domain::Rational : join(bt.domain, Domain::Real));
        else t.domain = !bf.n ? Domain::Real : Domain::Complex;
        if (bt.domain == Domain::Unknown) t.domain = Domain::Unknown;
        if (integer)
        {
            if (q.num % 2 == 0) { Flags f; f.p = bf.p || bf.n; f.z = bf.z; t.sign = q.num == 0 ? Sign::Positive : signOf(f); }
            else t.sign = bt.sign;
            if (q.num < 0) { Flags f = flagsOf(t.sign); f.z = false; t.sign = signOf(f); }
        }
        else if (!bf.n) { Flags f; f.p = bf.p; f.z = bf.z; t.sign = signOf(f); }
        return makeNode(Op::Pow, { base, exponent }, t, where);
    }

    ExprPtr mkRoot(const ExprPtr& x, const ExprPtr& index, const Source& where)
    {
        if (anyHole({ x, index })) return makeNode(Op::Root, { x, index }, holeType(), where);
        Type t;
        if (!numericShape(x->type) || x->type.shape != Shape::Scalar)
        {
            if (numericShape(x->type)) error("shape-mismatch", "A root needs a scalar.", where);
            return makeNode(Op::Root, { x, index }, t, where);
        }
        Rational k;
        const bool known = exactNumber(index, k) && k.isInteger() && k.num >= 2;
        if (!known && exactNumber(index, k) == false) error("unsupported-root", "The index of a root must be a whole number of at least 2.", where);
        t.shape = Shape::Scalar;
        t.dim = !x->type.dim.known ? Dim::unknown() : (x->type.dim.isDimensionless() ? Dim::dimensionless() : (known ? x->type.dim.power(Rational(1, k.num)) : Dim::unknown()));
        const bool odd = known && k.num % 2 == 1;
        if (x->type.domain == Domain::Complex) t.domain = Domain::Complex;
        else if (x->type.domain == Domain::Unknown) t.domain = Domain::Unknown;
        else t.domain = (!flagsOf(x->type.sign).n || odd) ? Domain::Real : Domain::Complex;
        if (odd) t.sign = x->type.sign;
        else { const auto f = flagsOf(x->type.sign); Flags r; r.p = f.p; r.z = f.z; t.sign = f.n ? Sign::Unknown : signOf(r); }
        return makeNode(Op::Root, { x, index }, t, where);
    }

    ExprPtr mkRelation(Op op, std::vector<ExprPtr> sides, const Source& where)
    {
        Type t; t.shape = Shape::Relation;
        if (anyHole(sides)) return makeNode(op, std::move(sides), t, where);
        for (size_t i = 1; i < sides.size(); ++i)
        {
            const auto& a = sides[i - 1]->type;
            const auto& b = sides[i]->type;
            if (!numericShape(a) || !numericShape(b)) continue;
            if (a.shape != b.shape || (a.rows != 0 && b.rows != 0 && a.rows != b.rows) || (a.cols != 0 && b.cols != 0 && a.cols != b.cols))
            { error("shape-mismatch", std::string("Both sides of the relation must have the same shape, not ") + shapeName(a.shape) + " and " + shapeName(b.shape) + ".", where); continue; }
            if (a.dim.known && b.dim.known && a.dim != b.dim)
                error("dimension-mismatch", "Both sides of the relation must have the same dimension: " + a.dim.toString() + " and " + b.dim.toString() + ".", where);
            if ((op == Op::Lt || op == Op::Le || op == Op::Gt || op == Op::Ge) && (a.domain == Domain::Complex || b.domain == Domain::Complex))
                error("ordering-complex", "Complex numbers are not ordered.", where);
        }
        return makeNode(op, std::move(sides), t, where);
    }

    ExprPtr mkBuiltinCall(const Builtin& fn, std::vector<ExprPtr> args, const Source& where)
    {
        if (anyHole(args)) return makeNode(Op::Call, std::move(args), holeType(), where, fn.name);
        if (fn.arity >= 0 && (int) args.size() != fn.arity)
        {
            error("arity", std::string(fn.name) + " takes " + std::to_string(fn.arity) + " argument" + (fn.arity == 1 ? "" : "s") + ", not " + std::to_string(args.size()) + ".", where);
            return makeNode(Op::Call, std::move(args), Type {}, where, fn.name);
        }
        Type t;
        const auto& a = args[0]->type;
        const bool scalarRule = fn.rule == Transcendental || fn.rule == Preserve || fn.rule == Floor || fn.rule == SignRule
                                || fn.rule == RealPart || fn.rule == ComplexConjugate || fn.rule == Argument;
        if (scalarRule && numericShape(a) && a.shape != Shape::Scalar)
        {
            error("shape-mismatch", std::string(fn.name) + " takes a scalar, not a " + shapeName(a.shape) + ".", where);
            return makeNode(Op::Call, std::move(args), Type {}, where, fn.name);
        }
        auto dimensionlessArgument = [&] {
            if (a.dim.known && !a.dim.isDimensionless())
                error("dimension-mismatch", std::string(fn.name) + " needs a dimensionless argument, not " + a.dim.toString() + ".", where);
        };
        switch (fn.rule)
        {
            case Transcendental:
                dimensionlessArgument();
                t = scalarType(a.domain == Domain::Complex ? Domain::Complex : Domain::Real, Dim::dimensionless());
                if (std::string(fn.name) == "exp" && t.domain == Domain::Real) t.sign = Sign::Positive;
                break;
            case Preserve:
                t = a; t.domain = a.domain == Domain::Unknown ? Domain::Unknown : (a.domain == Domain::Integer || a.domain == Domain::Rational ? a.domain : Domain::Real);
                t.sign = Sign::Nonnegative;
                break;
            case Floor: t = scalarType(Domain::Integer, a.dim); break;
            case SignRule: t = scalarType(Domain::Integer, Dim::dimensionless()); break;
            case MinMax:
            {
                t = args[0]->type; t.sign = Sign::Unknown;
                for (const auto& x : args)
                {
                    if (x->type.dim.known && t.dim.known && x->type.dim != t.dim)
                        error("dimension-mismatch", std::string(fn.name) + " compares quantities of different dimension.", where);
                    t.domain = join(t.domain, x->type.domain);
                }
                break;
            }
            case Det:
                if (a.shape != Shape::Matrix) { if (numericShape(a)) error("shape-mismatch", "det needs a matrix.", where); break; }
                if (a.rows != 0 && a.cols != 0 && a.rows != a.cols) error("shape-mismatch", "det needs a square matrix.", where);
                t = scalarType(a.domain, a.dim.known && a.rows > 0 ? a.dim.power(Rational(a.rows)) : Dim::unknown());
                break;
            case Trace:
                if (a.shape != Shape::Matrix) { if (numericShape(a)) error("shape-mismatch", "trace needs a matrix.", where); break; }
                if (a.rows != 0 && a.cols != 0 && a.rows != a.cols) error("shape-mismatch", "trace needs a square matrix.", where);
                t = scalarType(a.domain, a.dim);
                break;
            case Transpose:
                if (a.shape == Shape::Matrix) { t = a; std::swap(t.rows, t.cols); t.sign = Sign::Unknown; }
                else if (numericShape(a)) error("shape-mismatch", "transpose needs a matrix.", where);
                break;
            case RealPart:
                t = scalarType(a.domain == Domain::Unknown ? Domain::Unknown : (a.domain == Domain::Complex ? Domain::Real : a.domain), a.dim);
                if (std::string(fn.name) == "im" || std::string(fn.name) == "Im") t.domain = a.domain == Domain::Unknown ? Domain::Unknown : Domain::Real;
                break;
            case ComplexConjugate: t = a; t.sign = Sign::Unknown; if (a.domain != Domain::Complex) t = a; break;
            case Argument: t = scalarType(Domain::Real, Dim::dimensionless()); break;
            case Dot: case Cross:
            {
                const auto& b = args[1]->type;
                if (a.shape != Shape::Vector || b.shape != Shape::Vector || (a.rows != 0 && b.rows != 0 && a.rows != b.rows))
                { if (numericShape(a) && numericShape(b)) error("shape-mismatch", std::string(fn.name) + " needs two vectors of the same length.", where); break; }
                if (fn.rule == Cross && a.rows != 0 && a.rows != 3) { error("shape-mismatch", "cross needs vectors of length 3.", where); break; }
                t = fn.rule == Dot ? scalarType(join(a.domain, b.domain), a.dim * b.dim) : Type {};
                if (fn.rule == Cross) { t.shape = Shape::Vector; t.rows = 3; t.domain = join(a.domain, b.domain); t.dim = a.dim * b.dim; }
                break;
            }
            case Norm:
                if (a.shape == Shape::Vector || a.shape == Shape::Matrix) t = scalarType(a.domain == Domain::Complex ? Domain::Real : join(a.domain, Domain::Real), a.dim, Sign::Nonnegative);
                else if (numericShape(a)) error("shape-mismatch", "norm needs a vector or a matrix.", where);
                break;
        }
        if (fn.rule == MinMax && args.size() < 2) error("arity", std::string(fn.name) + " needs at least two arguments.", where);
        return makeNode(Op::Call, std::move(args), t, where, fn.name);
    }

    ExprPtr mkUserCall(const Definition& def, std::vector<ExprPtr> args, const Source& where)
    {
        if (anyHole(args)) return makeNode(Op::Call, std::move(args), holeType(), where, def.name);
        if (args.size() != def.params.size())
        {
            error("arity", def.name + " takes " + std::to_string(def.params.size()) + " argument" + (def.params.size() == 1 ? "" : "s") + ", not " + std::to_string(args.size()) + ".", where);
            return makeNode(Op::Call, std::move(args), Type {}, where, def.name);
        }
        use(def.name, where);
        return makeNode(Op::Call, std::move(args), def.type, where, def.name);
    }

    // ---- structure ------------------------------------------------------------------------------------------------------------------------
    // The differential that ends an integral: dx, dt (one letter after d), or `d` followed by a name.
    bool findDifferential(const Seg& s, size_t from, size_t to, size_t& at, size_t& after, std::string& variable) const
    {
        for (size_t i = from; i < to; ++i)
        {
            const auto& n = *s.items[i].node;
            if (n.kind != Kind::Identifier) continue;
            if (n.text == "d" && i + 1 < to && s.items[i + 1].node->kind == Kind::Identifier)
            {
                at = i; after = i + 2; variable = s.items[i + 1].node->text;
                return true;
            }
            if (n.text.size() > 1 && n.text[0] == 'd' && utf8Length(n.text, 1) == n.text.size() - 1)
            {
                at = i; after = i + 1; variable = n.text.substr(1);
                return true;
            }
        }
        return false;
    }

    size_t skipIntegral(const Seg& s, size_t i, size_t e) const
    {
        if (i < e && s.items[i].node->kind == Kind::BigOperator && s.items[i].node->text == kIntegral)
        {
            size_t at = 0, after = 0; std::string variable;
            if (findDifferential(s, i + 1, e, at, after, variable)) return after;
        }
        return i + 1;
    }

    std::vector<std::pair<size_t, size_t>> splitTop(const Seg& s, size_t b, size_t e, const char* separator) const
    {
        std::vector<std::pair<size_t, size_t>> parts;
        size_t start = b;
        for (size_t i = b; i < e; ++i)
            if (isOp(s.items[i], separator)) { parts.push_back({ start, i }); start = i + 1; }
        parts.push_back({ start, e });
        return parts;
    }

    // ---- relation, sum, product -----------------------------------------------------------------------------------------------------------
    static bool relationOp(const std::string& text, Op& op)
    {
        if (text == "=") op = Op::Eq;
        else if (text == "\xE2\x89\xA0") op = Op::Ne;
        else if (text == "<") op = Op::Lt;
        else if (text == "\xE2\x89\xA4") op = Op::Le;
        else if (text == ">") op = Op::Gt;
        else if (text == "\xE2\x89\xA5") op = Op::Ge;
        else if (text == "\xE2\x89\x88") op = Op::Approx;
        else return false;
        return true;
    }

    ExprPtr relationLevel(const Seg& s, size_t b, size_t e)
    {
        if (b >= e) return missing(s, b, e);
        std::vector<size_t> rel;
        for (size_t i = b; i < e;)
        {
            const auto& it = s.items[i];
            if (it.node->kind == Kind::Relation) { rel.push_back(i); ++i; continue; }
            i = skipIntegral(s, i, e);
        }
        if (rel.empty()) return additive(s, b, e);
        std::vector<ExprPtr> sides;
        std::vector<Op> ops;
        size_t start = b;
        for (size_t k = 0; k <= rel.size(); ++k)
        {
            const size_t end = k < rel.size() ? rel[k] : e;
            sides.push_back(start >= end ? missing(s, start, end) : additive(s, start, end));
            if (k < rel.size())
            {
                Op op = Op::Eq;
                if (s.items[rel[k]].node->text == kAssign)
                    return hole("definition-role", "A definition (:=) belongs in a Definition cell.", at(s.items[rel[k]]));
                if (!relationOp(s.items[rel[k]].node->text, op))
                    return hole("unsupported-relation", "The relation '" + s.items[rel[k]].node->text + "' is not supported by the analysis yet.", at(s.items[rel[k]]));
                ops.push_back(op);
            }
            start = k < rel.size() ? rel[k] + 1 : e;
        }
        const auto where = range(s, b, e);
        for (size_t k = 1; k < ops.size(); ++k)
            if (ops[k] != ops[0]) return hole("chained-relation", "A chain of relations must use the same relation throughout.", where);
        if (ops.size() > 1 && (ops[0] == Op::Ne || ops[0] == Op::Approx)) return hole("chained-relation", "This relation cannot be chained.", where);
        return mkRelation(ops[0], std::move(sides), where);
    }

    ExprPtr additive(const Seg& s, size_t b, size_t e)
    {
        if (b >= e) return missing(s, b, e);
        std::vector<size_t> splits;
        for (size_t i = b; i < e;)
        {
            const auto& it = s.items[i];
            if (isAddOp(it) && i > b)
            {
                const auto& prev = *s.items[i - 1].node;
                if (prev.kind != Kind::Operator && prev.kind != Kind::Relation) splits.push_back(i);
            }
            i = skipIntegral(s, i, e);
        }
        std::vector<ExprPtr> terms;
        size_t start = b;
        for (size_t k = 0; k <= splits.size(); ++k)
        {
            const size_t end = k < splits.size() ? splits[k] : e;
            bool negative = false;
            size_t ts = start;
            if (k > 0) { const auto& op = s.items[start - 1]; negative = isMinus(op); if (isOp(op, kPlusMinus)) return hole("unsupported-plusminus", "The sign \xC2\xB1 gives two results and is not supported by the analysis yet.", at(op)); }
            else if (ts < end && isAddOp(s.items[ts]))
            {
                if (isOp(s.items[ts], kPlusMinus)) return hole("unsupported-plusminus", "The sign \xC2\xB1 gives two results and is not supported by the analysis yet.", at(s.items[ts]));
                negative = isMinus(s.items[ts]);
                ++ts;
            }
            auto term = ts >= end ? missing(s, start, end) : multiplicative(s, ts, end);
            terms.push_back(negative ? mkNeg(term, range(s, start, end)) : term);
            start = k < splits.size() ? splits[k] + 1 : e;
        }
        if (terms.size() == 1) return terms[0];
        return mkAdd(std::move(terms), range(s, b, e));
    }

    ExprPtr multiplicative(const Seg& s, size_t b, size_t e)
    {
        std::vector<ExprPtr> product;
        ExprPtr acc;                 // a completed quotient so far
        char pending = '*';
        size_t i = b;
        bool needOperand = false;
        size_t groupStart = b;
        auto flush = [&]() -> ExprPtr {
            if (product.empty()) return acc;
            auto p = mkMul(product, range(s, groupStart, i));
            product.clear();
            return p;
        };
        while (i < e)
        {
            const auto& it = s.items[i];
            if (it.node->kind == Kind::Operator && (isMulOp(it) || isOp(it, kDivide)))
            {
                if (product.empty() && acc == nullptr) { product.push_back(missing(s, i, i + 1)); }
                pending = isOp(it, kDivide) ? '/' : '*';
                needOperand = true;
                ++i;
                continue;
            }
            size_t next = i;
            ExprPtr f = factor(s, i, e, next);
            needOperand = false;
            if (pending == '/')
            {
                auto numerator = flush();
                acc = mkDiv(numerator, f, range(s, groupStart, next));
                product.clear();
                pending = '*';
            }
            else
            {
                if (acc != nullptr && product.empty()) product.push_back(acc), acc = nullptr;
                product.push_back(f);
            }
            i = next;
        }
        if (needOperand)
        {
            if (acc != nullptr && product.empty()) { product.push_back(acc); acc = nullptr; }
            product.push_back(missing(s, e - 1, e));
        }
        if (acc != nullptr && product.empty()) return acc;
        if (product.empty()) return missing(s, b, e);
        return mkMul(product, range(s, b, e));
    }

    // One factor starting at items[i]; `next` is the first item after it.
    ExprPtr factor(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& n = *it.node;
        next = i + 1;
        ExprPtr result;
        switch (n.kind)
        {
            case Kind::Operator:
                if (isAddOp(it) && i + 1 < e)
                {
                    size_t after = i + 1;
                    auto operand = factor(s, i + 1, e, after);
                    next = after;
                    if (isOp(it, kPlusMinus)) return hole("unsupported-plusminus", "The sign \xC2\xB1 gives two results and is not supported by the analysis yet.", at(it));
                    return isMinus(it) ? mkNeg(operand, range(s, i, after)) : operand;
                }
                return hole("unexpected-operator", "The operator '" + n.text + "' has no operand here.", at(it));
            case Kind::Relation:
                return hole("unexpected-relation", "A relation is not allowed here.", at(it));
            case Kind::Placeholder:
                return placeholder(at(it));
            case Kind::Number:
                result = number(it, s, i, e, next);
                break;
            case Kind::Identifier:
                result = identifier(s, i, e, next);
                break;
            case Kind::Symbol:
                if (n.text == kInfinity) { result = makeConstant("infty", Scalar::real(INFINITY, "\xE2\x88\x9E"), scalarType(Domain::Real, Dim::unknown(), Sign::Positive), at(it)); break; }
                return hole("unsupported-symbol", "The symbol '" + n.text + "' is not supported in an expression yet.", at(it));
            case Kind::Text:
                return hole("unexpected-text", "Text is only meaningful as a unit after a number.", at(it));
            case Kind::Fraction: result = fraction(it); break;
            case Kind::Radical: result = radical(it); break;
            case Kind::Script: result = script(s, i, e, next); break;
            case Kind::Delimited: result = delimited(it); break;
            case Kind::BigOperator: result = bigOperator(s, i, e, next); break;
            default:
                return hole("unsupported-structure", "This structure is not supported by the analysis.", at(it));
        }
        return postfix(result, s, next, e, next);
    }

    ExprPtr postfix(ExprPtr f, const Seg& s, size_t from, size_t e, size_t& next)
    {
        next = from;
        while (next < e && f != nullptr)
        {
            const auto& it = s.items[next];
            if (isText(it, Kind::Symbol, kDegree))
            {
                auto u = math_units::parse(kDegree, at(it));
                f = mkMul({ f, u.expr }, at(it));
                ++next;
            }
            else if (isText(it, Kind::Symbol, "%"))
            {
                Type hundred = scalarType(Domain::Integer, Dim::dimensionless(), Sign::Positive);
                f = mkDiv(f, makeNumber(Scalar::integer(100), hundred, at(it)), at(it));
                ++next;
            }
            else if (isText(it, Kind::Symbol, "!"))
            {
                error("unsupported-factorial", "The factorial is not supported by the analysis yet.", at(it));
                ++next;
            }
            else break;
        }
        return f;
    }

    ExprPtr number(const Item& it, const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& text = it.node->text;
        ExprPtr value;
        if (text.find('.') != std::string::npos)
        {
            const double v = std::strtod(text.c_str(), nullptr);
            value = makeNumber(Scalar::real(v, text), scalarType(Domain::Real, Dim::dimensionless(), v == 0.0 ? Sign::Zero : Sign::Positive), at(it));
        }
        else
        {
            bool overflow = text.size() > 18;
            std::int64_t v = 0;
            if (!overflow) v = std::strtoll(text.c_str(), nullptr, 10);
            if (overflow)
            {
                warn("integer-overflow", "The integer " + text + " is too large for exact arithmetic and is treated as an approximate real.", at(it));
                value = makeNumber(Scalar::real(std::strtod(text.c_str(), nullptr), text), scalarType(Domain::Real, Dim::dimensionless(), Sign::Positive), at(it));
            }
            else
                value = makeNumber(Scalar::integer(v), scalarType(Domain::Integer, Dim::dimensionless(), v == 0 ? Sign::Zero : Sign::Positive), at(it));
        }
        if (i + 1 < e && s.items[i + 1].node->kind == Kind::Text)
        {
            const auto& unitNode = *s.items[i + 1].node;
            const auto parsed = math_units::parse(unitNode.text, at(s.items[i + 1]));
            next = i + 2;
            if (!parsed.ok)
            {
                error("unknown-unit", parsed.error, at(s.items[i + 1]));
                return value;
            }
            Type t = scalarType(value->type.domain, parsed.dim, value->type.sign);
            return makeNode(Op::Mul, { value, parsed.expr }, t, range(s, i, i + 2));
        }
        return value;
    }

    // ---- names, functions, scripts ---------------------------------------------------------------------------------------------------------
    std::string nameOfScript(const math_doc::Node& n, bool& ok, bool allowSuperscript = false) const
    {
        ok = false;
        if (n.slots[0] == nullptr || n.slots[0]->items.size() != 1 || n.slots[0]->items[0]->kind != Kind::Identifier
            || (n.slots[2] != nullptr && !allowSuperscript) || n.slots[1] == nullptr) return {};
        std::string name = n.slots[0]->items[0]->text + "_";
        for (const auto& sub : n.slots[1]->items)
        {
            if (sub->kind != Kind::Number && sub->kind != Kind::Identifier) return {};
            name += sub->text;
        }
        ok = true;
        return name;
    }

    ExprPtr identifier(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& name = it.node->text;
        const bool callable = i + 1 < e && s.items[i + 1].node->kind == Kind::Delimited && s.items[i + 1].node->text == "(";
        if (isFunctionName(name))
        {
            if (i + 1 < e && s.items[i + 1].node->kind == Kind::Script)
            {
                // sin(x)^2: the exponent was attached to the bracket; it belongs to the result of the call
                const auto& sc = *s.items[i + 1].node;
                if (sc.slots[0] != nullptr && sc.slots[0]->items.size() == 1 && sc.slots[0]->items[0]->kind == Kind::Delimited
                    && sc.slots[0]->items[0]->text == "(" && sc.slots[1] == nullptr && sc.slots[2] != nullptr)
                {
                    Item bracket { sc.slots[0]->items[0], 0 };
                    auto call = functionCall(name, bracket, range(s, i, i + 2));
                    next = i + 2;
                    return mkPow(call, scriptExponent(sc), range(s, i, i + 2));
                }
            }
            if (callable)
            {
                next = i + 2;
                return functionCall(name, s.items[i + 1], range(s, i, i + 2));
            }
            if (i + 1 < e)
            {
                size_t after = i + 1;
                auto arg = factor(s, i + 1, e, after);
                next = after;
                return finishCall(name, { arg }, range(s, i, after));
            }
            return hole("missing-argument", "The function " + name + " needs an argument.", at(it));
        }
        if (isUnsupportedFunction(name) && !isBound(name))
            return hole("unsupported-function", "The function '" + name + "' is not supported by the analysis yet.", at(it));
        return symbol(name, at(it));
    }

    ExprPtr finishCall(const std::string& name, std::vector<ExprPtr> args, const Source& where)
    {
        Definition def;
        if (!isBound(name) && lookupDefinition(name, def) && def.isFunction) return mkUserCall(def, std::move(args), where);
        if (const auto* fn = findBuiltin(name)) return mkBuiltinCall(*fn, std::move(args), where);
        return hole("not-a-function", "'" + name + "' is not a function.", where);
    }

    ExprPtr functionCall(const std::string& name, const Item& args, const Source& where)
    {
        const auto& delimited = *args.node;
        if (delimited.text2.empty()) warn("unclosed-bracket", "A bracket is not closed.", at(args));
        std::vector<ExprPtr> list;
        const Seg body = segOf(delimited.slots[0]);
        for (const auto& part : splitTop(body, 0, body.size(), ","))
            list.push_back(part.first >= part.second ? missing(body, part.first, part.second) : relationLevel(body, part.first, part.second));
        return finishCall(name, std::move(list), where);
    }

    ExprPtr script(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& n = *it.node;
        const auto& baseRow = n.slots[0];
        ExprPtr base;
        // a function with an exponent: sin^2(x), sin^2 x
        if (n.slots[2] != nullptr && n.slots[1] == nullptr && baseRow != nullptr && baseRow->items.size() == 1
            && baseRow->items[0]->kind == Kind::Identifier && isFunctionName(baseRow->items[0]->text))
        {
            const auto& name = baseRow->items[0]->text;
            if (i + 1 >= e) return hole("missing-argument", "The function " + name + " needs an argument.", at(it));
            size_t after = i + 1;
            ExprPtr call;
            if (s.items[i + 1].node->kind == Kind::Delimited && s.items[i + 1].node->text == "(")
            {
                call = functionCall(name, s.items[i + 1], range(s, i, i + 2));
                after = i + 2;
            }
            else
            {
                auto arg = factor(s, i + 1, e, after);
                call = finishCall(name, { arg }, range(s, i, after));
            }
            next = after;
            return mkPow(call, scriptExponent(n), range(s, i, after));
        }
        bool composite = false;
        const auto name = nameOfScript(n, composite, true);
        const bool identifierBase = baseRow != nullptr && baseRow->items.size() == 1 && baseRow->items[0]->kind == Kind::Identifier;
        bool asElement = false;
        if (composite && subscriptHasBound(n)) asElement = true;                       // x_i with i the index of a sum
        else if (composite)
        {
            // x_1: the symbol "x_1", unless only the vector or matrix x exists
            Definition named, container;
            Declaration declared;
            const bool nameKnown = lookupDefinition(name, named) || (resolver != nullptr && resolver->declaration(name, declared));
            asElement = !nameKnown && lookupDefinition(baseRow->items[0]->text, container) && !container.isFunction
                        && (container.type.shape == Shape::Vector || container.type.shape == Shape::Matrix);
        }
        else if (identifierBase && n.slots[1] != nullptr) asElement = true;            // x_(1,2)
        if (asElement)
        {
            auto container = symbol(baseRow->items[0]->text, at(baseRow->items[0]));
            std::vector<ExprPtr> args { container };
            const Seg sub = segOf(n.slots[1]);
            for (const auto& part : splitTop(sub, 0, sub.size(), ","))
                args.push_back(part.first >= part.second ? missing(sub, part.first, part.second) : additive(sub, part.first, part.second));
            Type element = scalarType(container->type.domain, container->type.dim);
            base = makeNode(Op::Index, std::move(args), element, at(it));
        }
        else if (composite)
            base = symbol(name, at(it));
        else
        {
            base = rowExpr(baseRow);
            if (n.slots[1] != nullptr) return hole("unsupported-subscript", "A subscript on an expression is only supported on a name.", at(it));
        }
        if (n.slots[2] == nullptr) return base;
        return mkPow(base, scriptExponent(n), at(it));
    }

    bool subscriptHasBound(const math_doc::Node& n) const
    {
        if (n.slots[1] == nullptr) return false;
        for (const auto& sub : n.slots[1]->items)
            if (sub->kind == Kind::Identifier && isBound(sub->text)) return true;
        return false;
    }

    ExprPtr scriptExponent(const math_doc::Node& n)
    {
        const auto& sup = n.slots[2];
        if (sup != nullptr && sup->items.size() == 1 && isText(Item { sup->items[0], 0 }, Kind::Symbol, kPrime))
            return hole("unsupported-prime", "Prime notation for a derivative is not supported by the analysis yet; use d/dx.", at(sup->items[0]));
        return rowExpr(sup);
    }

    ExprPtr rowExpr(const NodePtr& row)
    {
        if (row == nullptr) return hole("missing-operand", "An operand is missing here.", Source { cell, 0, 0, 0 });
        const Seg s = segOf(row);
        return relationLevel(s, 0, s.size());
    }

    // ---- structures ----------------------------------------------------------------------------------------------------------------------------
    bool differentialName(const NodePtr& row, std::string& variable, bool& partial) const
    {
        if (row == nullptr) return false;
        partial = false;
        const auto& v = row->items;
        if (v.size() == 1 && v[0]->kind == Kind::Identifier && v[0]->text.size() > 1 && v[0]->text[0] == 'd' && utf8Length(v[0]->text, 1) == v[0]->text.size() - 1)
        { variable = v[0]->text.substr(1); return true; }
        if (v.size() == 2 && v[0]->kind == Kind::Identifier && v[0]->text == "d" && v[1]->kind == Kind::Identifier)
        { variable = v[1]->text; return true; }
        if (v.size() == 2 && v[0]->kind == Kind::Symbol && v[0]->text == kPartial && v[1]->kind == Kind::Identifier)
        { variable = v[1]->text; partial = true; return true; }
        return false;
    }

    ExprPtr fraction(const Item& it)
    {
        const auto& n = *it.node;
        std::string function, variable;
        bool numPartial = false, denPartial = false;
        if (differentialName(n.slots[0], function, numPartial) && differentialName(n.slots[1], variable, denPartial) && numPartial == denPartial)
        {
            auto fullName = [](const NodePtr& row) { std::string t; for (const auto& i : row->items) t += i->text; return t; };
            const auto numName = fullName(n.slots[0]), denName = fullName(n.slots[1]);
            Definition def;
            Declaration decl;
            const bool plain = !numPartial && (isBound(numName) || isBound(denName) || lookupDefinition(numName, def) || lookupDefinition(denName, def));
            const bool declaredPlain = !numPartial && resolver != nullptr && (resolver->declaration(numName, decl) || resolver->declaration(denName, decl));
            if (!plain && !declaredPlain)
            {
                auto f = symbol(function, at(n.slots[0]));
                auto x = symbol(variable, at(n.slots[1]));
                Type t = scalarType(f->type.domain == Domain::Complex ? Domain::Complex : Domain::Real, f->type.dim / x->type.dim);
                if (f->type.shape != Shape::Scalar && f->type.shape != Shape::Unknown) t = Type {};
                return makeNode(Op::Derivative, { f }, t, at(it), variable, 1, numPartial ? 1 : 0);
            }
        }
        auto num = rowExpr(n.slots[0]);
        auto den = rowExpr(n.slots[1]);
        return mkDiv(num, den, at(it));
    }

    ExprPtr radical(const Item& it)
    {
        const auto& n = *it.node;
        auto x = rowExpr(n.slots[0]);
        ExprPtr index = n.slots[1] != nullptr ? rowExpr(n.slots[1])
                                              : makeNumber(Scalar::integer(2), scalarType(Domain::Integer, Dim::dimensionless(), Sign::Positive), at(it));
        return mkRoot(x, index, at(it));
    }

    ExprPtr delimited(const Item& it)
    {
        const auto& n = *it.node;
        if (n.text2.empty()) warn("unclosed-bracket", "A bracket is not closed.", at(it));
        const Seg body = segOf(n.slots[0]);
        if (n.text == "|")
        {
            auto inner = body.size() == 0 ? missing(body, 0, 0) : relationLevel(body, 0, body.size());
            return finishCall("abs", { inner }, at(it));
        }
        if (n.text == "(")
        {
            const auto parts = splitTop(body, 0, body.size(), ",");
            if (parts.size() == 1) return body.size() == 0 ? missing(body, 0, 0) : relationLevel(body, 0, body.size());
            std::vector<ExprPtr> elements;
            for (const auto& p : parts) elements.push_back(p.first >= p.second ? missing(body, p.first, p.second) : relationLevel(body, p.first, p.second));
            return makeNode(Op::Tuple, std::move(elements), Type {}, at(it));
        }
        if (n.text == "[")
        {
            const auto rows = splitTop(body, 0, body.size(), ";");
            const auto firstRow = splitTop(body, rows[0].first, rows[0].second, ",");
            if (rows.size() == 1 && firstRow.size() == 1) return body.size() == 0 ? missing(body, 0, 0) : relationLevel(body, 0, body.size());
            std::vector<ExprPtr> elements;
            size_t cols = 0;
            for (size_t r = 0; r < rows.size(); ++r)
            {
                const auto cells = splitTop(body, rows[r].first, rows[r].second, ",");
                if (r == 0) cols = cells.size();
                else if (cells.size() != cols) return hole("matrix-shape", "Every row of a matrix needs the same number of elements.", at(it));
                for (const auto& c : cells) elements.push_back(c.first >= c.second ? missing(body, c.first, c.second) : additive(body, c.first, c.second));
            }
            return rows.size() == 1 ? mkVector(std::move(elements), at(it)) : mkMatrix(std::move(elements), (int) rows.size(), (int) cols, at(it));
        }
        return hole("unsupported-delimiter", "The brackets " + n.text + (n.text2.empty() ? "" : n.text2) + " are not supported by the analysis yet.", at(it));
    }

    bool checkElements(const std::vector<ExprPtr>& elements, Domain& domain, Dim& dim, const Source& where)
    {
        bool ok = true, first = true;
        for (const auto& e : elements)
        {
            if (e->type.shape == Shape::Hole) { ok = false; continue; }
            if (e->type.shape != Shape::Scalar) { error("shape-mismatch", "The elements of a vector or matrix must be scalars.", where); return false; }
            domain = join(domain, e->type.domain);
            if (first) { dim = e->type.dim; first = false; }
            else if (dim.known && e->type.dim.known && dim != e->type.dim) { error("dimension-mismatch", "The elements of a vector or matrix must have the same dimension.", where); ok = false; }
            else if (!e->type.dim.known) dim = Dim::unknown();
        }
        return ok;
    }

    ExprPtr mkVector(std::vector<ExprPtr> elements, const Source& where)
    {
        Type t; t.shape = Shape::Vector; t.rows = (int) elements.size(); t.domain = Domain::Unknown; t.dim = Dim::dimensionless();
        if (anyHole(elements)) { t.shape = Shape::Hole; return makeNode(Op::Vector, std::move(elements), t, where); }
        if (!checkElements(elements, t.domain, t.dim, where)) t.dim = Dim::unknown();
        return makeNode(Op::Vector, std::move(elements), t, where);
    }

    ExprPtr mkMatrix(std::vector<ExprPtr> elements, int rows, int cols, const Source& where)
    {
        Type t; t.shape = Shape::Matrix; t.rows = rows; t.cols = cols; t.domain = Domain::Unknown; t.dim = Dim::dimensionless();
        if (anyHole(elements)) { t.shape = Shape::Hole; return makeNode(Op::Matrix, std::move(elements), t, where, {}, rows, cols); }
        if (!checkElements(elements, t.domain, t.dim, where)) t.dim = Dim::unknown();
        return makeNode(Op::Matrix, std::move(elements), t, where, {}, rows, cols);
    }

    // The end of the body of a sum or product that starts at `from`: before the next + - , ; or relation.
    size_t bodyEnd(const Seg& s, size_t from, size_t e) const
    {
        size_t i = from;
        while (i < e)
        {
            const auto& it = s.items[i];
            if (i > from && (isAddOp(it) || isOp(it, ",") || isOp(it, ";") || isOp(it, ":") || it.node->kind == Kind::Relation)) break;
            if (i == from && (it.node->kind == Kind::Relation || isOp(it, ",") || isOp(it, ";"))) break;
            i = skipIntegral(s, i, e);
        }
        return i;
    }

    ExprPtr bigOperator(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& n = *it.node;
        if (n.text == kSum || n.text == kProduct) return sumOrProduct(s, i, e, next);
        if (n.text == kIntegral) return integral(s, i, e, next);
        return hole("unsupported-operator", "The large operator '" + n.text + "' is not supported by the analysis yet.", at(it));
    }

    ExprPtr sumOrProduct(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& n = *it.node;
        const bool isSum = n.text == kSum;
        const char* label = isSum ? "sum" : "product";
        const size_t end = bodyEnd(s, i + 1, e);
        next = end;
        if (n.slots[0] == nullptr || n.slots[1] == nullptr)
            return hole("sum-bounds", std::string("A ") + label + " needs both bounds, written like sum_(i=1)^n.", at(it));
        const Seg lower = segOf(n.slots[0]);
        if (lower.size() < 3 || lower.items[0].node->kind != Kind::Identifier || !isText(lower.items[1], Kind::Relation, "="))
            return hole("sum-bounds", std::string("The lower bound of a ") + label + " must look like i=1.", at(n.slots[0]));
        const std::string index = lower.items[0].node->text;
        auto lo = additive(lower, 2, lower.size());
        auto hi = rowExpr(n.slots[1]);
        auto checkBound = [&](const ExprPtr& b) {
            if (b->type.shape == Shape::Scalar && (b->type.domain == Domain::Real || b->type.domain == Domain::Complex))
                error("sum-bounds", std::string("The bounds of a ") + label + " must be whole numbers.", b->src);
            if (b->type.dim.known && !b->type.dim.isDimensionless()) error("dimension-mismatch", std::string("The bounds of a ") + label + " must be dimensionless.", b->src);
        };
        checkBound(lo); checkBound(hi);
        if (end <= i + 1)
            return placeholderBody(range(s, i, i + 1), label);
        bound.push_back({ index, scalarType(Domain::Integer, Dim::dimensionless()) });
        auto body = multiplicative(s, i + 1, end);
        bound.pop_back();
        Type t;
        if (anyHole({ body, lo, hi })) t = holeType();
        else if (numericShape(body->type))
        {
            t = body->type;
            t.sign = Sign::Unknown;
            if (!isSum && !(body->type.dim.known && body->type.dim.isDimensionless())) t.dim = Dim::unknown();
        }
        return makeNode(isSum ? Op::Sum : Op::Product, { body, lo, hi }, t, range(s, i, end), index);
    }

    ExprPtr placeholderBody(const Source& where, const char* label)
    {
        return hole("missing-body", std::string("The ") + label + " has no body.", where);
    }

    ExprPtr integral(const Seg& s, size_t i, size_t e, size_t& next)
    {
        const auto& it = s.items[i];
        const auto& n = *it.node;
        size_t at = 0, after = 0;
        std::string variable;
        if (!findDifferential(s, i + 1, e, at, after, variable))
        {
            next = e;
            return hole("integral-differential", "An integral needs a differential such as dx at the end of its integrand.", this->at(it));
        }
        next = after;
        if ((n.slots[0] == nullptr) != (n.slots[1] == nullptr))
            return hole("integral-bounds", "A definite integral needs both bounds; an indefinite integral has none.", this->at(it));
        ExprPtr lo, hi;
        if (n.slots[0] != nullptr) { lo = rowExpr(n.slots[0]); hi = rowExpr(n.slots[1]); }
        if (at <= i + 1) return placeholderBody(range(s, i, after), "integral");
        bool declared = false;
        Type vt = freeType(variable, declared);
        bound.push_back({ variable, vt });
        auto body = additive(s, i + 1, at);
        bound.pop_back();
        Type t;
        std::vector<ExprPtr> args { body, lo, hi };
        if (anyHole({ body, lo, hi })) t = holeType();
        else if (numericShape(body->type) && body->type.shape == Shape::Scalar)
        {
            t = scalarType(join(body->type.domain, Domain::Real), body->type.dim * vt.dim);
            if (lo != nullptr && lo->type.dim.known && hi->type.dim.known && lo->type.dim != hi->type.dim)
                error("dimension-mismatch", "The bounds of the integral have different dimensions.", range(s, i, after));
        }
        return makeNode(Op::Integral, std::move(args), t, range(s, i, after), variable);
    }

    // ---- roles -------------------------------------------------------------------------------------------------------------------------------------
    bool headName(const Seg& s, size_t b, size_t e, std::string& name, std::vector<std::string>& params, bool& isFunction)
    {
        isFunction = false;
        params.clear();
        if (e - b == 1 && s.items[b].node->kind == Kind::Identifier) { name = s.items[b].node->text; return true; }
        if (e - b == 1 && s.items[b].node->kind == Kind::Script)
        {
            bool ok = false;
            name = nameOfScript(*s.items[b].node, ok);
            return ok;
        }
        if (e - b == 2 && s.items[b].node->kind == Kind::Identifier && s.items[b + 1].node->kind == Kind::Delimited && s.items[b + 1].node->text == "(")
        {
            name = s.items[b].node->text;
            isFunction = true;
            const Seg body = segOf(s.items[b + 1].node->slots[0]);
            for (const auto& part : splitTop(body, 0, body.size(), ","))
            {
                if (part.second - part.first == 1 && body.items[part.first].node->kind == Kind::Identifier) params.push_back(body.items[part.first].node->text);
                else if (part.second - part.first == 1 && body.items[part.first].node->kind == Kind::Script)
                {
                    bool ok = false;
                    const auto p = nameOfScript(*body.items[part.first].node, ok);
                    if (!ok) return false;
                    params.push_back(p);
                }
                else return false;
            }
            return !params.empty();
        }
        return false;
    }

    ExprPtr definitionLine(const Seg& s)
    {
        size_t r = s.size();
        for (size_t i = 0; i < s.size(); ++i)
            if (s.items[i].node->kind == Kind::Relation) { r = i; break; }
        if (r == s.size() || r == 0 || (!isText(s.items[r], Kind::Relation, "=") && !isText(s.items[r], Kind::Relation, kAssign)))
            return hole("definition-form", "A definition looks like name = expression, name := expression or f(x) = expression.", range(s, 0, s.size()));
        std::string name;
        std::vector<std::string> params;
        bool isFunction = false;
        if (!headName(s, 0, r, name, params, isFunction))
            return hole("definition-form", "The left side of a definition must be a name or a function such as f(x, y).", range(s, 0, r));
        for (size_t i = r + 1; i < s.size(); ++i)
            if (s.items[i].node->kind == Kind::Relation) return hole("definition-form", "A definition has one relation: name = expression.", at(s.items[i]));
        selfName = name;
        for (const auto& p : params) bound.push_back({ p, scalarType(options.defaultDomain, Dim::unknown()) });
        ExprPtr body = r + 1 >= s.size() ? missing(s, r + 1, s.size()) : additive(s, r + 1, s.size());
        for (size_t k = 0; k < params.size(); ++k) bound.pop_back();
        selfName.clear();
        const auto where = range(s, 0, s.size());
        const auto headSource = range(s, 0, r);
        ExprPtr head;
        if (isFunction)
        {
            std::vector<ExprPtr> ps;
            for (const auto& p : params) ps.push_back(makeSymbol(p, scalarType(options.defaultDomain, Dim::unknown()), headSource));
            head = makeNode(Op::Call, std::move(ps), Type { Shape::Function, Domain::Unknown, Dim::unknown(), Sign::Unknown, 0, 0 }, headSource, name);
        }
        else
            head = makeSymbol(name, body->type, headSource);
        if (out->defines.count(name) > 0) error("duplicate-definition", "'" + name + "' is defined twice in this cell.", headSource);
        out->defines.insert(name);
        out->defineSites[name] = headSource;
        Definition def;
        def.name = name; def.cell = cell; def.isFunction = isFunction; def.params = params; def.type = body->type; def.body = body; def.where = headSource;
        out->definitions.push_back(def);
        Type t; t.shape = Shape::Relation;
        return makeNode(Op::Define, { head, body }, t, where);
    }

    ExprPtr assumptionLine(const Seg& s)
    {
        size_t r = s.size();
        for (size_t i = 0; i < s.size(); ++i)
            if (s.items[i].node->kind == Kind::Relation) { r = i; break; }
        if (r == s.size() || r == 0 || r + 1 >= s.size())
            return hole("assumption-form", "An assumption looks like x in R, v in \"m/s\", x > 0 or n in Z.", range(s, 0, s.size()));
        std::vector<std::pair<std::string, Source>> names;
        for (const auto& part : splitTop(s, 0, r, ","))
        {
            std::string name;
            std::vector<std::string> params;
            bool isFunction = false;
            if (part.first >= part.second || !headName(s, part.first, part.second, name, params, isFunction) || isFunction)
                return hole("assumption-form", "The left side of an assumption lists plain symbols, such as x, y.", range(s, 0, r));
            names.push_back({ name, range(s, part.first, part.second) });
        }
        const auto& relation = *s.items[r].node;
        const auto where = range(s, 0, s.size());
        std::vector<Declaration> made;
        if (relation.text == kElementOf)
        {
            Domain d = Domain::Unknown;
            if (r + 2 == s.size() && s.items[r + 1].node->kind == Kind::Text)
            {
                const auto parsed = math_units::parse(s.items[r + 1].node->text, at(s.items[r + 1]));
                if (!parsed.ok) return hole("unknown-unit", parsed.error, at(s.items[r + 1]));
                for (const auto& nm : names)
                {
                    Declaration decl; decl.name = nm.first; decl.cell = cell; decl.where = nm.second;
                    decl.hasDim = true; decl.dim = parsed.dim; decl.unit = s.items[r + 1].node->text;
                    decl.hasDomain = true; decl.domain = Domain::Real;
                    made.push_back(decl);
                }
            }
            else if (r + 2 == s.size() && s.items[r + 1].node->kind == Kind::Identifier)
            {
                const auto& id = *s.items[r + 1].node;
                bool known = true;
                if (id.text == "\xE2\x84\x9D") d = Domain::Real;
                else if (id.text == "\xE2\x84\x82") d = Domain::Complex;
                else if (id.text == "\xE2\x84\xA4" || id.text == "\xE2\x84\x95") d = Domain::Integer;
                else if (id.text == "\xE2\x84\x9A") d = Domain::Rational;
                else known = domainFromName(id.text, d);
                if (!known) return hole("assumption-form", "'" + id.text + "' is not a number set; use R, C, Z, Q or N, or a quoted unit.", at(s.items[r + 1]));
                for (const auto& nm : names)
                {
                    Declaration decl; decl.name = nm.first; decl.cell = cell; decl.where = nm.second;
                    decl.hasDomain = true; decl.domain = d;
                    made.push_back(decl);
                }
            }
            else return hole("assumption-form", "After 'in' write a number set (R, C, Z, Q, N) or a quoted unit.", where);
        }
        else
        {
            Op op = Op::Eq;
            if (!relationOp(relation.text, op) || op == Op::Eq || op == Op::Approx)
                return hole("assumption-form", "An assumption uses 'in', or compares a symbol with zero: x > 0, x >= 0, x < 0, x <= 0, x != 0.", at(s.items[r]));
            Rational zero;
            auto rhs = additive(s, r + 1, s.size());
            if (!exactNumber(rhs, zero) || !zero.isZero())
            {
                warn("assumption-sign", "Only comparisons with zero are used as sign assumptions.", where);
                std::vector<ExprPtr> sides;
                for (const auto& nm : names) sides.push_back(symbol(nm.first, nm.second));
                sides.push_back(rhs);
                return mkRelation(op, std::move(sides), where);
            }
            for (const auto& nm : names)
            {
                Declaration decl; decl.name = nm.first; decl.cell = cell; decl.where = nm.second;
                decl.hasSign = true;
                decl.sign = op == Op::Gt ? Sign::Positive : op == Op::Ge ? Sign::Nonnegative : op == Op::Lt ? Sign::Negative : op == Op::Le ? Sign::Nonpositive : Sign::Nonzero;
                made.push_back(decl);
            }
        }
        for (const auto& decl : made)
        {
            out->declares.insert(decl.name);
            out->declarations.push_back(decl);
        }
        return nullptr;
    }
};
}

bool CellAnalysis::hasErrors() const
{
    for (const auto& d : diagnostics) if (d.severity == Diagnostic::Severity::Error) return true;
    return false;
}

std::shared_ptr<const CellAnalysis> analyzeCell(const math_doc::MathSnapshot& snapshot, Resolver* resolver, const Options& options)
{
    Translator translator(snapshot, resolver, options);
    return translator.run();
}

Source toSource(const std::string& cell, const math_doc::SourceRef& ref)
{
    Source s;
    s.cell = cell; s.node = ref.node; s.begin = ref.childBegin; s.end = ref.childEnd;
    return s;
}

math_doc::SourceRef toSourceRef(const Source& source)
{
    math_doc::SourceRef r;
    r.node = source.node; r.childBegin = source.begin; r.childEnd = source.end;
    return r;
}

math_doc::AnalysisResult toAnalysisResult(const CellAnalysis& analysis)
{
    math_doc::AnalysisResult result;
    result.cellId = analysis.cell;
    result.forRevision = analysis.revision;
    result.complete = analysis.complete;
    for (const auto& d : analysis.diagnostics)
    {
        math_doc::Diagnostic out;
        out.where = toSourceRef(d.where);
        out.severity = d.severity == Diagnostic::Severity::Info ? math_doc::Diagnostic::Severity::Info
                     : d.severity == Diagnostic::Severity::Warning ? math_doc::Diagnostic::Severity::Warning
                                                                    : math_doc::Diagnostic::Severity::Error;
        out.code = d.code;
        out.message = d.message;
        result.diagnostics.push_back(std::move(out));
    }
    return result;
}

void Frontend::analyze(const math_doc::MathSnapshot& snapshot, std::function<void(math_doc::AnalysisResult)> done)
{
    const auto analysis = analyzeCell(snapshot, resolver, options);
    if (done) done(toAnalysisResult(*analysis));
}

bool isBuiltinFunction(const std::string& name) { return findBuiltin(name) != nullptr; }
}
