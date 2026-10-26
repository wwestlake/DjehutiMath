#include "MathResult.h"

#include "MathUnits.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace math_result
{
using namespace math_doc;
using math_ir::Op;

namespace
{
const char* const kMinus = "\xE2\x88\x92";
const char* const kCdot = "\xE2\x8B\x85";
const char* const kTimes = "\xC3\x97";
const char* const kThinSpace = "\xE2\x80\x89";
const char* const kAssign = "\xE2\x89\x94";

struct Precedence { static constexpr int Relation = 0, Add = 1, Mul = 2, Power = 3, Atom = 4; };

class Builder
{
public:
    explicit Builder(IdAllocator& allocator) : ids(allocator) {}
    Tree tree;

    NodePtr row(std::vector<NodePtr> items) { return items.empty() ? placeholderRow(ids) : makeRow(ids, std::move(items)); }
    NodePtr atom(Kind kind, const std::string& text, Style style = Style::Auto) { return makeAtom(ids, kind, text, style); }
    NodePtr note(NodePtr node, const math_ir::ExprPtr& e)
    {
        if (e != nullptr && e->src.valid() && node != nullptr) tree.derivedFrom[node->id] = e->src;
        return node;
    }

    // ---- values ------------------------------------------------------------------------------------------------------------
    void number(std::vector<NodePtr>& out, const std::string& text) { out.push_back(atom(Kind::Number, text)); }

    static std::string shortest(double v, int digits)
    {
        char buffer[64];
        std::snprintf(buffer, sizeof buffer, "%.*g", digits, v);
        return buffer;
    }

    void real(std::vector<NodePtr>& out, double v, int digits)
    {
        if (std::isnan(v)) { out.push_back(atom(Kind::Text, "undefined")); return; }
        if (std::isinf(v))
        {
            if (v < 0) out.push_back(atom(Kind::Operator, kMinus));
            out.push_back(atom(Kind::Symbol, "\xE2\x88\x9E"));
            return;
        }
        if (v < 0) { out.push_back(atom(Kind::Operator, kMinus)); v = -v; }
        const auto text = shortest(v, digits);
        const auto e = text.find('e');
        if (e == std::string::npos) { number(out, text); return; }
        std::string mantissa = text.substr(0, e);
        int exponent = std::atoi(text.c_str() + e + 1);
        number(out, mantissa);
        out.push_back(atom(Kind::Operator, kTimes));
        number(out, "10");
        std::vector<NodePtr> sup;
        if (exponent < 0) { sup.push_back(atom(Kind::Operator, kMinus)); exponent = -exponent; }
        number(sup, std::to_string(exponent));
        out.push_back(makeScript(ids, row({ out.back() }), nullptr, row(std::move(sup))));
        // the "10" atom is now the script's base; remove the duplicate left in the list
        out.erase(out.end() - 2);
    }

    void rational(std::vector<NodePtr>& out, const math_ir::Rational& q)
    {
        if (!q.ok()) { out.push_back(atom(Kind::Text, "undefined")); return; }
        const bool negative = q.num < 0;
        const std::int64_t n = negative ? -q.num : q.num;
        if (negative) out.push_back(atom(Kind::Operator, kMinus));
        if (q.isInteger()) { number(out, std::to_string(n)); return; }
        out.push_back(makeFraction(ids, row({ atom(Kind::Number, std::to_string(n)) }), row({ atom(Kind::Number, std::to_string(q.den)) })));
    }

    void scalar(std::vector<NodePtr>& out, const math_value::Value& v, double divisor, int digits)
    {
        switch (v.kind)
        {
            case math_value::Kind::Integer: case math_value::Kind::Rational:
                if (divisor == 1.0) rational(out, v.scalar.q);
                else real(out, v.scalar.q.toDouble() / divisor, digits);
                break;
            case math_value::Kind::Real: real(out, v.scalar.re / divisor, digits); break;
            default:
            {
                real(out, v.scalar.re / divisor, digits);
                const double im = v.scalar.im / divisor;
                if (im == 0.0) break;
                out.push_back(atom(Kind::Operator, im < 0 ? kMinus : "+"));
                if (std::abs(im) != 1.0) real(out, std::abs(im), digits);
                out.push_back(atom(Kind::Identifier, "i", Style::Upright));
                break;
            }
        }
    }

    void dimensionUnits(std::vector<NodePtr>& out, const math_ir::Dim& dim)
    {
        static const char* const names[7] = { "m", "kg", "s", "A", "K", "mol", "cd" };
        std::vector<NodePtr> numerator, denominator;
        auto factor = [&](std::vector<NodePtr>& list, int i, std::int64_t power) {
            if (!list.empty()) list.push_back(atom(Kind::Operator, kCdot));
            auto unit = atom(Kind::Identifier, names[i], Style::Upright);
            if (power == 1) list.push_back(unit);
            else list.push_back(makeScript(ids, row({ unit }), nullptr, row({ atom(Kind::Number, std::to_string(power)) })));
        };
        for (int i = 0; i < 7; ++i)
        {
            const auto& e = dim.e[(size_t) i];
            if (e.isZero()) continue;
            if (!e.isInteger()) { out.push_back(atom(Kind::Text, "(" + dim.toString() + ")")); return; }
            if (e.num > 0) factor(numerator, i, e.num); else factor(denominator, i, -e.num);
        }
        if (numerator.empty() && denominator.empty()) return;
        if (denominator.empty()) { for (auto& n : numerator) out.push_back(n); return; }
        if (numerator.empty()) numerator.push_back(atom(Kind::Number, "1"));
        out.push_back(makeFraction(ids, row(std::move(numerator)), row(std::move(denominator))));
    }

    void value(std::vector<NodePtr>& out, const math_value::Value& v, const Options& options)
    {
        using K = math_value::Kind;
        switch (v.kind)
        {
            case K::Undefined: out.push_back(atom(Kind::Text, "undefined")); return;
            case K::Failed: out.push_back(atom(Kind::Text, "failed")); return;
            case K::Boolean: out.push_back(atom(Kind::Text, v.truth ? "true" : "false")); return;
            case K::Symbolic: emit(v.expr, Precedence::Relation, out); return;
            case K::Vector: case K::Matrix:
            {
                std::vector<NodePtr> body;
                for (size_t i = 0; i < v.elements.size(); ++i)
                {
                    if (i != 0)
                        body.push_back(atom(Kind::Operator, (v.kind == K::Matrix && v.cols > 0 && (int) i % v.cols == 0) ? ";" : ","));
                    value(body, v.elements[i], options);
                }
                out.push_back(makeDelimited(ids, "[", "]", row(std::move(body))));
                return;
            }
            default: break;
        }
        double divisor = 1.0;
        bool shown = false;
        math_ir::ExprPtr unitExpr;
        if (!options.displayUnit.empty() && v.dim.known)
        {
            const auto parsed = math_units::parse(options.displayUnit);
            if (parsed.ok && parsed.dim == v.dim) { divisor = parsed.scale; unitExpr = parsed.expr; }
            else tree.problem = "The unit '" + options.displayUnit + "' does not match the dimension " + v.dim.toString() + "; shown in SI.";
        }
        scalar(out, v, divisor, options.significantDigits);
        if (v.dim.known && !v.dim.isDimensionless())
        {
            out.push_back(atom(Kind::Symbol, kThinSpace));
            if (unitExpr != nullptr) emit(unitExpr, Precedence::Relation, out);
            else dimensionUnits(out, v.dim);
            shown = true;
        }
        (void) shown;
    }

    // ---- expressions -----------------------------------------------------------------------------------------------------------
    static int precedenceOf(const math_ir::ExprPtr& e)
    {
        if (e == nullptr) return Precedence::Atom;
        switch (e->op)
        {
            case Op::Add: return Precedence::Add;
            case Op::Neg: return Precedence::Add;
            case Op::Mul: return Precedence::Mul;
            case Op::Pow: return Precedence::Power;
            case Op::Define: case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Approx: return Precedence::Relation;
            case Op::Number:
                return (e->value.isExact() && !e->value.q.isInteger()) || (e->value.isExact() && e->value.q.num < 0) ? Precedence::Mul : Precedence::Atom;
            default: return Precedence::Atom;
        }
    }

    void group(const math_ir::ExprPtr& e, int minimum, std::vector<NodePtr>& out)
    {
        if (precedenceOf(e) < minimum)
        {
            std::vector<NodePtr> inner;
            emit(e, Precedence::Relation, inner);
            out.push_back(makeDelimited(ids, "(", ")", row(std::move(inner))));
        }
        else emit(e, minimum, out);
    }

    NodePtr slot(const math_ir::ExprPtr& e)
    {
        std::vector<NodePtr> items;
        emit(e, Precedence::Relation, items);
        return row(std::move(items));
    }

    static bool isUnitExpr(const math_ir::ExprPtr& e)
    {
        if (e == nullptr) return false;
        if (e->op == Op::Unit) return true;
        if (e->op == Op::Number) return e->value.isExact() && e->value.q == math_ir::Rational(1);
        if (e->op == Op::Pow || e->op == Op::Mul || e->op == Op::Div)
        {
            bool any = false;
            for (size_t i = 0; i < e->args.size(); ++i)
            {
                if (e->op == Op::Pow && i == 1) continue;
                if (!isUnitExpr(e->args[i])) return false;
                any = any || e->args[i]->op != Op::Number;
            }
            return any;
        }
        return false;
    }

    void symbolName(std::vector<NodePtr>& out, const std::string& name, const math_ir::ExprPtr& e)
    {
        const auto underscore = name.find('_');
        if (underscore == std::string::npos || underscore == 0 || underscore + 1 >= name.size())
        {
            out.push_back(note(atom(Kind::Identifier, name), e));
            return;
        }
        const auto base = name.substr(0, underscore), sub = name.substr(underscore + 1);
        const bool digits = sub.find_first_not_of("0123456789") == std::string::npos;
        out.push_back(note(makeScript(ids, row({ atom(Kind::Identifier, base) }), row({ atom(digits ? Kind::Number : Kind::Identifier, sub) }), nullptr), e));
    }

    void emit(const math_ir::ExprPtr& e, int minimum, std::vector<NodePtr>& out)
    {
        if (e == nullptr) { out.push_back(placeholderNode()); return; }
        switch (e->op)
        {
            case Op::Number:
            {
                if (precedenceOf(e) < minimum && minimum > Precedence::Relation)
                {
                    std::vector<NodePtr> inner;
                    scalarOf(e, inner);
                    out.push_back(makeDelimited(ids, "(", ")", row(std::move(inner))));
                    return;
                }
                scalarOf(e, out);
                return;
            }
            case Op::Symbol: symbolName(out, e->name, e); return;
            case Op::Constant:
                if (e->name == "pi") out.push_back(note(atom(Kind::Identifier, "\xCF\x80"), e));
                else if (e->name == "infty") out.push_back(note(atom(Kind::Symbol, "\xE2\x88\x9E"), e));
                else out.push_back(note(atom(Kind::Identifier, e->name, Style::Upright), e));
                return;
            case Op::Unit: out.push_back(note(atom(Kind::Identifier, e->name, Style::Upright), e)); return;
            case Op::Hole: out.push_back(note(placeholderNode(), e)); return;
            case Op::Add:
            {
                if (minimum > Precedence::Add) { group(e, minimum, out); return; }
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    const auto& t = e->args[i];
                    if (t != nullptr && t->op == Op::Neg && t->args.size() == 1)
                    {
                        out.push_back(atom(Kind::Operator, kMinus));
                        group(t->args[0], Precedence::Mul, out);
                    }
                    else
                    {
                        if (i != 0) out.push_back(atom(Kind::Operator, "+"));
                        group(t, Precedence::Add, out);
                    }
                }
                return;
            }
            case Op::Neg:
                if (minimum > Precedence::Add) { group(e, minimum, out); return; }
                out.push_back(atom(Kind::Operator, kMinus));
                group(e->args.at(0), Precedence::Mul, out);
                return;
            case Op::Mul:
            {
                if (minimum > Precedence::Mul) { group(e, minimum, out); return; }
                const bool quantity = e->args.size() >= 2 && (e->args[0]->op == Op::Number || e->args[0]->op == Op::Symbol) && isUnitExpr(e->args.back());
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    if (i != 0)
                    {
                        if (quantity && i + 1 == e->args.size()) out.push_back(atom(Kind::Symbol, kThinSpace));
                        else out.push_back(atom(Kind::Operator, kCdot));
                    }
                    group(e->args[i], (quantity && i + 1 == e->args.size()) ? Precedence::Mul : Precedence::Mul + 1, out);
                }
                return;
            }
            case Op::Div:
                out.push_back(note(makeFraction(ids, slot(e->args.at(0)), slot(e->args.at(1))), e));
                return;
            case Op::Pow:
            {
                std::vector<NodePtr> base;
                const auto& b = e->args.at(0);
                if (b != nullptr && (b->op == Op::Div || b->op == Op::Pow || precedenceOf(b) < Precedence::Atom)) group(b, Precedence::Atom + 1, base);
                else emit(b, Precedence::Atom, base);
                out.push_back(note(makeScript(ids, row(std::move(base)), nullptr, slot(e->args.at(1))), e));
                return;
            }
            case Op::Root:
            {
                const auto& index = e->args.at(1);
                const bool square = index != nullptr && index->op == Op::Number && index->value.isExact() && index->value.q == math_ir::Rational(2);
                out.push_back(note(makeRadical(ids, slot(e->args.at(0)), square ? nullptr : slot(index)), e));
                return;
            }
            case Op::Call:
            {
                if (e->name == "abs" && e->args.size() == 1)
                {
                    out.push_back(note(makeDelimited(ids, "|", "|", slot(e->args[0])), e));
                    return;
                }
                out.push_back(note(atom(Kind::Identifier, e->name, Style::Upright), e));
                std::vector<NodePtr> args;
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    if (i != 0) args.push_back(atom(Kind::Operator, ","));
                    emit(e->args[i], Precedence::Relation, args);
                }
                out.push_back(makeDelimited(ids, "(", ")", row(std::move(args))));
                return;
            }
            case Op::Sum: case Op::Product:
            {
                std::vector<NodePtr> lower { atom(Kind::Identifier, e->name), atom(Kind::Relation, "=") };
                emit(e->args.at(1), Precedence::Relation, lower);
                out.push_back(note(makeBigOperator(ids, e->op == Op::Sum ? "\xE2\x88\x91" : "\xE2\x88\x8F", row(std::move(lower)), slot(e->args.at(2))), e));
                group(e->args.at(0), Precedence::Mul, out);
                return;
            }
            case Op::Integral:
            {
                NodePtr lower = e->args.size() > 1 && e->args[1] != nullptr ? slot(e->args[1]) : nullptr;
                NodePtr upper = e->args.size() > 2 && e->args[2] != nullptr ? slot(e->args[2]) : nullptr;
                out.push_back(note(makeBigOperator(ids, "\xE2\x88\xAB", lower, upper), e));
                emit(e->args.at(0), Precedence::Relation, out);
                out.push_back(atom(Kind::Identifier, "d", Style::Upright));
                out.push_back(atom(Kind::Identifier, e->name));
                return;
            }
            case Op::Derivative:
            {
                const bool partial = e->cols != 0;
                auto d = [&] { return partial ? atom(Kind::Symbol, "\xE2\x88\x82") : atom(Kind::Identifier, "d", Style::Upright); };
                std::vector<NodePtr> numerator { d() }, denominator { d(), atom(Kind::Identifier, e->name) };
                group(e->args.at(0), Precedence::Atom, numerator);
                out.push_back(note(makeFraction(ids, row(std::move(numerator)), row(std::move(denominator))), e));
                return;
            }
            case Op::Index:
            {
                std::vector<NodePtr> sub;
                for (size_t i = 1; i < e->args.size(); ++i)
                {
                    if (i != 1) sub.push_back(atom(Kind::Operator, ","));
                    emit(e->args[i], Precedence::Relation, sub);
                }
                std::vector<NodePtr> base;
                emit(e->args.at(0), Precedence::Atom, base);
                out.push_back(note(makeScript(ids, row(std::move(base)), row(std::move(sub)), nullptr), e));
                return;
            }
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: case Op::Approx:
            {
                static const char* const symbols[] = { "=", "\xE2\x89\xA0", "<", "\xE2\x89\xA4", ">", "\xE2\x89\xA5", "\xE2\x89\x88" };
                const char* symbol = symbols[(int) e->op - (int) Op::Eq];
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    if (i != 0) out.push_back(atom(Kind::Relation, symbol));
                    emit(e->args[i], Precedence::Add, out);
                }
                return;
            }
            case Op::Define:
                emit(e->args.at(0), Precedence::Atom, out);
                out.push_back(atom(Kind::Relation, kAssign));
                emit(e->args.at(1), Precedence::Add, out);
                return;
            case Op::Vector: case Op::Matrix: case Op::Tuple:
            {
                std::vector<NodePtr> body;
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    if (i != 0) body.push_back(atom(Kind::Operator, (e->op == Op::Matrix && e->cols > 0 && (int) i % e->cols == 0) ? ";" : ","));
                    emit(e->args[i], Precedence::Relation, body);
                }
                out.push_back(note(makeDelimited(ids, e->op == Op::Tuple ? "(" : "[", e->op == Op::Tuple ? ")" : "]", row(std::move(body))), e));
                return;
            }
            case Op::System:
            {
                // a system continues on several lines; as one row its equations are joined by a semicolon
                for (size_t i = 0; i < e->args.size(); ++i)
                {
                    if (i != 0) out.push_back(atom(Kind::Operator, ";"));
                    emit(e->args[i], Precedence::Relation, out);
                }
                return;
            }
            default:
                tree.problem = std::string("The expression kind '") + math_ir::opName(e->op) + "' cannot be displayed yet.";
                out.push_back(atom(Kind::Text, math_ir::opName(e->op)));
                return;
        }
    }

    NodePtr placeholderNode() { return makePlaceholder(ids); }

    void scalarOf(const math_ir::ExprPtr& e, std::vector<NodePtr>& out)
    {
        const auto& s = e->value;
        std::vector<NodePtr> local;
        if (s.isExact()) rational(local, s.q);
        else if (s.domain == math_ir::Domain::Real && !s.text.empty()) local.push_back(atom(Kind::Number, s.text));
        else if (s.domain == math_ir::Domain::Real) real(local, s.re, 15);
        else { real(local, s.re, 15); local.push_back(atom(Kind::Operator, s.im < 0 ? kMinus : "+")); real(local, std::abs(s.im), 15); local.push_back(atom(Kind::Identifier, "i", Style::Upright)); }
        for (auto& n : local) out.push_back(n);
        if (!local.empty()) note(local.back(), e);
    }

private:
    IdAllocator& ids;
};
}

Tree fromExpr(const math_ir::ExprPtr& expr, IdAllocator& ids)
{
    Builder b(ids);
    if (expr != nullptr && expr->op == Op::System)
    {
        for (const auto& line : expr->args)
        {
            std::vector<NodePtr> items;
            b.emit(line, Precedence::Relation, items);
            b.tree.lines.push_back(b.row(std::move(items)));
        }
    }
    else
    {
        std::vector<NodePtr> items;
        b.emit(expr, Precedence::Relation, items);
        b.tree.lines.push_back(b.row(std::move(items)));
    }
    return std::move(b.tree);
}

Tree fromValue(const math_value::Value& value, IdAllocator& ids, const math_ir::Source& origin, const Options& options)
{
    Builder b(ids);
    if (value.kind == math_value::Kind::Symbolic && value.expr != nullptr && value.expr->op == Op::System)
        return fromExpr(value.expr, ids);
    std::vector<NodePtr> items;
    b.value(items, value, options);
    auto line = b.row(std::move(items));
    b.tree.lines.push_back(line);
    if (origin.valid()) b.tree.derivedFrom[line->id] = origin;
    return std::move(b.tree);
}
}
