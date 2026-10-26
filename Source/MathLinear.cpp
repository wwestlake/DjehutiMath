#include "MathLinear.h"

#include <algorithm>
#include <map>

namespace math_linear
{
using namespace math_doc;

namespace
{
struct Named { const char* name; const char* glyph; };

const Named kGreek[] = {
    { "alpha", "\xCE\xB1" }, { "beta", "\xCE\xB2" }, { "gamma", "\xCE\xB3" }, { "delta", "\xCE\xB4" }, { "epsilon", "\xCE\xB5" },
    { "zeta", "\xCE\xB6" }, { "eta", "\xCE\xB7" }, { "theta", "\xCE\xB8" }, { "iota", "\xCE\xB9" }, { "kappa", "\xCE\xBA" },
    { "lambda", "\xCE\xBB" }, { "mu", "\xCE\xBC" }, { "nu", "\xCE\xBD" }, { "xi", "\xCE\xBE" }, { "omicron", "\xCE\xBF" },
    { "pi", "\xCF\x80" }, { "rho", "\xCF\x81" }, { "sigma", "\xCF\x83" }, { "tau", "\xCF\x84" }, { "upsilon", "\xCF\x85" },
    { "phi", "\xCF\x86" }, { "chi", "\xCF\x87" }, { "psi", "\xCF\x88" }, { "omega", "\xCF\x89" },
    { "Gamma", "\xCE\x93" }, { "Delta", "\xCE\x94" }, { "Theta", "\xCE\x98" }, { "Lambda", "\xCE\x9B" }, { "Xi", "\xCE\x9E" },
    { "Pi", "\xCE\xA0" }, { "Sigma", "\xCE\xA3" }, { "Upsilon", "\xCE\xA5" }, { "Phi", "\xCE\xA6" }, { "Psi", "\xCE\xA8" },
    { "Omega", "\xCE\xA9" },
};

// word -> symbol / operator / relation
struct WordSymbol { const char* name; const char* glyph; Kind kind; };
const WordSymbol kWordSymbols[] = {
    { "infty", "\xE2\x88\x9E", Kind::Symbol }, { "inf", "\xE2\x88\x9E", Kind::Symbol }, { "partial", "\xE2\x88\x82", Kind::Symbol },
    { "nabla", "\xE2\x88\x87", Kind::Symbol }, { "hbar", "\xE2\x84\x8F", Kind::Identifier }, { "ell", "\xE2\x84\x93", Kind::Identifier },
    { "forall", "\xE2\x88\x80", Kind::Symbol }, { "exists", "\xE2\x88\x83", Kind::Symbol }, { "degree", "\xC2\xB0", Kind::Symbol },
    { "pm", "\xC2\xB1", Kind::Operator }, { "mp", "\xE2\x88\x93", Kind::Operator }, { "times", "\xC3\x97", Kind::Operator },
    { "cdot", "\xE2\x8B\x85", Kind::Operator }, { "div", "\xC3\xB7", Kind::Operator },
    { "in", "\xE2\x88\x88", Kind::Relation }, { "notin", "\xE2\x88\x89", Kind::Relation }, { "subset", "\xE2\x8A\x82", Kind::Relation },
    { "subseteq", "\xE2\x8A\x86", Kind::Relation }, { "approx", "\xE2\x89\x88", Kind::Relation }, { "neq", "\xE2\x89\xA0", Kind::Relation },
    { "leq", "\xE2\x89\xA4", Kind::Relation }, { "geq", "\xE2\x89\xA5", Kind::Relation }, { "propto", "\xE2\x88\x9D", Kind::Relation },
    { "to", "\xE2\x86\x92", Kind::Relation }, { "implies", "\xE2\x87\x92", Kind::Relation }, { "iff", "\xE2\x87\x94", Kind::Relation },
    { "equiv", "\xE2\x89\xA1", Kind::Relation },
};

const char* kFunctions[] = { "sin", "cos", "tan", "cot", "sec", "csc", "asin", "acos", "atan", "arcsin", "arccos", "arctan", "sinh", "cosh",
                             "tanh", "coth", "ln", "log", "lg", "exp", "min", "max", "det", "lim", "sup", "inf_", "dim", "ker", "gcd", "arg",
                             "mod", "sgn", "Re", "Im", "tr", "rank", "deg" };

struct BigOp { const char* name; const char* glyph; };
const BigOp kBigOps[] = { { "sum", "\xE2\x88\x91" }, { "prod", "\xE2\x88\x8F" }, { "int", "\xE2\x88\xAB" }, { "iint", "\xE2\x88\xAC" },
                          { "oint", "\xE2\x88\xAE" }, { "coprod", "\xE2\x88\x90" } };

const char* greekName(const std::string& glyph)
{
    for (const auto& g : kGreek)
        if (glyph == g.glyph)
            return g.name;
    return nullptr;
}

bool isRelationChar(juce::juce_wchar c)
{
    switch (c)
    {
        case 0x2208: case 0x2209: case 0x220B: case 0x2282: case 0x2283: case 0x2286: case 0x2287: case 0x2248: case 0x2260:
        case 0x2264: case 0x2265: case 0x2261: case 0x221D: case 0x2192: case 0x21D2: case 0x21D4: case 0x2194: case 0x2190:
        case 0x226A: case 0x226B: case 0x223C: case 0x2254: case 0x2245: case 0x2243: case 0x22A5: case 0x2225: case 0x2223:
            return true;
        default: return false;
    }
}

bool isOperatorChar(juce::juce_wchar c)
{
    switch (c)
    {
        case 0x2212: case 0x00B1: case 0x2213: case 0x00D7: case 0x00F7: case 0x22C5: case 0x2217: case 0x2218: case 0x2227:
        case 0x2228: case 0x2229: case 0x222A: case 0x2216: case 0x2295: case 0x2297: case 0x2020:
            return true;
        default: return false;
    }
}

std::string utf8(juce::juce_wchar c) { return juce::String::charToString(c).toStdString(); }

class Parser
{
public:
    Parser(const juce::String& text, IdAllocator& ids, std::vector<std::string>& notes) : s(text), ids(ids), notes(notes) {}

