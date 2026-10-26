#pragma once

// The common representation of computational values in Djehuti Math. A Value is what any engine returns and
// what the worksheet stores as a cell's result: exact and approximate scalars, complex numbers, vectors and
// matrices, symbolic expressions, booleans (the truth of a relation), and the two ways of not having a value:
// Undefined (the question has no answer, such as dividing by zero) and Failed (the engine could not answer).
//
// Numeric values are held in coherent SI together with their physical dimension, so a quantity of
// "9.81 m/s^2" is the Real 9.81 with dimension m*s^-2, and 3 g is 0.003 with dimension kg. Display units are a
// presentation matter. A Value knows nothing of any engine: engines fill it in and describe themselves in its
// provenance. No GUI, document or solver dependency.

#include "MathIR.h"

namespace math_value
{
enum class Kind : std::uint8_t { Undefined, Failed, Boolean, Integer, Rational, Real, Complex, Vector, Matrix, Symbolic };
const char* kindName(Kind kind);

// Where a result came from, so it can be trusted, compared, reproduced and shown to the user.
struct Provenance
{
    std::string engine, version, method;   // which engine, and what it did ("exact rational arithmetic")
    bool exact = false;                    // true when the result is mathematically exact
    std::string cell;                      // the cell the question was about
    std::uint64_t cellRevision = 0;        // and the revision of it that was analysed
    double elapsedMs = 0;
};

class Value
{
public:
    Kind kind = Kind::Undefined;
    math_ir::Scalar scalar;                 // Integer, Rational, Real, Complex
    bool truth = false;                     // Boolean
    math_ir::Dim dim = math_ir::Dim::dimensionless();   // of a numeric value (the elements of a Vector or Matrix share it)
    double errorBound = -1;                 // absolute error estimate of an approximate value; < 0 when not known
    std::vector<Value> elements;            // Vector: the elements; Matrix: rows * cols, row-major
    int rows = 0, cols = 0;
    math_ir::ExprPtr expr;                  // Symbolic
    std::vector<math_ir::Diagnostic> diagnostics;   // why it is Undefined or Failed; warnings about the others
    Provenance provenance;

    static Value integer(std::int64_t v);
    static Value rational(const math_ir::Rational& q);
    static Value real(double v, double errorBound = -1);
    static Value complexNumber(double re, double im);
    static Value boolean(bool v);
    static Value vector(std::vector<Value> elements);
    static Value matrix(int rows, int cols, std::vector<Value> elements);   // an inconsistent size gives a Failed value
    static Value symbolic(math_ir::ExprPtr expression);
    static Value undefined(const std::string& code, const std::string& message, const math_ir::Source& where = {});
    static Value failed(const std::string& code, const std::string& message, const math_ir::Source& where = {});

    bool ok() const { return kind != Kind::Undefined && kind != Kind::Failed; }
    bool isScalar() const { return kind == Kind::Integer || kind == Kind::Rational || kind == Kind::Real || kind == Kind::Complex; }
    bool isExact() const;                   // exact scalars, and collections of exact elements
    Value withDim(const math_ir::Dim& d) const { Value v = *this; v.dim = d; return v; }
    // A canonical text for diagnostics and tests, e.g. "1/3", "0.5 [m*s^-1]", "[1, 2]", "undefined(div-zero)".
    std::string describe() const;
};
}
