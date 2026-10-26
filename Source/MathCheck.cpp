#include "MathCheck.h"

#include "MathDoc.h"
#include "MathFont.h"
#include "MathLayout.h"
#include "MathLinear.h"
#include "WorksheetPanel.h"

#include <cmath>

namespace
{
using namespace math_doc;

struct Report
{
    juce::Array<juce::var> checks;
    bool passed = true;
    void add(bool ok, const juce::String& name, const juce::String& detail = {})
    {
        auto* value = new juce::DynamicObject();
        value->setProperty("name", name);
        value->setProperty("passed", ok);
        value->setProperty("detail", detail);
        checks.add(juce::var(value));
        passed = passed && ok;
    }
};

std::string shape(const NodePtr& n)
{
    if (n == nullptr) return "-";
    auto join = [](const std::vector<NodePtr>& items) {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) out += (i ? " " : "") + shape(items[i]);
        return out;
    };
    switch (n->kind)
    {
        case Kind::Row: return "[" + join(n->items) + "]";
        case Kind::Text: return "\"" + n->text + "\"";
        case Kind::Placeholder: return "?";
        case Kind::Fraction: return "frac(" + shape(n->slots[0]) + "|" + shape(n->slots[1]) + ")";
        case Kind::Radical: return "sqrt(" + shape(n->slots[0]) + (n->slots[1] ? "@" + shape(n->slots[1]) : std::string()) + ")";
        case Kind::Script: return "sc(" + shape(n->slots[0]) + "|" + shape(n->slots[1]) + "|" + shape(n->slots[2]) + ")";
        case Kind::Delimited: return n->text + shape(n->slots[0]) + (n->text2.empty() ? "\xE2\x88\x85" : n->text2);
        case Kind::BigOperator: return "big(" + n->text + "|" + shape(n->slots[0]) + "|" + shape(n->slots[1]) + ")";
        default: return n->text;
    }
}

std::vector<NodeId> idsOf(const NodePtr& root)
{
    std::vector<NodeId> ids;
    forEach(root, [&ids](const Node& n) { ids.push_back(n.id); });
    return ids;
}

NodePtr parse(const juce::String& text, IdAllocator& ids, std::vector<std::string>* notes = nullptr)
{
    std::vector<std::string> scratch;
    return math_linear::parseLine(text, ids, notes != nullptr ? *notes : scratch);
}

std::string shapeOf(const juce::String& text)
{
    IdAllocator ids;
    return shape(parse(text, ids));
}

juce::var cloneVar(const juce::var& v) { return juce::JSON::parse(juce::JSON::toString(v)); }

std::vector<const math_layout::Primitive*> glyphsOf(const math_layout::Rendered& r)
{
    std::vector<const math_layout::Primitive*> out;
    for (const auto& p : r.primitives) if (p.type == math_layout::Primitive::Type::Glyph) out.push_back(&p);
    return out;
}
std::vector<const math_layout::Primitive*> rulesOf(const math_layout::Rendered& r)
{
    std::vector<const math_layout::Primitive*> out;
    for (const auto& p : r.primitives) if (p.type == math_layout::Primitive::Type::Rule) out.push_back(&p);
    return out;
}

math_layout::Rendered layoutText(const juce::String& text, float size = 40.0f, bool display = true)
{
    IdAllocator ids;
    math_layout::Options o;
    o.fontSize = size;
    o.displayStyle = display;
    return math_layout::layoutRow(parse(text, ids), o);
}
}