    NodePtr run()
    {
        bool dummy = false;
        auto items = parseItems({}, dummy);
        return makeRow(ids, std::move(items));
    }

private:
    const juce::String s;
    int pos = 0;
    IdAllocator& ids;
    std::vector<std::string>& notes;

    bool eof() { skipSpace(); return pos >= s.length(); }
    void skipSpace() { while (pos < s.length() && juce::CharacterFunctions::isWhitespace(s[pos])) ++pos; }
    juce::juce_wchar peek(int ahead = 0) const { return pos + ahead < s.length() ? s[pos + ahead] : 0; }
    void note(const std::string& message) { if (std::find(notes.begin(), notes.end(), message) == notes.end()) notes.push_back(message); }
    NodePtr atom(Kind kind, const std::string& text, Style style = Style::Auto) { return makeAtom(ids, kind, text, style); }
    NodePtr rowOf(std::vector<NodePtr> items) { return items.empty() ? placeholderRow(ids) : makeRow(ids, std::move(items)); }

    static bool isAsciiLetter(juce::juce_wchar c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    static bool isDigit(juce::juce_wchar c) { return c >= '0' && c <= '9'; }

    // Items until one of `closers` (consumed; reported in `closed`) or the end of the text.
    std::vector<NodePtr> parseItems(const juce::String& closers, bool& closed)
    {
        std::vector<NodePtr> items;
        closed = false;
        for (;;)
        {
            skipSpace();
            if (pos >= s.length())
                return items;
            const auto c = peek();
            if (closers.containsChar(c))
            {
                ++pos;
                closed = true;
                lastCloser = c;
                return items;
            }
            parseOne(items);
        }
    }
    juce::juce_wchar lastCloser = 0;

    void parseOne(std::vector<NodePtr>& items)
    {
        const auto c = peek();
        if (c == '/') { ++pos; buildFraction(items); return; }
        if (c == '^') { ++pos; attachScript(items, 1, parseScriptArgument()); return; }
        if (c == '_') { ++pos; attachScript(items, 0, parseScriptArgument()); return; }
        if (c == '\'')
        {
            ++pos;
            attachScript(items, 1, makeRow(ids, { atom(Kind::Symbol, "\xE2\x80\xB2") }));
            return;
        }
        if (parseAtomic(items))
            return;
        parseOperator(items);
    }

    // One atomic item (a number, a name, a group, a structure call). False when the next token is not one.
    bool parseAtomic(std::vector<NodePtr>& out)
    {
        skipSpace();
        if (pos >= s.length())
            return false;
        const auto c = peek();
        if (isDigit(c) || (c == '.' && isDigit(peek(1))))
        {
            int start = pos;
            bool dot = false;
            while (pos < s.length() && (isDigit(peek()) || (peek() == '.' && !dot && isDigit(peek(1)))))
            {
                if (peek() == '.') dot = true;
                ++pos;
            }
            if (!out.empty() && out.back()->kind == Kind::Number)
                note("Two numbers are written next to each other; they will look like one number. Put an operator between them.");
            out.push_back(atom(Kind::Number, s.substring(start, pos).toStdString()));
            return true;
        }
        if (isAsciiLetter(c))
        {
            int start = pos;
            while (pos < s.length() && isAsciiLetter(peek())) ++pos;
            handleWord(s.substring(start, pos).toStdString(), out);
            return true;
        }
        if (c == '(' || c == '[' || c == '{' || c == '|')
        {
            out.push_back(parseGroup(c));
            return true;
        }
        if (c == '"')
        {
            ++pos;
            const int start = pos;
            while (pos < s.length() && peek() != '"') ++pos;
            const auto content = s.substring(start, pos).toStdString();
            if (pos < s.length()) ++pos; else note("A quotation mark was never closed.");
            if (!content.empty()) out.push_back(atom(Kind::Text, content));
            return true;
        }
        if (c == '?')
        {
            ++pos;
            out.push_back(makePlaceholder(ids));
            return true;
        }
        if (c >= 0x80)
        {
            ++pos;
            classifyUnicode(c, out);
            return true;
        }
        return false;
    }

    void classifyUnicode(juce::juce_wchar c, std::vector<NodePtr>& out)
    {
        const auto text = utf8(c);
        if (isRelationChar(c)) out.push_back(atom(Kind::Relation, text));
        else if (isOperatorChar(c)) out.push_back(atom(Kind::Operator, text));
        else if (c == 0x2211 || c == 0x220F || c == 0x222B || c == 0x222C || c == 0x222E || c == 0x2210)
            out.push_back(makeBigOperator(ids, text, nullptr, nullptr));
        else if (juce::CharacterFunctions::isLetter(c)) out.push_back(atom(Kind::Identifier, text));
        else out.push_back(atom(Kind::Symbol, text));
    }

    void handleWord(const std::string& word, std::vector<NodePtr>& out)
    {
        for (const auto& g : kGreek)
            if (word == g.name) { out.push_back(atom(Kind::Identifier, g.glyph)); return; }
        for (const auto& w : kWordSymbols)
            if (word == w.name) { out.push_back(atom(w.kind, w.glyph)); return; }
        for (const auto& b : kBigOps)
            if (word == b.name) { out.push_back(makeBigOperator(ids, b.glyph, nullptr, nullptr)); return; }
        if (word == "sqrt") { out.push_back(parseRadical(false)); return; }
        if (word == "root") { out.push_back(parseRadical(true)); return; }
        if (word == "abs") { out.push_back(parseCall("|", "|")); return; }
        if (word == "frac")
        {
            skipSpace();
            if (peek() == '(')
            {
                ++pos;
                bool closed = false;
                auto numerator = parseItems(",)", closed);
                if (closed && lastCloser == ',')
                {
                    auto denominator = parseItems(")", closed);
                    if (!closed) note("A bracket was never closed.");
                    out.push_back(makeFraction(ids, rowOf(std::move(numerator)), rowOf(std::move(denominator))));
                    return;
                }
                note("frac needs two arguments: frac(a, b).");
                out.push_back(makeFraction(ids, rowOf(std::move(numerator)), placeholderRow(ids)));
                return;
            }
            note("frac needs two arguments: frac(a, b).");
            out.push_back(makeFraction(ids, placeholderRow(ids), placeholderRow(ids)));
            return;
        }
        if (isFunctionName(word)) { out.push_back(atom(Kind::Identifier, word, Style::Upright)); return; }
        out.push_back(atom(Kind::Identifier, word));
    }

    NodePtr parseCall(const char* open, const char* close)
    {
        skipSpace();
        if (peek() != '(')
        {
            note("A function needs its argument in parentheses.");
            return makeDelimited(ids, open, close, placeholderRow(ids));
        }
        ++pos;
        bool closed = false;
        auto inner = parseItems(")", closed);
        if (!closed) note("A bracket was never closed.");
        return makeDelimited(ids, open, close, rowOf(std::move(inner)));
    }

    NodePtr parseRadical(bool withIndex)
    {
        skipSpace();
        NodePtr index;
        std::vector<NodePtr> radicand;
        if (peek() == '(')
        {
            ++pos;
            bool closed = false;
            if (withIndex)
            {
                auto first = parseItems(",)", closed);
                if (closed && lastCloser == ',')
                {
                    index = rowOf(std::move(first));
                    radicand = parseItems(")", closed);
                }
                else
                {
                    note("root needs an index and an expression: root(3, x).");
                    radicand = std::move(first);
                }
            }
            else
                radicand = parseItems(")", closed);
            if (!closed) note("A bracket was never closed.");
        }
        else
            radicand = parsePrimary(true);
        return makeRadical(ids, rowOf(std::move(radicand)), index);
    }

    // One factor with its scripts; a parenthesised group is unwrapped when `strip`.
    std::vector<NodePtr> parsePrimary(bool strip, bool withScripts = true)
    {
        std::vector<NodePtr> out;
        skipSpace();
        if (pos >= s.length())
            return out;
        if (!parseAtomic(out))
            return out;
        if (strip && out.size() == 1 && out[0]->kind == Kind::Delimited && out[0]->text == "(" && out[0]->text2 == ")")
        {
            auto body = out[0]->slots[0]->items;
            return body;
        }
        if (withScripts) parseScripts(out);
        return out;
    }

    void parseScripts(std::vector<NodePtr>& out)
    {
        for (;;)
        {
            const auto c = peek();
            if (c == '^') { ++pos; attachScript(out, 1, parseScriptArgument()); }
            else if (c == '_') { ++pos; attachScript(out, 0, parseScriptArgument()); }
            else if (c == '\'') { ++pos; attachScript(out, 1, makeRow(ids, { atom(Kind::Symbol, "\xE2\x80\xB2") })); }
            else return;
        }
    }

    NodePtr parseScriptArgument()
    {
        skipSpace();
        std::vector<NodePtr> items;
        if (peek() == '+' || peek() == '-')
        {
            items.push_back(atom(Kind::Operator, peek() == '-' ? "\xE2\x88\x92" : "+"));
            ++pos;
        }
        // a script argument takes no scripts of its own: x_i^2 is one base with two scripts (write x^(2^3) to nest)
        auto primary = parsePrimary(true, false);
        for (auto& p : primary) items.push_back(std::move(p));
        if (items.empty())
        {
            note("A superscript or subscript is missing its expression.");
            return placeholderRow(ids);
        }
        return makeRow(ids, std::move(items));
    }

    // which: 0 = subscript / lower limit, 1 = superscript / upper limit
    void attachScript(std::vector<NodePtr>& items, int which, NodePtr argument)
    {
        const bool attachable = !items.empty() && items.back()->kind != Kind::Operator && items.back()->kind != Kind::Relation;
        if (!attachable)
        {
            note("A superscript or subscript has nothing to attach to.");
            std::vector<NodePtr> slots { placeholderRow(ids), nullptr, nullptr };
            slots[(size_t) which + 1] = std::move(argument);
            items.push_back(withSlots(makeScript(ids, nullptr, nullptr, nullptr), std::move(slots)));
            return;
        }
        auto base = items.back();
        if (base->kind == Kind::BigOperator && base->slots[(size_t) which] == nullptr)
        {
            auto slots = base->slots;
            slots[(size_t) which] = std::move(argument);
            items.back() = withSlots(base, std::move(slots));
            return;
        }
        if (base->kind == Kind::Script && base->slots[(size_t) which + 1] == nullptr)
        {
            auto slots = base->slots;
            slots[(size_t) which + 1] = std::move(argument);
            items.back() = withSlots(base, std::move(slots));
            return;
        }
        items.pop_back();
        NodePtr sub = which == 0 ? argument : nullptr, sup = which == 1 ? argument : nullptr;
        items.push_back(makeScript(ids, makeRow(ids, { base }), sub, sup));
    }

    void buildFraction(std::vector<NodePtr>& items)
    {
        // the factors just before the slash: no operator or relation between them
        size_t start = items.size();
        while (start > 0)
        {
            const auto kind = items[start - 1]->kind;
            if (kind == Kind::Operator || kind == Kind::Relation || kind == Kind::BigOperator)
                break;
            --start;
        }
        std::vector<NodePtr> numerator(items.begin() + (long) start, items.end());
        items.erase(items.begin() + (long) start, items.end());
        if (numerator.size() == 1 && numerator[0]->kind == Kind::Delimited && numerator[0]->text == "(" && numerator[0]->text2 == ")")
        {
            auto inner = numerator[0]->slots[0]->items;   // copy first: the assignment releases the group that owns these items
            numerator = std::move(inner);
        }
        std::vector<NodePtr> denominator;
        skipSpace();
        if (peek() == '-' || peek() == '+')
        {
            denominator.push_back(atom(Kind::Operator, peek() == '-' ? "\xE2\x88\x92" : "+"));
            ++pos;
        }
        for (auto& item : parsePrimary(true)) denominator.push_back(std::move(item));
        if (denominator.size() == 1 && denominator[0]->kind == Kind::Operator) denominator.clear();
        if (numerator.empty()) note("A fraction is missing its numerator.");
        if (denominator.empty()) note("A fraction is missing its denominator.");
        items.push_back(makeFraction(ids, rowOf(std::move(numerator)), rowOf(std::move(denominator))));
    }

    NodePtr parseGroup(juce::juce_wchar open)
    {
        ++pos;
        const juce::juce_wchar close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : '|';
        bool closed = false;
        auto inner = parseItems(juce::String::charToString(close), closed);
        if (!closed) note("A bracket was never closed.");
        return makeDelimited(ids, utf8(open), closed ? utf8(close) : std::string(), makeRow(ids, std::move(inner)));
    }

    void parseOperator(std::vector<NodePtr>& items)
    {
        const auto c = peek();
        const auto c1 = peek(1);
        auto two = [&](juce::juce_wchar second) { return c1 == second; };
        auto take = [&](int n, Kind kind, const char* glyph) { pos += n; items.push_back(atom(kind, glyph)); };
        switch (c)
        {
            case '+': take(1, Kind::Operator, "+"); return;
            case '-':
                if (two('>')) { take(2, Kind::Relation, "\xE2\x86\x92"); return; }
                take(1, Kind::Operator, "\xE2\x88\x92"); return;
            case '*': take(1, Kind::Operator, "\xE2\x8B\x85"); return;
            case '=':
                if (two('>')) { take(2, Kind::Relation, "\xE2\x87\x92"); return; }
                if (two('=')) { take(2, Kind::Relation, "="); return; }
                take(1, Kind::Relation, "="); return;
            case '<':
                if (two('=')) { take(2, Kind::Relation, "\xE2\x89\xA4"); return; }
                if (two('<')) { take(2, Kind::Relation, "\xE2\x89\xAA"); return; }
                if (two('-') && peek(2) == '>') { take(3, Kind::Relation, "\xE2\x86\x94"); return; }
                take(1, Kind::Relation, "<"); return;
            case '>':
                if (two('=')) { take(2, Kind::Relation, "\xE2\x89\xA5"); return; }
                if (two('>')) { take(2, Kind::Relation, "\xE2\x89\xAB"); return; }
                take(1, Kind::Relation, ">"); return;
            case '!':
                if (two('=')) { take(2, Kind::Relation, "\xE2\x89\xA0"); return; }
                take(1, Kind::Symbol, "!"); return;
            case '~':
                if (two('=') || two('~')) { take(2, Kind::Relation, "\xE2\x89\x88"); return; }
                take(1, Kind::Relation, "\xE2\x88\xBC"); return;
            case ':':
                if (two('=')) { take(2, Kind::Relation, "\xE2\x89\x94"); return; }
                take(1, Kind::Operator, ":"); return;
            case ',': take(1, Kind::Operator, ","); return;
            case ';': take(1, Kind::Operator, ";"); return;
            case '%': take(1, Kind::Symbol, "%"); return;
            case '&': take(1, Kind::Symbol, "&"); return;
            case ')': case ']': case '}':
                note("A closing bracket has no matching opening bracket.");
                take(1, Kind::Symbol, utf8(c).c_str()); return;
            default:
                note("The character '" + utf8(c) + "' is kept as a plain symbol.");
                take(1, Kind::Symbol, utf8(c).c_str()); return;
        }
    }
};

// ---- the inverse ---------------------------------------------------------------------------------------------------------------
std::string linearItems(const std::vector<NodePtr>& items);
std::string linear(const NodePtr& node);

std::string group(const NodePtr& row)
{
    if (row == nullptr)
        return "?";
    if (row->items.size() == 1)
    {
        const auto& only = row->items[0];
        if (only->kind != Kind::Operator && only->kind != Kind::Relation)
            return linear(only);
    }
    return "(" + linearItems(row->items) + ")";
}

std::string scriptArgument(const NodePtr& row)
{
    if (row != nullptr && row->items.size() == 1)
    {
        const auto& only = row->items[0];
        if (only->kind == Kind::Number || (only->kind == Kind::Identifier && juce::String(only->text).length() == 1 && !greekName(only->text)))
            return only->text;
    }
    return "(" + linearItems(row == nullptr ? std::vector<NodePtr>() : row->items) + ")";
}

std::string linear(const NodePtr& node)
{
    if (node == nullptr)
        return {};
    switch (node->kind)
    {
        case Kind::Row: return linearItems(node->items);
        case Kind::Number: return node->text;
        case Kind::Identifier: if (const auto* g = greekName(node->text)) return g; return node->text;
        case Kind::Operator:
            if (node->text == "\xE2\x88\x92") return "-";
            if (node->text == "\xE2\x8B\x85") return "*";
            return node->text;
        case Kind::Relation:
            if (node->text == "\xE2\x89\xA4") return "<=";
            if (node->text == "\xE2\x89\xA5") return ">=";
            if (node->text == "\xE2\x89\xA0") return "!=";
            if (node->text == "\xE2\x89\x88") return "~=";
            if (node->text == "\xE2\x86\x92") return "->";
            if (node->text == "\xE2\x87\x92") return "=>";
            if (node->text == "\xE2\x89\x94") return ":=";
            return node->text;
        case Kind::Symbol: return node->text;
        case Kind::Text: return "\"" + node->text + "\"";
        case Kind::Placeholder: return "?";
        case Kind::Fraction: return group(node->slots[0]) + "/" + group(node->slots[1]);
        case Kind::Radical:
            if (node->slots[1] != nullptr) return "root(" + linearItems(node->slots[1]->items) + ", " + linearItems(node->slots[0]->items) + ")";
            return "sqrt(" + linearItems(node->slots[0]->items) + ")";
        case Kind::Script:
        {
            std::string out = group(node->slots[0]);
            if (node->slots[1] != nullptr) out += "_" + scriptArgument(node->slots[1]);
            if (node->slots[2] != nullptr)
            {
                const auto& sup = node->slots[2];
                const bool prime = sup->items.size() == 1 && sup->items[0]->kind == Kind::Symbol && sup->items[0]->text == "\xE2\x80\xB2";
                out += prime ? std::string("'") : "^" + scriptArgument(sup);
            }
            return out;
        }
        case Kind::Delimited:
            return node->text + linearItems(node->slots[0]->items) + node->text2;
        case Kind::BigOperator:
        {
            std::string name = node->text;
            for (const auto& b : kBigOps)
                if (node->text == b.glyph) name = b.name;
            if (node->slots[0] != nullptr) name += "_" + scriptArgument(node->slots[0]);
            if (node->slots[1] != nullptr) name += "^" + scriptArgument(node->slots[1]);
            return name;
        }
    }
    return {};
}

std::string linearItems(const std::vector<NodePtr>& items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0) out += " ";
        out += linear(items[i]);
    }
    return out;
}
}

bool isFunctionName(const std::string& name)
{
    for (const auto* f : kFunctions)
        if (name == f)
            return true;
    return false;
}

NodePtr parseLine(const juce::String& text, IdAllocator& ids, std::vector<std::string>& notes)
{
    Parser parser(text, ids, notes);
    return parser.run();
}

std::vector<NodePtr> parseCell(const juce::String& text, Role role, IdAllocator& ids, std::vector<std::string>& notes)
{
    std::vector<NodePtr> lines;
    if (role == Role::System)
    {
        for (const auto& line : juce::StringArray::fromLines(text))
            if (lines.size() < (size_t) kMaxLinesPerCell)
                lines.push_back(parseLine(line, ids, notes));
        if (lines.empty())
            lines.push_back(makeRow(ids));
    }
    else
        lines.push_back(parseLine(text.replaceCharacters("\r\n", "  "), ids, notes));
    return lines;
}

std::string toLinear(const NodePtr& row) { return row == nullptr ? std::string() : linearItems(row->items); }

std::string toLinear(const std::vector<NodePtr>& lines)
{
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (i > 0) out += "\n";
        out += toLinear(lines[i]);
    }
    return out;
}
}
