#include "MathUnits.h"

#include <cmath>
#include <map>

namespace math_units
{
using namespace math_ir;

namespace
{
Dim dimOf(int length, int mass, int time, int current, int temperature, int amount, int luminous)
{
    Dim d;
    const int v[7] = { length, mass, time, current, temperature, amount, luminous };
    for (int i = 0; i < 7; ++i) d.e[(size_t) i] = Rational(v[i]);
    return d;
}

const std::map<std::string, UnitInfo>& table()
{
    static const std::map<std::string, UnitInfo> units = [] {
        std::map<std::string, UnitInfo> t;
        auto add = [&t](const char* symbol, const Dim& dim, double scale) { t[symbol] = { symbol, dim, scale }; };
        add("m", dimOf(1, 0, 0, 0, 0, 0, 0), 1.0);
        add("g", dimOf(0, 1, 0, 0, 0, 0, 0), 1e-3);
        add("s", dimOf(0, 0, 1, 0, 0, 0, 0), 1.0);
        add("A", dimOf(0, 0, 0, 1, 0, 0, 0), 1.0);
        add("K", dimOf(0, 0, 0, 0, 1, 0, 0), 1.0);
        add("mol", dimOf(0, 0, 0, 0, 0, 1, 0), 1.0);
        add("cd", dimOf(0, 0, 0, 0, 0, 0, 1), 1.0);
        add("Hz", dimOf(0, 0, -1, 0, 0, 0, 0), 1.0);
        add("N", dimOf(1, 1, -2, 0, 0, 0, 0), 1.0);
        add("Pa", dimOf(-1, 1, -2, 0, 0, 0, 0), 1.0);
        add("J", dimOf(2, 1, -2, 0, 0, 0, 0), 1.0);
        add("W", dimOf(2, 1, -3, 0, 0, 0, 0), 1.0);
        add("C", dimOf(0, 0, 1, 1, 0, 0, 0), 1.0);
        add("V", dimOf(2, 1, -3, -1, 0, 0, 0), 1.0);
        add("F", dimOf(-2, -1, 4, 2, 0, 0, 0), 1.0);
        const Dim ohm = dimOf(2, 1, -3, -2, 0, 0, 0);
        add("ohm", ohm, 1.0);
        add("Ohm", ohm, 1.0);
        add("\xCE\xA9", ohm, 1.0);          // U+03A9 GREEK CAPITAL OMEGA
        add("\xE2\x84\xA6", ohm, 1.0);      // U+2126 OHM SIGN
        add("S", dimOf(-2, -1, 3, 2, 0, 0, 0), 1.0);
        add("H", dimOf(2, 1, -2, -2, 0, 0, 0), 1.0);
        add("T", dimOf(0, 1, -2, -1, 0, 0, 0), 1.0);
        add("Wb", dimOf(2, 1, -2, -1, 0, 0, 0), 1.0);
        add("rad", Dim::dimensionless(), 1.0);
        add("sr", Dim::dimensionless(), 1.0);
        add("\xC2\xB0", Dim::dimensionless(), 0.017453292519943295);   // degree
        add("min", dimOf(0, 0, 1, 0, 0, 0, 0), 60.0);
        add("h", dimOf(0, 0, 1, 0, 0, 0, 0), 3600.0);
        add("L", dimOf(3, 0, 0, 0, 0, 0, 0), 1e-3);
        add("l", dimOf(3, 0, 0, 0, 0, 0, 0), 1e-3);
        add("eV", dimOf(2, 1, -2, 0, 0, 0, 0), 1.602176634e-19);
        add("bar", dimOf(-1, 1, -2, 0, 0, 0, 0), 1e5);
        return t;
    }();
    return units;
}

struct Prefix { const char* symbol; double factor; };
const Prefix kPrefixes[] = {
    { "G", 1e9 }, { "M", 1e6 }, { "k", 1e3 }, { "c", 1e-2 }, { "m", 1e-3 }, { "u", 1e-6 },
    { "\xC2\xB5", 1e-6 }, { "\xCE\xBC", 1e-6 }, { "n", 1e-9 }, { "p", 1e-12 }, { "f", 1e-15 },
};

bool prefixable(const std::string& base) { return base != "min" && base != "h" && base != "\xC2\xB0" && base != "rad" && base != "sr"; }

Type unitType(const Dim& dim)
{
    Type t;
    t.shape = Shape::Scalar; t.domain = Domain::Real; t.dim = dim; t.sign = Sign::Positive;
    return t;
}

struct UnitParser
{
    const std::string& s;
    const Source& src;
    size_t pos = 0;
    std::string error;