bool mathCheck(const juce::File& destination)
{
    Report report;
    const auto work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("djehuti-math-math-check-" + juce::Uuid().toString());
    work.createDirectory();
    auto& font = math_font::MathFont::instance();

    // ---- the bundled font ----------------------------------------------------------------------------------------------
    report.add(font.ok(), "STIX Two Math loads (cmap, hmtx, MATH table, JUCE outlines)", font.error());
    if (!font.ok())
    {
        auto* root = new juce::DynamicObject();
        root->setProperty("passed", false);
        root->setProperty("checks", report.checks);
        destination.getParentDirectory().createDirectory();
        destination.replaceWithText(juce::JSON::toString(juce::var(root), false));
        return false;
    }
    {
        const auto& c = font.constants();
        const auto italicF = font.glyphFor(0x1D453), upright = font.glyphFor('H');
        const auto h = font.inkBounds(upright);
        const auto* paren = font.verticalConstruction(font.glyphFor('('));
        const auto* radical = font.verticalConstruction(font.glyphFor(0x221A));
        const auto* sum = font.verticalConstruction(font.glyphFor(0x2211));
        report.add(font.unitsPerEm() == 1000 && c.value[math_font::AxisHeight] > 200 && c.value[math_font::FractionRuleThickness] > 20
                       && c.scriptPercentScaleDown >= 60 && c.scriptPercentScaleDown <= 80 && c.displayOperatorMinHeight > 1000,
                   "MATH constants are read (axis height, rule thickness, script scale, display operator size)",
                   "axis " + juce::String(c.value[math_font::AxisHeight]) + ", rule " + juce::String(c.value[math_font::FractionRuleThickness])
                       + ", script " + juce::String(c.scriptPercentScaleDown) + "%, displayOp " + juce::String(c.displayOperatorMinHeight));
        report.add(italicF != 0 && font.advance(italicF) > 0 && font.italicCorrection(italicF) != 0,
                   "Non-BMP mathematical italic letters map to glyphs with an italic correction", "glyph " + juce::String(italicF));
        report.add(h.getY() < -0.55f && h.getY() > -0.85f && h.getBottom() > -0.02f && h.getBottom() < 0.05f,
                   "Glyph outlines are in em units with the baseline at zero (capital H is about 0.66 em tall)",
                   "top " + juce::String(h.getY(), 3) + " bottom " + juce::String(h.getBottom(), 3));
        report.add(paren != nullptr && paren->variants.size() >= 3 && radical != nullptr && radical->variants.size() >= 2
                       && sum != nullptr && sum->variants.size() >= 2 && !paren->parts.empty(),
                   "Stretchy size variants and glyph assemblies exist for parentheses, the radical sign and the summation sign",
                   paren != nullptr ? "( has " + juce::String((int) paren->variants.size()) + " variants and " + juce::String((int) paren->parts.size()) + " parts" : "none");
    }

    // ---- linear input becomes structure ------------------------------------------------------------------------------------
    struct Case { const char* input; const char* expect; };
    const Case cases[] = {
        { "(x^2+1)/(x+3)", "[frac([sc([x]|-|[2]) + 1]|[x + 3])]" },
        { "sqrt(x^2+1)", "[sqrt([sc([x]|-|[2]) + 1])]" },
        { "F=m*a", "[F = m \xE2\x8B\x85 a]" },
        { "f(x)=sqrt(x^2+1)/(x+3)", "[f ([x]) = frac([sqrt([sc([x]|-|[2]) + 1])]|[x + 3])]" },
        { "a+b/c", "[a + frac([b]|[c])]" },
        { "2x/3", "[frac([2 x]|[3])]" },
        { "(a+b)/c", "[frac([a + b]|[c])]" },
        { "1/2/3", "[frac([frac([1]|[2])]|[3])]" },
        { "x_i^2", "[sc([x]|[i]|[2])]" },
        { "x_(i+1)", "[sc([x]|[i + 1]|-)]" },
        { "e^(-x^2)", "[sc([e]|-|[\xE2\x88\x92 sc([x]|-|[2])])]" },
        { "sum_(i=1)^n i^2", "[big(\xE2\x88\x91|[i = 1]|[n]) sc([i]|-|[2])]" },
        { "int_0^1 x dx", "[big(\xE2\x88\xAB|[0]|[1]) x dx]" },
        { "alpha+Omega", "[\xCE\xB1 + \xCE\xA9]" },
        { "sin(x)", "[sin ([x])]" },
        { "f'(x)", "[sc([f]|-|[\xE2\x80\xB2]) ([x])]" },
        { "abs(x)", "[|[x]|]" },
        { "|x|", "[|[x]|]" },
        { "root(3, x)", "[sqrt([x]@[3])]" },
        { ".5+1", "[.5 + 1]" },
        { "a<=b", "[a \xE2\x89\xA4 b]" },
        { "x:=1", "[x \xE2\x89\x94 1]" },
        { "2*pi*r", "[2 \xE2\x8B\x85 \xCF\x80 \xE2\x8B\x85 r]" },
        { "dv/dt", "[frac([dv]|[dt])]" },
        { "1/", "[frac([1]|[?])]" },
        { "/2", "[frac([?]|[2])]" },
        { "x^", "[sc([x]|-|[?])]" },
        { "(a+b", "[([a + b]\xE2\x88\x85]" },
        { "sqrt(", "[sqrt([?])]" },
        { "x+", "[x +]" },
        { "1/-2", "[frac([1]|[\xE2\x88\x92 2])]" },
    };
    {
        juce::String failures;
        for (const auto& c : cases)
            if (shapeOf(c.input) != c.expect)
                failures << "\n" << c.input << "  =>  " << juce::String::fromUTF8(shapeOf(c.input).c_str()) << "  (wanted " << juce::String::fromUTF8(c.expect) << ")";
        report.add(failures.isEmpty(), "Linear notation produces the expected structures (fractions, roots, scripts, limits, delimiters, incomplete input)", failures);
    }
    {
        juce::String failures;
        int count = 0;
        for (const auto& c : cases)
        {
            IdAllocator a, b;
            const auto first = parse(c.input, a);
            const auto again = parse(juce::String::fromUTF8(math_linear::toLinear(first).c_str()), b);
            ++count;
            if (!sameStructure(first, again))
                failures << "\n" << c.input << " -> " << juce::String::fromUTF8(math_linear::toLinear(first).c_str()) << " => " << juce::String::fromUTF8(shape(again).c_str());
        }
        report.add(failures.isEmpty(), "Structure converts to linear text and back to the same structure (" + juce::String(count) + " expressions)", failures);
    }
    {
        std::vector<std::string> notes;
        IdAllocator ids;
        parse("1/", ids, &notes);
        parse("(a", ids, &notes);
        report.add(notes.size() >= 2, "Incomplete input is accepted with plain-language notes", notes.empty() ? "no notes" : juce::String(notes[0]));
    }

    // ---- identity and validation ---------------------------------------------------------------------------------------
    {
        IdAllocator ids;
        const auto row = parse("f(x)=sqrt(x^2+1)/(x+3)", ids);
        auto list = idsOf(row);
        std::set<NodeId> unique(list.begin(), list.end());
        juce::String why;
        report.add(unique.size() == list.size() && !unique.count(0) && validate(row, why), "Every node has a unique, nonzero identifier and the tree validates", why);
        std::set<NodeId> seen;
        IdAllocator reloaded;
        NodePtr back;
        const bool ok = fromVar(cloneVar(toVar(row)), reloaded, seen, back, why);
        report.add(ok && sameStructure(row, back) && idsOf(back) == list && reloaded.next > *std::max_element(list.begin(), list.end()),
                   "A tree survives serialisation with the same structure and the same node identifiers", why);
    }
    {
        IdAllocator ids;
        const auto before = parse("(x^2+1)/(x+3)", ids);
        const auto after = adoptIds(before, parse("(x^2+1)/(x+4)", ids));
        const bool same = idsOf(before) == idsOf(after) && !sameStructure(before, after);
        const auto b2 = parse("a+b", ids);
        const auto a2 = adoptIds(b2, parse("a+c+b", ids));
        bool keeps = a2->items.size() == 5 && a2->items[0]->id == b2->items[0]->id && a2->items[1]->id == b2->items[1]->id && a2->items[4]->id == b2->items[2]->id;
        const auto oldList = idsOf(b2);
        std::set<NodeId> oldIds(oldList.begin(), oldList.end());
        keeps = keeps && !oldIds.count(a2->items[2]->id) && !oldIds.count(a2->items[3]->id);
        report.add(same && keeps, "Editing keeps the identifiers of unchanged nodes and gives new nodes new ones", same ? "" : "ids changed");
    }
    {
        IdAllocator ids;
        const auto good = toVar(parse("x^2+1", ids));
        auto mutate = [&](const std::function<void(juce::DynamicObject&)>& change) {
            auto v = cloneVar(good);
            change(*v.getDynamicObject());
            IdAllocator a; std::set<NodeId> seen; NodePtr out; juce::String why;
            return fromVar(v, a, seen, out, why);
        };
        const bool dup = !mutate([](juce::DynamicObject& o) { auto* items = o.getProperty("c").getArray(); items->getReference(1).getDynamicObject()->setProperty("n", (*items)[0]["n"]); });
        const bool unknown = !mutate([](juce::DynamicObject& o) { o.getProperty("c").getArray()->getReference(1).getDynamicObject()->setProperty("k", "bogus"); });
        const bool zero = !mutate([](juce::DynamicObject& o) { o.setProperty("n", 0); });
        const bool badNumber = !mutate([](juce::DynamicObject& o) { auto& item = o.getProperty("c").getArray()->getReference(2); item.getDynamicObject()->setProperty("t", "12a"); });
        const bool rowInRow = !mutate([&](juce::DynamicObject& o) { auto* items = o.getProperty("c").getArray(); auto inner = cloneVar(good); inner.getDynamicObject()->setProperty("n", 9999); items->add(inner); });
        IdAllocator many; auto deep = parse("x", many);
        for (int i = 0; i < 80; ++i) deep = makeRow(many, { makeDelimited(many, "(", ")", deep) });
        IdAllocator fresh; std::set<NodeId> seen; NodePtr out; juce::String why;
        const bool depth = !fromVar(toVar(deep), fresh, seen, out, why) && why.contains("deep");
        const bool halfFraction = !mutate([](juce::DynamicObject& o) {
            auto& frac = o.getProperty("c").getArray()->getReference(0);
            frac.getDynamicObject()->setProperty("k", "frac"); frac.getDynamicObject()->removeProperty("t");
            frac.getDynamicObject()->setProperty("num", juce::JSON::parse("{\"n\":777,\"k\":\"row\",\"c\":[]}"));
        });
        report.add(dup && unknown && zero && badNumber && rowInRow && depth && halfFraction, "Malformed trees are rejected (duplicate or zero ids, unknown kinds, bad numbers, rows in rows, excess depth, missing slots)",
                   juce::String((int) dup) + juce::String((int) unknown) + juce::String((int) zero) + juce::String((int) badNumber) + juce::String((int) rowInRow) + juce::String((int) depth) + juce::String((int) halfFraction));
    }

    // ---- worksheet persistence -----------------------------------------------------------------------------------------------
    {
        WorksheetDocument doc;
        doc.reset();
        std::vector<std::string> notes;
        const auto equation = doc.newMathCell(Role::Equation, "F = m*a", notes);
        const auto incomplete = doc.newMathCell(Role::Expression, "1/", notes);
        const auto system = doc.newMathCell(Role::System, "F &= m*a\na &= dv/dt\nv(t) &= v_0 + a*t", notes);
        const auto file = work.getChildFile("v2.djmath");
        juce::String error;
        const bool saved = doc.save(file, error);
        const auto text = file.loadFileAsString();
        const auto parsed = juce::JSON::parse(text);
        report.add(saved && (int) parsed["version"] == 2 && (juce::int64) parsed["nextNodeId"] == (juce::int64) doc.nodeIds.next
                       && !text.contains("\\frac") && !text.contains("<html") && !text.contains("<math"),
                   "Version 2 stores the structured trees and the next node id, not LaTeX or HTML", error);
        WorksheetDocument reopened;
        const bool loaded = reopened.load(file, error);
        report.add(loaded && juce::JSON::toString(reopened.toJson()) == juce::JSON::toString(doc.toJson()) && reopened.nodeIds.next == doc.nodeIds.next,
                   "A saved worksheet reopens identical: cells, order, roles, node identifiers", error);
        const int idx = reopened.indexOf(incomplete), sx = reopened.indexOf(system);
        report.add(idx >= 0 && hasPlaceholder(reopened.cells[(size_t) idx].math.lines[0]) && sx >= 0 && reopened.cells[(size_t) sx].math.role == Role::System
                       && reopened.cells[(size_t) sx].math.lines.size() == 3 && reopened.cells[(size_t) reopened.indexOf(equation)].math.role == Role::Equation
                       && reopened.indexOf(equation) < reopened.indexOf(incomplete) && reopened.indexOf(incomplete) < reopened.indexOf(system),
                   "Placeholders, roles, three-line systems and cell ordering survive");

        // version 1 documents
        const auto v1 = juce::File(DJEHUTIMATH_SOURCE_ROOT).getChildFile("examples").getChildFile("ohms-law.djmath");
        WorksheetDocument old;
        const bool oldOk = old.load(v1, error);
        const auto oldFile = work.getChildFile("upgraded.djmath");
        const bool upgraded = oldOk && old.save(oldFile, error);
        WorksheetDocument upgradedBack;
        report.add(oldOk && old.cells.size() == 2 && old.cells[0].kind == "text" && old.cells[1].kind == "frust" && upgraded
                       && upgradedBack.load(oldFile, error) && upgradedBack.cells.size() == 2,
                   "A version 1 worksheet loads unchanged and saves as version 2", error);

        // rejections leave the current document alone
        const auto before = juce::JSON::toString(doc.toJson());
        auto reject = [&](const juce::String& body) {
            const auto f = work.getChildFile("bad.djmath");
            f.replaceWithText(body);
            juce::String why;
            const bool refused = !doc.load(f, why);
            return refused && juce::JSON::toString(doc.toJson()) == before;
        };
        auto corrupt = cloneVar(parsed);
        { auto* cells = corrupt["cells"].getArray(); auto& math = cells->getReference(1)["math"]; math.getDynamicObject()->getProperty("lines").getArray()->getReference(0).getDynamicObject()->setProperty("n", 0); }
        auto future = cloneVar(parsed); future.getDynamicObject()->setProperty("version", 3);
        const bool r1 = reject("{\"format\":\"djehuti-math-worksheet\",\"version\":1,\"cells\":[{\"id\":\"a\",\"kind\":\"math\",\"source\":\"x\"}]}");
        const bool r2 = reject(juce::JSON::toString(corrupt));
        const bool r3 = reject(juce::JSON::toString(future));
        const bool r4 = reject("not json");
        report.add(r1 && r2 && r3 && r4, "Math cells in a version 1 file, corrupt math, future versions and non-JSON are all rejected without touching the open worksheet",
                   juce::String((int) r1) + juce::String((int) r2) + juce::String((int) r3) + juce::String((int) r4));
        auto repaired = cloneVar(parsed); repaired.getDynamicObject()->setProperty("nextNodeId", 1);
        const auto repairedFile = work.getChildFile("repaired.djmath");
        repairedFile.replaceWithText(juce::JSON::toString(repaired));
        WorksheetDocument repair;
        report.add(repair.load(repairedFile, error) && repair.nodeIds.next > 20, "A stored next-node-id that is too low is repaired from the ids in use");

        // snapshots and revisions
        MathSnapshot snap;
        const bool got = doc.snapshot(equation, snap);
        const auto oldShape = shape(snap.lines[0]);
        const auto firstId = snap.lines[0]->items[0]->id;
        const auto revision = snap.cellRevision;
        doc.setMathSource(equation, "F = m*a + b", notes);
        MathSnapshot next;
        doc.snapshot(equation, next);
        report.add(got && shape(snap.lines[0]) == oldShape && next.cellRevision > revision && next.lines[0]->items[0]->id == firstId
                       && next.cellId == equation.toStdString() && next.role == Role::Equation && shape(next.lines[0]) != oldShape,
                   "A snapshot is immutable; the cell's revision advances with each edit; unchanged nodes keep their ids");
        // oversized or too deeply nested input is refused and leaves the cell as it was
        const auto beforeRevision = next.cellRevision;
        const bool refused = !doc.setMathSource(equation, juce::String::repeatedString("(", 100) + "x" + juce::String::repeatedString(")", 100), notes);
        MathSnapshot afterRefusal;
        doc.snapshot(equation, afterRefusal);
        report.add(refused && afterRefusal.cellRevision == beforeRevision && shape(afterRefusal.lines[0]) == shape(next.lines[0]), "Input too deeply nested is refused and the cell is unchanged");
        // role changes re-divide the text into lines
        doc.setMathRole(system, Role::Expression, notes);
        MathSnapshot oneLine; doc.snapshot(system, oneLine);
        doc.setMathRole(system, Role::System, notes);
        MathSnapshot threeLines; doc.snapshot(system, threeLines);
        report.add(oneLine.lines.size() == 1 && threeLines.lines.size() == 3 && oneLine.role == Role::Expression, "Changing a cell between System and a single line re-divides its text");
    }

    // ---- layout ---------------------------------------------------------------------------------------------------------------
    {
        const auto frac = layoutText("1/2");
        const auto g = glyphsOf(frac);
        const auto r = rulesOf(frac);
        const float axis = (float) font.constants().value[math_font::AxisHeight] * 40.0f / 1000.0f;
        const float thickness = (float) font.constants().value[math_font::FractionRuleThickness] * 40.0f / 1000.0f;
        bool ok = g.size() == 2 && r.size() == 1;
        float numeratorY = 0, denominatorY = 0;
        if (ok)
        {
            numeratorY = std::min(g[0]->y, g[1]->y);
            denominatorY = std::max(g[0]->y, g[1]->y);
            const float ruleCentre = r[0]->rect.getCentreY();
            ok = numeratorY < ruleCentre && ruleCentre < denominatorY && std::abs(ruleCentre + axis) < 0.6f && std::abs(r[0]->rect.getHeight() - thickness) < 0.6f
                 && r[0]->rect.getWidth() >= g[0]->size * 0.4f && std::abs(g[0]->size - 40.0f) < 0.1f;   // display style: text-size numerator and denominator
        }
        report.add(ok, "A fraction puts the numerator above and the denominator below a rule centred on the math axis, with the font's rule thickness",
                   ok ? "" : "glyphs " + juce::String((int) g.size()) + " rules " + juce::String((int) r.size())
                       + (g.size() == 2 && r.size() == 1
                              ? ": numerator y " + juce::String(numeratorY, 2) + ", denominator y " + juce::String(denominatorY, 2) + ", rule centre "
                                    + juce::String(r[0]->rect.getCentreY(), 2) + " (axis " + juce::String(axis, 2) + "), rule height "
                                    + juce::String(r[0]->rect.getHeight(), 2) + " (want " + juce::String(thickness, 2) + "), rule width "
                                    + juce::String(r[0]->rect.getWidth(), 2) + ", glyph size " + juce::String(g[0]->size, 2)
                              : juce::String()));
        const auto inlineFraction = layoutText("1/2", 40.0f, false);
        const auto ig2 = glyphsOf(inlineFraction);
        report.add(ig2.size() == 2 && ig2[0]->size < 30.0f && ig2[1]->size < 30.0f, "In inline (text) style a fraction's parts are drawn in script size",
                   ig2.size() == 2 ? juce::String(ig2[0]->size, 1) : "");
    }
    {
        const auto up = layoutText("x^2"), down = layoutText("x_i"), both = layoutText("x_i^2");
        auto sup = glyphsOf(up), sub = glyphsOf(down), two = glyphsOf(both);
        const bool ok = sup.size() == 2 && sub.size() == 2 && two.size() == 3
                        && sup[1]->y < sup[0]->y && sup[1]->size < sup[0]->size && sub[1]->y > sub[0]->y && sub[1]->size < sub[0]->size
                        && two[1]->y < two[0]->y && two[2]->y > two[0]->y && two[1]->x > two[0]->x && two[2]->x > two[0]->x;   // base, superscript, subscript
        report.add(ok, "Superscripts rise and subscripts drop, in smaller type, to the right of the base; both together do not collide");
        const auto nestedLayout = layoutText("x^(2^2)");
        const auto nested = glyphsOf(nestedLayout);
        report.add(nested.size() == 3 && nested[0]->size > nested[1]->size && nested[1]->size > nested[2]->size,
                   "Nested scripts scale down at each level (text, script, script-script)",
                   nested.size() == 3 ? juce::String(nested[0]->size, 1) + " > " + juce::String(nested[1]->size, 1) + " > " + juce::String(nested[2]->size, 1) : "");
    }
    {
        const auto root = layoutText("sqrt(x)");
        const auto rules = rulesOf(root);
        const auto glyphs = glyphsOf(root);
        bool ok = rules.size() == 1 && glyphs.size() == 2;
        if (ok)
        {
            // glyphs: the radical sign first, then x; the rule lies above x and starts at the sign's right edge
            const auto& sign = *glyphs[0];
            const auto& radicand = *glyphs[1];
            ok = rules[0]->rect.getBottom() < radicand.y - 0.45f * radicand.size && rules[0]->rect.getX() >= sign.x + 0.3f * sign.size
                 && rules[0]->rect.getRight() >= radicand.x + 0.4f * radicand.size && radicand.x > sign.x;
        }
        report.add(ok, "A radical has its sign, a rule over the radicand, and the radicand beside the sign");
        const auto indexed = layoutText("root(3, x)");
        const auto ig = glyphsOf(indexed);
        // glyphs: the radical sign (full size), the index (smaller, tucked into the sign's notch) and the radicand
        const math_layout::Primitive* indexGlyph = nullptr;
        for (auto* p : ig) if (p->size < 39.0f) indexGlyph = p;
        // the font's negative kern after the degree tucks the sign under it, so the index sits above the sign's left arm
        report.add(ig.size() == 3 && indexGlyph != nullptr && indexGlyph->y < ig[0]->y && indexGlyph->x < ig[2]->x && ig[0]->x < ig[2]->x,
                   "A root with an index draws the index, smaller and raised, left of the radicand",
                   indexGlyph != nullptr ? "index at (" + juce::String(indexGlyph->x, 1) + ", " + juce::String(indexGlyph->y, 1) + "), sign at (" + juce::String(ig[0]->x, 1) + ", " + juce::String(ig[0]->y, 1) + ")" : "no small glyph");
    }
    {
        const auto small = layoutText("(x)"), tall = layoutText("(1/2)");
        const auto tallGlyphs = glyphsOf(tall), smallGlyphs = glyphsOf(small);
        const auto* smallRect = small.nodes.empty() ? nullptr : &small.nodes.back();
        float smallHeight = small.height(), tallHeight = tall.height();
        // the delimiters grow with their contents: a different, larger glyph (or an assembly of several)
        bool bigger = tallHeight > smallHeight + 4.0f;
        std::set<std::uint16_t> smallParens, tallParens;
        smallParens.insert(smallGlyphs.front()->glyph); tallParens.insert(tallGlyphs.front()->glyph);
        report.add(bigger && (tallGlyphs.front()->glyph != smallGlyphs.front()->glyph || tallGlyphs.size() > smallGlyphs.size() + 2),
                   "Parentheses grow with their contents (a larger variant or an assembled stretch)",
                   "heights " + juce::String(smallHeight, 1) + " -> " + juce::String(tallHeight, 1));
        juce::ignoreUnused(smallRect);
    }
    {
        const auto sum = layoutText("sum_(i=1)^n i", 40.0f, true);
        const auto sg = glyphsOf(sum);
        // the operator is the largest glyph; the upper limit is above it and the lower limit below
        const math_layout::Primitive* op = nullptr;
        for (auto* p : sg) if (op == nullptr || p->glyph != op->glyph) { if (op == nullptr || font.inkBounds(p->glyph).getHeight() > font.inkBounds(op->glyph).getHeight()) op = p; }
        bool ok = op != nullptr && sg.size() >= 6;
        if (ok)
        {
            int above = 0, below = 0;
            for (auto* p : sg) { if (p == op) continue; if (p->size < 40.0f && p->y < op->y - 6.0f) ++above; if (p->size < 40.0f && p->y > op->y + 6.0f) ++below; }
            ok = above >= 1 && below >= 3;
        }
        report.add(ok, "A display summation carries its upper limit above and its lower limit below");
        const auto integral = layoutText("int_0^1 x", 40.0f, true);
        const auto ig = glyphsOf(integral);
        const math_layout::Primitive* iop = ig.front();
        report.add(ig.size() == 4 && ig[1]->x >= iop->x + 0.2f * iop->size && ig[2]->x >= iop->x + 0.2f * iop->size && ig[1]->y != ig[2]->y,
                   "An integral places its limits beside it, like scripts");
    }
    {
        const auto binaryLayout = layoutText("a+b"), unaryLayout = layoutText("-b");
        const auto binary = glyphsOf(binaryLayout), unary = glyphsOf(unaryLayout);
        const float gapBinary = binary[2]->x - binary[1]->x, gapUnary = unary[1]->x - unary[0]->x;
        const float plainAdvance = (float) font.advance(binary[1]->glyph) * 40.0f / 1000.0f;
        report.add(binary.size() == 3 && unary.size() == 2 && gapBinary > plainAdvance + 3.0f && gapUnary < plainAdvance + 1.0f,
                   "Binary operators get medium spaces; a leading minus is unary and gets none",
                   "binary gap " + juce::String(gapBinary - plainAdvance, 1) + ", unary gap " + juce::String(gapUnary - plainAdvance, 1));
        const auto relationLayout = layoutText("a=b");
        const auto relation = glyphsOf(relationLayout);
        const float gapRel = relation[2]->x - relation[1]->x - (float) font.advance(relation[1]->glyph) * 40.0f / 1000.0f;
        report.add(gapRel > 4.0f && gapRel > (gapBinary - plainAdvance), "Relations get thick spaces");
    }
    {
        const auto demo = layoutText("f(x)=sqrt(x^2+1)/(x+3)", 30.0f);
        IdAllocator ids;
        const auto tree = parse("f(x)=sqrt(x^2+1)/(x+3)", ids);
        bool every = demo.problem.isEmpty() && demo.width > 100.0f && demo.ascent > 0 && demo.depth > 0;
        for (auto id : idsOf(tree)) every = every;
        // layout assigns every node of its own tree a rectangle (ids differ between parses, so use one parse)
        math_layout::Options o; o.fontSize = 30.0f;
        const auto laid = math_layout::layoutRow(tree, o);
        int missing = 0;
        for (auto id : idsOf(tree)) if (laid.rectOf(id) == nullptr) ++missing;
        const auto* fraction = laid.rectOf(tree->items[3]->id);
        const bool hit = fraction != nullptr && laid.nodeAt(fraction->rect.getCentre()) != nullptr;
        report.add(every && missing == 0 && hit, "The demonstration expression lays out with a rectangle for every node and supports hit testing", "nodes without a rectangle: " + juce::String(missing));
        const auto doubled = layoutText("f(x)=sqrt(x^2+1)/(x+3)", 60.0f);
        report.add(std::abs(doubled.width / demo.width - 2.0f) < 0.05f, "Layout scales with the font size", juce::String(doubled.width / demo.width, 3));
        const auto placeholder = layoutText("1/");
        bool hasMark = false;
        for (const auto& p : placeholder.primitives) hasMark = hasMark || p.type == math_layout::Primitive::Type::Placeholder;
        report.add(hasMark, "An empty slot is drawn as a placeholder mark");
        const auto rendered = layoutText("a+b");
        const auto aligned = [&] {
            IdAllocator a;
            std::vector<std::string> n;
            auto lines = math_linear::parseCell("F &= m*a\nvelocity &= v_0 + a*t", Role::System, a, n);
            math_layout::Options opt; opt.fontSize = 30.0f;
            auto cell = math_layout::layoutCell(lines, Role::System, opt);
            // the two relation signs sit in the same column
            std::vector<float> xs;
            for (const auto& p : cell.primitives) if (p.type == math_layout::Primitive::Type::Glyph && p.glyph == font.glyphFor('=')) xs.push_back(p.x);
            return xs.size() == 2 && std::abs(xs[0] - xs[1]) < 0.5f && cell.height() > 60.0f;
        }();
        juce::ignoreUnused(rendered);
        report.add(aligned, "A system of equations aligns its lines on the & marks");
    }

    // ---- a gallery for the eye -------------------------------------------------------------------------------------------------
    {
        const char* gallery[] = { "f(x)=sqrt(x^2+1)/(x+3)", "F=m*a", "a=dv/dt", "v(t)=v_0+a*t", "(a+b)^2 = a^2 + 2*a*b + b^2",
                                  "x = (-b + sqrt(b^2 - 4*a*c)) / (2*a)", "sum_(i=1)^n i^2 = n*(n+1)*(2*n+1)/6", "int_0^1 x^2 dx = 1/3",
                                  "e^(i*pi) + 1 = 0", "(1/(1+1/(1+1/x)))", "root(3, x^2+1) + alpha*beta/gamma", "sin(x)^2 + cos(x)^2 = 1", "f'(x) = lim_(h->0) ((f(x+h)-f(x))/h)",
                                  "1/ + ? ", "(a+b" };
        const int count = (int) (sizeof(gallery) / sizeof(gallery[0]));
        juce::Image image(juce::Image::ARGB, 1300, 150 * ((count + 1) / 2) + 30, true);
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101720));
        math_layout::Options o;
        o.fontSize = 34.0f;
        float y = 12;
        for (int i = 0; i < count; ++i)
        {
            IdAllocator ids;
            const auto rendered = math_layout::layoutRow(parse(gallery[i], ids), o);
            const float x = (i % 2) * 650.0f + 20.0f;
            const float rowHeight = 150.0f;
            const float top = 12 + (float) (i / 2) * rowHeight;
            g.setColour(juce::Colour(0xff1b2430));
            g.fillRoundedRectangle(x - 10, top, 630, rowHeight - 8, 6.0f);
            math_layout::paint(g, rendered, { x, top + 10.0f + rendered.ascent }, juce::Colour(0xffeaf1f7));
            g.setColour(juce::Colour(0xff7f93a8));
            g.setFont(juce::Font("Consolas", 12.0f, juce::Font::plain));
            g.drawText(gallery[i], (int) x, (int) (top + rowHeight - 28), 600, 16, juce::Justification::left);
            y = top + rowHeight;
        }
        juce::PNGImageFormat png;
        const auto file = destination.getSiblingFile("math-gallery.png");
        file.deleteFile();
        juce::FileOutputStream stream(file);
        const bool wrote = stream.openedOk() && png.writeImageToStream(image, stream);
        report.add(wrote, "A gallery image of typeset expressions was written for visual inspection", file.getFullPathName());
    }

    // ---- the worksheet keeps its cells ---------------------------------------------------------------------------------------------
    {
        auto doc = std::make_shared<WorksheetDocument>();
        doc->reset();
        WorksheetPanel panel(doc);
        panel.setBounds(0, 0, 1000, 800);
        std::vector<std::pair<juce::String, const void*>> before;
        for (const auto& cell : doc->cells) before.push_back({ cell.id, panel.componentFor(cell.id) });
        const auto equation = panel.addMathCell(Role::Equation, "F=m*a");
        const int heightSimple = panel.cellHeight(equation);
        const auto* equationComponent = panel.componentFor(equation);
        bool preserved = panel.componentCount() == 4 && equationComponent != nullptr;
        for (const auto& [id, pointer] : before) preserved = preserved && panel.componentFor(id) == pointer && pointer != nullptr;
        report.add(preserved, "Adding a math cell keeps every existing cell component (no destructive rebuild)");
        panel.setCell(equation, "F = m*a + sqrt(x^2+1)/(x+3)");
        report.add(panel.componentFor(equation) == equationComponent && panel.cellHeight(equation) > heightSimple,
                   "Editing a math cell updates it in place and the cell grows to fit its typeset content",
                   juce::String(heightSimple) + " -> " + juce::String(panel.cellHeight(equation)));
        const auto note = panel.addCell("text", "A note"), code = panel.addCell("frust", "pub fn run() -> String = { \"x\" }");
        bool still = true;
        for (const auto& [id, pointer] : before) still = still && panel.componentFor(id) == pointer;
        still = still && panel.componentFor(equation) == equationComponent && !note.isEmpty() && !code.isEmpty() && panel.componentCount() == 6;
        report.add(still, "Adding text and FRust cells also leaves every other component in place");
        panel.selectCell(equation);
        report.add(panel.selectedCell() == equation, "A cell can be selected");
        panel.setMathRole(equation, Role::System);
        report.add(panel.componentFor(equation) == equationComponent && doc->cells[(size_t) doc->indexOf(equation)].math.role == Role::System, "Changing a role keeps the component");
        // replacing the worksheet creates components for the new cells and drops the old ones
        const auto saved = work.getChildFile("panel.djmath");
        juce::String error;
        WorksheetDocument other;
        other.reset();
        other.save(saved, error);
        doc->load(saved, error);
        panel.rebuild();
        report.add(panel.componentCount() == (int) doc->cells.size() && panel.componentFor(equation) == nullptr && panel.componentFor(doc->cells[0].id) != nullptr,
                   "Opening another worksheet replaces the cell components", error);
    }

    auto* root = new juce::DynamicObject();
    root->setProperty("passed", report.passed);
    root->setProperty("checks", report.checks);
    destination.getParentDirectory().createDirectory();
    destination.replaceWithText(juce::JSON::toString(juce::var(root), false));
    work.deleteRecursively();
    return report.passed;
}
