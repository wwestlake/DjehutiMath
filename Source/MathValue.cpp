#include "MathValue.h"

namespace math_value
{
const char* kindName(Kind kind)
{
    switch (kind)
    {
        case Kind::Undefined: return "undefined";
        case Kind::Failed: return "failed";
        case Kind::Boolean: return "boolean";
        case Kind::Integer: return "integer";
        case Kind::Rational: return "rational";
        case Kind::Real: return "real";
        case Kind::Complex: return "complex";
        case Kind::Vector: return "vector";
        case Kind::Matrix: return "matrix";
        default: return "symbolic";
    }
}

Value Value::integer(std::int64_t v) { Value r; r.kind = Kind::Integer; r.scalar = math_ir::Scalar::integer(v); return r; }

Value Value::rational(const math_ir::Rational& q)
{
    Value r;
    r.kind = q.isInteger() ? Kind::Integer : Kind::Rational;
    r.scalar = math_ir::Scalar::rational(q);
    return r;
}

Value Value::real(double v, double errorBound) { Value r; r.kind = Kind::Real; r.scalar = math_ir::Scalar::real(v); r.errorBound = errorBound; return r; }
Value Value::complexNumber(double re, double im) { Value r; r.kind = Kind::Complex; r.scalar = math_ir::Scalar::complexNumber(re, im); return r; }
Value Value::boolean(bool v) { Value r; r.kind = Kind::Boolean; r.truth = v; return r; }

Value Value::vector(std::vector<Value> elements)
{
    Value r;
    r.kind = Kind::Vector;
    r.rows = (int) elements.size();
    r.cols = 1;
    if (!elements.empty()) r.dim = elements[0].dim;
    r.elements = std::move(elements);
    return r;
}

Value Value::matrix(int rows, int cols, std::vector<Value> elements)
{
    if (rows <= 0 || cols <= 0 || (size_t) rows * (size_t) cols != elements.size())
        return failed("matrix-shape", "A matrix needs rows * columns elements.");
    Value r;
    r.kind = Kind::Matrix;
    r.rows = rows;
    r.cols = cols;
    r.dim = elements[0].dim;
    r.elements = std::move(elements);
    return r;
}

Value Value::symbolic(math_ir::ExprPtr expression)
{
    Value r;
    r.kind = Kind::Symbolic;
    r.expr = std::move(expression);
    return r;
}

Value Value::undefined(const std::string& code, const std::string& message, const math_ir::Source& where)
{
    Value r;
    r.kind = Kind::Undefined;
    r.diagnostics.push_back({ math_ir::Diagnostic::Severity::Error, code, message, where });
    return r;
}

Value Value::failed(const std::string& code, const std::string& message, const math_ir::Source& where)
{
    Value r;
    r.kind = Kind::Failed;
    r.diagnostics.push_back({ math_ir::Diagnostic::Severity::Error, code, message, where });
    return r;
}

bool Value::isExact() const
{
    switch (kind)
    {
        case Kind::Integer: case Kind::Rational: case Kind::Boolean: return true;
        case Kind::Vector: case Kind::Matrix:
            for (const auto& e : elements) if (!e.isExact()) return false;
            return !elements.empty();
        case Kind::Symbolic: return true;
        default: return false;
    }
}

std::string Value::describe() const
{
    std::string out;
    switch (kind)
    {
        case Kind::Undefined: case Kind::Failed:
            out = std::string(kindName(kind)) + "(" + (diagnostics.empty() ? std::string() : diagnostics[0].code) + ")";
            return out;
        case Kind::Boolean: return truth ? "true" : "false";
        case Kind::Symbolic: return math_ir::dump(expr);
        case Kind::Vector: case Kind::Matrix:
        {
            out = "[";
            for (size_t i = 0; i < elements.size(); ++i)
            {
                if (i != 0)
                    out += (kind == Kind::Matrix && cols > 0 && (int) i % cols == 0) ? "; " : ", ";
                out += elements[i].describe();
            }
            out += "]";
            break;
        }
        default:
            out = scalar.toString();
            if (kind == Kind::Real && errorBound >= 0)
                out += " +/- " + std::to_string(errorBound);
            break;
    }
    if (isScalar() && dim.known && !dim.isDimensionless()) out += " [" + dim.toString() + "]";
    return out;
}
}