    bool atEnd() { skipSpace(); return pos >= s.size(); }
    void skipSpace() { while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos; }

    // Multiplication or division sign at the cursor (not consumed): 1 = multiply, 2 = divide, 0 = neither.
    int operatorAhead(size_t& width)
    {
        skipSpace();
        if (pos >= s.size()) return 0;
        const unsigned char c = (unsigned char) s[pos];
        if (c == '*') { width = 1; return 1; }
        if (c == '/') { width = 1; return 2; }
        if (c == 0xC2 && pos + 1 < s.size() && (unsigned char) s[pos + 1] == 0xB7) { width = 2; return 1; }      // middle dot
        if (c == 0xC3 && pos + 1 < s.size() && (unsigned char) s[pos + 1] == 0x97) { width = 2; return 1; }      // times
        if (c == 0xE2 && pos + 2 < s.size() && (unsigned char) s[pos + 1] == 0x8B && (unsigned char) s[pos + 2] == 0x85) { width = 3; return 1; }
        return 0;
    }

    bool startsAtom()
    {
        skipSpace();
        if (pos >= s.size()) return false;
        const char c = s[pos];
        return c != ')' && c != '^' && c != '*' && c != '/' && !(((unsigned char) c == 0xC2 && pos + 1 < s.size() && (unsigned char) s[pos + 1] == 0xB7));
    }

    bool fail(const std::string& message) { if (error.empty()) error = message; return false; }

    ExprPtr product(Dim& dim, double& scale)
    {
        ExprPtr left = term(dim, scale);
        if (left == nullptr) return nullptr;
        for (;;)
        {
            size_t width = 0;
            const int op = operatorAhead(width);
            if (op == 0)
            {
                if (!startsAtom()) break;
                // juxtaposition ("N m") multiplies
                Dim d2; double s2 = 1.0;
                auto right = term(d2, s2);
                if (right == nullptr) return nullptr;
                dim = dim * d2; scale *= s2;
                left = makeNode(Op::Mul, { left, right }, unitType(dim), src);
                continue;
            }
            pos += width;
            Dim d2; double s2 = 1.0;
            auto right = term(d2, s2);
            if (right == nullptr) return nullptr;
            if (op == 1) { dim = dim * d2; scale *= s2; left = makeNode(Op::Mul, { left, right }, unitType(dim), src); }
            else { dim = dim / d2; scale /= s2; left = makeNode(Op::Div, { left, right }, unitType(dim), src); }
        }
        return left;
    }

    ExprPtr term(Dim& dim, double& scale)
    {
        auto base = atom(dim, scale);
        if (base == nullptr) return nullptr;
        skipSpace();
        if (pos < s.size() && s[pos] == '^')
        {
            ++pos;
            skipSpace();
            bool paren = false;
            if (pos < s.size() && s[pos] == '(') { paren = true; ++pos; }
            bool negative = false;
            if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) { negative = s[pos] == '-'; ++pos; }
            std::int64_t n = 0, d = 1;
            const size_t digitsAt = pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && n < 1000000) n = n * 10 + (s[pos++] - '0');
            if (pos == digitsAt) { fail("A unit exponent must be a whole number or a fraction such as ^(1/2)."); return nullptr; }
            if (paren && pos < s.size() && s[pos] == '/')
            {
                ++pos; d = 0;
                const size_t denAt = pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9' && d < 1000000) d = d * 10 + (s[pos++] - '0');
                if (pos == denAt || d == 0) { fail("A unit exponent has an invalid denominator."); return nullptr; }
            }
            if (paren)
            {
                if (pos >= s.size() || s[pos] != ')') { fail("A unit exponent in parentheses is not closed."); return nullptr; }
                ++pos;
            }
            const Rational p(negative ? -n : n, d);
            dim = dim.power(p);
            scale = std::pow(scale, p.toDouble());
            std::vector<ExprPtr> args { base, makeNumber(Scalar::rational(p), [] { Type t; t.shape = Shape::Scalar; t.domain = Domain::Rational; t.dim = Dim::dimensionless(); return t; }(), src) };
            return makeNode(Op::Pow, std::move(args), unitType(dim), src);
        }
        return base;
    }

