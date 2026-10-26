#pragma once

// MathIR: the typed, immutable intermediate representation of Djehuti Math mathematics.
//
// MathIR is what computation engines receive; the structured presentation trees of math_doc are what the
// user edits and sees. MathIR is derived from a snapshot of a cell by the frontend, is never saved in a
// document, and depends on no GUI, document, font or solver library: this header and its implementation use
// the C++ standard library only. Every node carries its mathematical type (shape, number domain, physical
// dimension, sign) and the source reference (cell, node, item range) of the notation it came from, so that
// diagnostics and results can always be traced back to what the user wrote.
//
// Expressions are immutable and shared (shared_ptr<const Expr>): substitution and simplification build new
// nodes only along the changed paths and share everything else.

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace math_ir
{
// ---- exact rationals ---------------------------------------------------------------------------------------------------
// Overflow of the 64-bit components is detected and reported (`ok` false) rather than wrapped.
struct Rational
{
    std::int64_t num = 0, den = 1;
    Rational() = default;
    Rational(std::int64_t n) : num(n) {}
    Rational(std::int64_t n, std::int64_t d);   // normalised; d == 0 gives 0/1 with ok() false
    bool ok() const { return den > 0; }
    bool isInteger() const { return den == 1; }
    bool isZero() const { return num == 0; }
    double toDouble() const { return (double) num / (double) den; }
    bool operator==(const Rational& o) const { return num == o.num && den == o.den; }
    bool operator!=(const Rational& o) const { return !(*this == o); }
    bool operator<(const Rational& o) const;
    std::string toString() const;
};
// False (and `out` untouched) when the exact result does not fit in 64 bits or divides by zero.
bool add(const Rational& a, const Rational& b, Rational& out);
bool sub(const Rational& a, const Rational& b, Rational& out);
bool mul(const Rational& a, const Rational& b, Rational& out);
bool div(const Rational& a, const Rational& b, Rational& out);
bool pow(const Rational& base, std::int64_t exponent, Rational& out);

// ---- physical dimensions ------------------------------------------------------------------------------------------------
// Exponents of the seven SI base dimensions: length, mass, time, current, temperature, amount, luminous intensity.
// A dimension may be unknown (nothing has declared it); an unknown dimension never causes a dimension error.
struct Dim
{
    bool known = true;
    std::array<Rational, 7> e {};
    static Dim dimensionless() { return {}; }
    static Dim unknown() { Dim d; d.known = false; return d; }
    static Dim base(int index);
    bool isDimensionless() const;
    bool operator==(const Dim& o) const { return known == o.known && e == o.e; }
    bool operator!=(const Dim& o) const { return !(*this == o); }
    Dim operator*(const Dim& o) const;
    Dim operator/(const Dim& o) const;
    Dim power(const Rational& p) const;
    // "m*s^-2", "1" for dimensionless, "?" for unknown; base-dimension symbols m kg s A K mol cd.
    std::string toString() const;
};

// ---- types ------------------------------------------------------------------------------------------------------------------
enum class Domain : std::uint8_t { Unknown, Integer, Rational, Real, Complex };
enum class Shape : std::uint8_t { Unknown, Scalar, Vector, Matrix, Relation, Function, System, Hole };
enum class Sign : std::uint8_t { Unknown, Zero, Positive, Nonnegative, Negative, Nonpositive, Nonzero };

const char* domainName(Domain d);
const char* shapeName(Shape s);
const char* signName(Sign s);
bool domainFromName(const std::string& name, Domain& out);   // N and Z are Integer, Q Rational, R Real, C Complex
Domain join(Domain a, Domain b);                              // the smallest domain holding both

struct Type
{
    Shape shape = Shape::Unknown;
    Domain domain = Domain::Unknown;
    Dim dim = Dim::unknown();
    Sign sign = Sign::Unknown;
    int rows = 0, cols = 0;      // Vector: rows; Matrix: rows x cols (0 = not known)
    bool operator==(const Type& o) const;
    std::string toString() const;
};

// ---- scalar constants -----------------------------------------------------------------------------------------------------
struct Scalar
{
    Domain domain = Domain::Integer;    // Integer and Rational are exact (`q`); Real and Complex are approximate
    Rational q;
    double re = 0, im = 0;
    std::string text;                   // the literal as written, for display and provenance
    static Scalar integer(std::int64_t v);
    static Scalar rational(const Rational& q);
    static Scalar real(double v, std::string text = {});
    static Scalar complexNumber(double re, double im);
    bool isExact() const { return domain == Domain::Integer || domain == Domain::Rational; }
    double toDouble() const;            // the real part for approximate values
    bool operator==(const Scalar& o) const;
    std::string toString() const;
};

// ---- source references -------------------------------------------------------------------------------------------------------
// The place in the user's notation that a node came from: a node of the cell's presentation tree, or the
// items [begin, end) of a Row node (0,0 = the node itself).
struct Source
{
    std::string cell;
    std::uint64_t node = 0;
    std::uint32_t begin = 0, end = 0;
    bool valid() const { return node != 0; }
    bool operator==(const Source& o) const { return cell == o.cell && node == o.node && begin == o.begin && end == o.end; }
};

// ---- diagnostics ----------------------------------------------------------------------------------------------------------
struct Diagnostic
{
    enum class Severity : std::uint8_t { Info, Warning, Error } severity = Severity::Error;
    std::string code, message;
    Source where;
};
const char* severityName(Diagnostic::Severity s);

// ---- expressions ----------------------------------------------------------------------------------------------------------
enum class Op : std::uint8_t
{
    // leaves
    Number,       // value
    Symbol,       // name: a variable
    Constant,     // name: pi, e, i, infty
    Unit,         // name: a unit symbol; value.re is its scale to SI, type.dim its dimension
    Hole,         // an incomplete position (a placeholder or an error); never evaluates
    // arithmetic
    Add, Mul,     // n-ary, n >= 2
    Div,          // numerator, denominator
    Neg,          // one argument
    Pow,          // base, exponent
    Root,         // radicand, index (index 2 for a square root)
    Call,         // name: the function; arguments follow
    Sum, Product, // name: the index variable; args: body, lower, upper
    Integral,     // name: the variable; args: body, lower, upper (the bounds are null for an indefinite integral)
    Derivative,   // name: the variable; args: body; rows: order; cols: 1 for a partial derivative
    Limit,        // reserved for later milestones: the frontend does not produce it
    Index,        // base, index expressions: element of a vector or matrix
    // relations
    Eq, Ne, Lt, Le, Gt, Ge, Approx,
    Define,       // `:=`/definition: args: head (a Symbol, or a Call whose arguments are the parameters), body
    // collections
    Vector,       // elements
    Matrix,       // rows x cols elements, row-major
    Tuple,
    System        // equations / relations that hold together
};
const char* opName(Op op);
bool isRelation(Op op);
bool isBinder(Op op);   // Sum, Product, Integral bind `name` inside their body

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct Expr
{
    Op op = Op::Hole;
    std::string name;
    Scalar value;
    std::vector<ExprPtr> args;    // may hold null for an absent optional argument (Integral bounds)
    Type type;
    Source src;
    std::int32_t rows = 0, cols = 0;
    std::uint64_t hash = 0;       // structural: ignores `src` and `type`
};

// construction (the hash is computed here; nodes are never modified afterwards)
ExprPtr makeNumber(const Scalar& value, const Type& type, const Source& src = {});
ExprPtr makeSymbol(const std::string& name, const Type& type, const Source& src = {});
ExprPtr makeConstant(const std::string& name, const Scalar& value, const Type& type, const Source& src = {});
ExprPtr makeUnit(const std::string& name, double scaleToSi, const Dim& dim, const Source& src = {});
ExprPtr makeHole(const std::string& reason, const Source& src = {});
ExprPtr makeNode(Op op, std::vector<ExprPtr> args, const Type& type, const Source& src = {}, const std::string& name = {},
                 std::int32_t rows = 0, std::int32_t cols = 0);
// the same node with different type / source (arguments shared)
ExprPtr withType(const ExprPtr& e, const Type& type);
ExprPtr withSource(const ExprPtr& e, const Source& src);

// structure
bool equals(const ExprPtr& a, const ExprPtr& b);          // structural, ignoring source and type
bool hasHole(const ExprPtr& e);
int countNodes(const ExprPtr& e);
std::set<std::string> freeSymbols(const ExprPtr& e);       // honours binders
// Replaces free symbols; unchanged subtrees are shared with `e`.
ExprPtr substitute(const ExprPtr& e, const std::map<std::string, ExprPtr>& values);
// A canonical text for diagnostics and tests, e.g. (add (num 1) (sym x)). Not a notation for users.
std::string dump(const ExprPtr& e);
}