    ExprPtr atom(Dim& dim, double& scale)
    {
        skipSpace();
        if (pos >= s.size()) { fail("A unit is missing."); return nullptr; }
        if (s[pos] == '(')
        {
            ++pos;
            auto inner = product(dim, scale);
            if (inner == nullptr) return nullptr;
            skipSpace();
            if (pos >= s.size() || s[pos] != ')') { fail("A bracket in a unit is not closed."); return nullptr; }
            ++pos;
            return inner;
        }
        if (s[pos] == '1' && (pos + 1 >= s.size() || !(s[pos + 1] >= '0' && s[pos + 1] <= '9')))
        {
            ++pos;
            dim = Dim::dimensionless(); scale = 1.0;
            Type t; t.shape = Shape::Scalar; t.domain = Domain::Integer; t.dim = dim; t.sign = Sign::Positive;
            return makeNumber(Scalar::integer(1), t, src);
        }
        const size_t start = pos;
        while (pos < s.size())
        {
            const unsigned char c = (unsigned char) s[pos];
            if (c == ' ' || c == '*' || c == '/' || c == '^' || c == '(' || c == ')') break;
            if (c == 0xC2 && pos + 1 < s.size() && (unsigned char) s[pos + 1] == 0xB7) break;
            if (c == 0xC3 && pos + 1 < s.size() && (unsigned char) s[pos + 1] == 0x97) break;
            if (c == 0xE2 && pos + 2 < s.size() && (unsigned char) s[pos + 1] == 0x8B && (unsigned char) s[pos + 2] == 0x85) break;
            ++pos;
        }
        const auto symbol = s.substr(start, pos - start);
        UnitInfo info;
        if (symbol.empty() || !lookup(symbol, info))
        {
            fail("Unknown unit '" + symbol + "'.");
            return nullptr;
        }
        dim = info.dim; scale = info.scale;
        return makeUnit(symbol, info.scale, info.dim, src);
    }
};
}

bool lookup(const std::string& symbol, UnitInfo& out)
{
    const auto& units = table();
    const auto exact = units.find(symbol);
    if (exact != units.end()) { out = exact->second; return true; }
    for (const auto& prefix : kPrefixes)
    {
        const std::string p = prefix.symbol;
        if (symbol.size() > p.size() && symbol.compare(0, p.size(), p) == 0)
        {
            const auto base = units.find(symbol.substr(p.size()));
            if (base != units.end() && prefixable(base->first))
            {
                out = base->second;
                out.symbol = symbol;
                out.scale *= prefix.factor;
                return true;
            }
        }
    }
    return false;
}

std::vector<UnitInfo> catalogue()
{
    std::vector<UnitInfo> out;
    for (const auto& entry : table()) out.push_back(entry.second);
    return out;
}

Parsed parse(const std::string& text, const Source& src)
{
    Parsed result;
    UnitParser parser { text, src };
    Dim dim = Dim::dimensionless();
    double scale = 1.0;
    auto expr = parser.product(dim, scale);
    if (expr != nullptr && !parser.atEnd())
    {
        parser.fail("Unexpected '" + text.substr(parser.pos, 1) + "' in the unit.");
        expr = nullptr;
    }
    if (expr == nullptr)
    {
        result.error = parser.error.empty() ? "The unit could not be read." : parser.error;
        return result;
    }
    result.ok = true;
    result.dim = dim;
    result.scale = scale;
    result.expr = expr;
    return result;
}
}
