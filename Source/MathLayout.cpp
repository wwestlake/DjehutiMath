#include "MathLayout.h"

#include "MathFont.h"
#include "MathLinear.h"

#include <algorithm>
#include <cmath>

namespace math_layout
{
using namespace math_doc;
using math_font::MathFont;

const NodeRect* Rendered::rectOf(NodeId id) const
{
    for (const auto& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const NodeRect* Rendered::nodeAt(juce::Point<float> p) const
{
    const NodeRect* best = nullptr;
    for (const auto& n : nodes)
        if (n.rect.contains(p) && (best == nullptr || n.rect.getWidth() * n.rect.getHeight() <= best->rect.getWidth() * best->rect.getHeight()))
            best = &n;
    return best;
}

namespace
{
enum class Cls { Ord, Op, Bin, Rel, Open, Close, Punct, Inner };

struct Sty
{
    int level = 0;          // 0 display, 1 text, 2 script, 3 scriptscript
    bool cramped = false;
};

struct Box
{
    float width = 0, ascent = 0, depth = 0;
    float italic = 0;       // italic correction of the last glyph, when the box ends in one
    float markerX = -1;     // x of an alignment mark inside this box, or -1
    bool single = false;    // exactly one glyph
    std::vector<Primitive> prims;
    std::vector<NodeRect> rects;
};

// TeX spacing between adjacent atom classes: amount in {0, thin 3mu, medium 4mu, thick 5mu}; `parenthesised`
// entries disappear in script styles.
struct Space { signed char amount; bool parenthesised; };
const Space kSpace[8][8] = {
    /* Ord   */ { {0,0}, {1,0}, {2,1}, {3,1}, {0,0}, {0,0}, {0,0}, {1,1} },
    /* Op    */ { {1,0}, {1,0}, {0,0}, {3,1}, {0,0}, {0,0}, {0,0}, {1,1} },
    /* Bin   */ { {2,1}, {2,1}, {0,0}, {0,0}, {2,1}, {0,0}, {0,0}, {2,1} },
    /* Rel   */ { {3,1}, {3,1}, {0,0}, {0,0}, {3,1}, {0,0}, {0,0}, {3,1} },
    /* Open  */ { {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0} },
    /* Close */ { {0,0}, {1,0}, {2,1}, {3,1}, {0,0}, {0,0}, {0,0}, {1,1} },
    /* Punct */ { {1,1}, {1,1}, {0,0}, {1,1}, {1,1}, {1,1}, {1,1}, {1,1} },
    /* Inner */ { {1,1}, {1,0}, {2,1}, {3,1}, {1,1}, {0,0}, {1,1}, {1,1} },
};

juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }

// Characters of an Identifier as they are drawn: single letters in the mathematical italic block,
// multi-letter names and capital Greek upright, bold on request.
std::vector<juce::juce_wchar> identifierCharacters(const Node& node)
{
    const auto source = text(node.text);
    std::vector<juce::juce_wchar> cps;
    for (auto c : source)
        cps.push_back(c);
    Style style = node.style;
    if (style == Style::Auto)
    {
        const bool single = cps.size() == 1;
        const bool letter = single && (((cps[0] >= 'a' && cps[0] <= 'z') || (cps[0] >= 'A' && cps[0] <= 'Z')) || (cps[0] >= 0x3B1 && cps[0] <= 0x3C9));
        style = letter ? Style::Italic : Style::Upright;
    }
    for (auto& c : cps)
    {
        if (style == Style::Italic)
        {
            if (c == 'h') c = 0x210E;
            else if (c >= 'A' && c <= 'Z') c = 0x1D434 + (c - 'A');
            else if (c >= 'a' && c <= 'z') c = 0x1D44E + (c - 'a');
            else if (c >= 0x3B1 && c <= 0x3C9) c = 0x1D6FC + (c - 0x3B1);
        }
        else if (style == Style::Bold)
        {
            if (c >= 'A' && c <= 'Z') c = 0x1D400 + (c - 'A');
            else if (c >= 'a' && c <= 'z') c = 0x1D41A + (c - 'a');
        }
    }
    return cps;
}

Cls classOf(const Node& n)
{
    switch (n.kind)
    {
        case Kind::Identifier:
        {
            const bool upright = n.style == Style::Upright || (n.style == Style::Auto && juce::String(n.text).length() > 1);
            return upright && math_linear::isFunctionName(n.text) ? Cls::Op : Cls::Ord;
        }
        case Kind::Operator: return (n.text == "," || n.text == ";" || n.text == ":") ? Cls::Punct : Cls::Bin;
        case Kind::Relation: return Cls::Rel;
        case Kind::Symbol:
            if (n.text == "(" || n.text == "[" || n.text == "{") return Cls::Open;
            if (n.text == ")" || n.text == "]" || n.text == "}") return Cls::Close;
            return Cls::Ord;
        case Kind::Fraction: return Cls::Inner;
        case Kind::Delimited: return Cls::Close;          // as the left neighbour of what follows
        case Kind::BigOperator: return Cls::Op;
        case Kind::Script:
            if (n.slots[0] != nullptr && n.slots[0]->items.size() == 1)
                return classOf(*n.slots[0]->items[0]);
            return Cls::Ord;
        default: return Cls::Ord;
    }
}

// The class an atom has as the right-hand neighbour of what precedes it (a group opens on its left).
Cls rightClassOf(const Node& n)
{
    if (n.kind == Kind::Delimited)
        return Cls::Open;
    if (n.kind == Kind::Script && n.slots[0] != nullptr && n.slots[0]->items.size() == 1)
        return rightClassOf(*n.slots[0]->items[0]);
    return classOf(n);
}

class Layout
{
public:
    Layout(const Options& options) : font(MathFont::instance()), opts(options), upem((float) font.unitsPerEm()) {}

    // ---- sizes ---------------------------------------------------------------------------------------------------
    float size(Sty s) const
    {
        const auto& c = font.constants();
        if (s.level <= 1) return opts.fontSize;
        if (s.level == 2) return opts.fontSize * (float) c.scriptPercentScaleDown / 100.0f;
        return opts.fontSize * (float) c.scriptScriptPercentScaleDown / 100.0f;
    }
    float k(Sty s) const { return size(s) / upem; }                                  // pixels per design unit
    float C(int index, Sty s) const { return (float) font.constants().value[index] * k(s); }
    static Sty scriptStyle(Sty s, bool cramped) { return { s.level < 2 ? 2 : 3, cramped }; }
    static Sty fractionStyle(Sty s, bool cramped) { return { std::min(3, s.level + 1), cramped }; }
    static Sty crampedOf(Sty s) { return { s.level, true }; }

    // ---- boxes ---------------------------------------------------------------------------------------------------------
    static void place(Box& parent, const Box& child, float dx, float dyDown)
    {
        for (auto p : child.prims)
        {
            p.x += dx; p.y += dyDown; p.rect.translate(dx, dyDown);
            parent.prims.push_back(p);
        }
        for (auto r : child.rects)
        {
            r.rect.translate(dx, dyDown);
            parent.rects.push_back(r);
        }
        parent.ascent = std::max(parent.ascent, child.ascent - dyDown);
        parent.depth = std::max(parent.depth, child.depth + dyDown);
        parent.width = std::max(parent.width, dx + child.width);
        if (child.markerX >= 0) parent.markerX = dx + child.markerX;
    }

    static void record(Box& box, NodeId id, float size)
    {
        const float top = std::min(-box.ascent, -0.72f * size), bottom = std::max(box.depth, 0.22f * size);
        box.rects.push_back({ id, juce::Rectangle<float>(0.0f, top, box.width, bottom - top) });
    }

    Box glyphBox(std::uint16_t glyph, Sty s, float forcedAdvance = -1.0f)
    {
        Box b;
        const float sz = size(s);
        b.width = forcedAdvance >= 0.0f ? forcedAdvance : (float) font.advance(glyph) * k(s);
        const auto ink = font.inkBounds(glyph);
        b.ascent = std::max(0.0f, -ink.getY() * sz);
        b.depth = std::max(0.0f, ink.getBottom() * sz);
        Primitive p;
        p.type = Primitive::Type::Glyph;
        p.glyph = glyph;
        p.size = sz;
        b.prims.push_back(p);
        b.italic = (float) font.italicCorrection(glyph) * k(s);
        b.single = true;
        return b;
    }

    Box glyphRun(const std::vector<juce::juce_wchar>& cps, Sty s)
    {
        Box b;
        const float sz = size(s);
        float x = 0;
        for (auto c : cps)
        {
            const auto glyph = font.glyphFor(c);
            const auto ink = font.inkBounds(glyph);
            Primitive p;
            p.type = Primitive::Type::Glyph;
            p.glyph = glyph;
            p.x = x;
            p.size = sz;
            b.prims.push_back(p);
            b.ascent = std::max(b.ascent, -ink.getY() * sz);
            b.depth = std::max(b.depth, ink.getBottom() * sz);
            x += (float) font.advance(glyph) * k(s);
            b.italic = (float) font.italicCorrection(glyph) * k(s);
        }
        b.width = x;
        b.single = cps.size() == 1;
        return b;
    }

    Box atomBox(const Node& n, Sty s)
    {
        std::vector<juce::juce_wchar> cps;
        if (n.kind == Kind::Identifier)
            cps = identifierCharacters(n);
        else
            for (auto c : text(n.text))
                cps.push_back(n.kind == Kind::Operator && c == '-' ? (juce::juce_wchar) 0x2212 : c);
        Box b = glyphRun(cps, s);
        if (n.kind == Kind::Symbol && n.text == "&")   // an alignment mark: no ink, no width
        {
            Box mark;
            mark.markerX = 0;
            return mark;
        }
        return b;
    }

    Box placeholderBox(const Node& n, Sty s)
    {
        Box b;
        const float sz = size(s);
        b.width = 0.62f * sz;
        b.ascent = 0.70f * sz;
        b.depth = 0.0f;
        Primitive p;
        p.type = Primitive::Type::Placeholder;
        p.rect = juce::Rectangle<float>(0.06f * sz, -0.70f * sz, 0.50f * sz, 0.70f * sz);
        b.prims.push_back(p);
        juce::ignoreUnused(n);
        return b;
    }

    // ---- stretchy glyphs -----------------------------------------------------------------------------------------------------
    // A vertical glyph (delimiter, radical sign, large operator) of at least `required` pixels, centred on the
    // math axis when `centreOnAxis`; otherwise its baseline stays on the line. `usedMeasure` is its actual size.
    Box stretchy(juce::juce_wchar character, float required, Sty s, bool centreOnAxis, float* usedMeasure = nullptr)
    {
        const auto base = font.glyphFor(character);
        Box result;
        if (base == 0)
            return result;
        const auto* construction = font.verticalConstruction(base);
        const float kk = k(s);
        std::uint16_t chosen = base;
        float measure = 0;
        bool assemble = false;
        if (construction != nullptr && !construction->variants.empty())
        {
            const math_font::Variant* pick = nullptr;
            for (const auto& v : construction->variants)
                if ((float) v.measure * kk >= required) { pick = &v; break; }
            if (pick == nullptr)
            {
                if (!construction->parts.empty()) assemble = true;
                else pick = &construction->variants.back();
            }
            if (pick != nullptr) { chosen = pick->glyph; measure = (float) pick->measure * kk; }
        }
        else if (construction != nullptr && !construction->parts.empty() && required > 0)
            assemble = true;

        if (!assemble)
        {
            Box g = glyphBox(chosen, s);
            if (measure == 0) measure = g.ascent + g.depth;
            const float dy = centreOnAxis ? (g.ascent - g.depth) / 2.0f - C(math_font::AxisHeight, s) : 0.0f;
            place(result, g, 0, dy);
            result.width = g.width;
            result.italic = g.italic;
            if (usedMeasure != nullptr) *usedMeasure = measure;
            return result;
        }

        // assembly from parts, bottom to top
        const auto& parts = construction->parts;
        const float minOverlap = (float) font.minConnectorOverlap() * kk;
        auto sumFor = [&](int repeats, int& count) {
            float sum = 0;
            count = 0;
            for (const auto& p : parts)
            {
                const int times = p.extender ? repeats : 1;
                sum += (float) p.fullAdvance * kk * (float) times;
                count += times;
            }
            return sum;
        };
        int repeats = 0, count = 0;
        float total = sumFor(repeats, count);
        while (count > 1 && total - (float) (count - 1) * minOverlap < required && repeats < 60)
            total = sumFor(++repeats, count);
        const float overlap = count > 1 ? std::max(minOverlap, (total - required) / (float) (count - 1)) : 0.0f;
        float cursor = 0, width = 0;
        std::vector<std::pair<Box, float>> placed;
        for (const auto& p : parts)
        {
            const int times = p.extender ? repeats : 1;
            for (int t = 0; t < times; ++t)
            {
                Box g = glyphBox(p.glyph, s);
                placed.push_back({ g, cursor + g.depth });          // the glyph's origin, measured up from the assembly bottom
                width = std::max(width, g.width);
                cursor += (float) p.fullAdvance * kk - overlap;
            }
        }
        const float extent = cursor + overlap;
        // y (down) of the assembly bottom relative to the line baseline: the middle of the assembly sits on the axis
        const float bottomDown = centreOnAxis ? extent / 2.0f - C(math_font::AxisHeight, s) : 0.0f;
        for (auto& [g, originUp] : placed)
            place(result, g, 0, bottomDown - originUp);
        result.width = width;
        result.ascent = std::max(result.ascent, extent - bottomDown);
        result.depth = std::max(result.depth, bottomDown);
        if (usedMeasure != nullptr) *usedMeasure = extent;
        return result;
    }

    // ---- rows -----------------------------------------------------------------------------------------------------------------
    Box row(const NodePtr& r, Sty s)
    {
        Box out;
        if (r == nullptr)
            return out;
        struct Item { Box box; Cls cls; Cls rcls; };      // cls: as left neighbour; rcls: as right neighbour
        std::vector<Item> items;
        for (const auto& child : r->items)
            items.push_back({ node(child, s), classOf(*child), rightClassOf(*child) });
        // TeX rules 5 and 6: a binary operator with no left operand, or no right operand, is ordinary
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (items[i].cls != Cls::Bin) continue;
            const bool noLeft = i == 0 || items[i - 1].cls == Cls::Bin || items[i - 1].cls == Cls::Op || items[i - 1].cls == Cls::Rel
                                || items[i - 1].cls == Cls::Open || items[i - 1].cls == Cls::Punct;
            const bool noRight = i + 1 == items.size() || items[i + 1].rcls == Cls::Rel || items[i + 1].rcls == Cls::Close || items[i + 1].rcls == Cls::Punct;
            if (noLeft || noRight) items[i].cls = items[i].rcls = Cls::Ord;
        }
        const float mu = size(s) / 18.0f;
        float x = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
            {
                const auto sp = kSpace[(int) items[i - 1].cls][(int) items[i].rcls];
                if (!(sp.parenthesised && s.level >= 2))
                    x += (sp.amount == 1 ? 3.0f : sp.amount == 2 ? 4.0f : sp.amount == 3 ? 5.0f : 0.0f) * mu;
            }
            place(out, items[i].box, x, 0);
            x += items[i].box.width;
            out.italic = items[i].box.italic;
        }
        out.width = x;
        record(out, r->id, size(s));
        return out;
    }

    Box node(const NodePtr& n, Sty s)
    {
        Box b;
        switch (n->kind)
        {
            case Kind::Row: return row(n, s);
            case Kind::Number: case Kind::Identifier: case Kind::Operator: case Kind::Relation: case Kind::Symbol:
                b = atomBox(*n, s);
                break;
            case Kind::Text:
            {
                std::vector<juce::juce_wchar> cps;
                for (auto c : text(n->text)) cps.push_back(c);
                b = glyphRun(cps, s);
                break;
            }
            case Kind::Placeholder: b = placeholderBox(*n, s); break;
            case Kind::Fraction: b = fraction(*n, s); break;
            case Kind::Radical: b = radical(*n, s); break;
            case Kind::Script: b = script(*n, s); break;
            case Kind::Delimited: b = delimited(*n, s); break;
            case Kind::BigOperator: b = bigOperator(*n, s); break;
        }
        record(b, n->id, size(s));
        return b;
    }

    // ---- structures ------------------------------------------------------------------------------------------------------------
    Box fraction(const Node& n, Sty s)
    {
        const bool display = s.level == 0;
        const Box num = row(n.slots[0], fractionStyle(s, s.cramped)), den = row(n.slots[1], fractionStyle(s, true));
        const float axis = C(math_font::AxisHeight, s);
        const float thickness = std::max(C(math_font::FractionRuleThickness, s), 1.0f);
        float shiftUp = C(display ? math_font::FractionNumeratorDisplayStyleShiftUp : math_font::FractionNumeratorShiftUp, s);
        float shiftDown = C(display ? math_font::FractionDenominatorDisplayStyleShiftDown : math_font::FractionDenominatorShiftDown, s);
        const float gapNum = C(display ? math_font::FractionNumDisplayStyleGapMin : math_font::FractionNumeratorGapMin, s);
        const float gapDen = C(display ? math_font::FractionDenomDisplayStyleGapMin : math_font::FractionDenominatorGapMin, s);
        const float numGap = (shiftUp - num.depth) - (axis + thickness / 2.0f);
        if (numGap < gapNum) shiftUp += gapNum - numGap;
        const float denGap = (axis - thickness / 2.0f) - (-shiftDown + den.ascent);
        if (denGap < gapDen) shiftDown += gapDen - denGap;
        const float pad = 0.08f * size(s);
        const float inner = std::max(num.width, den.width);
        Box b;
        place(b, num, pad + (inner - num.width) / 2.0f, -shiftUp);
        place(b, den, pad + (inner - den.width) / 2.0f, shiftDown);
        Primitive rule;
        rule.type = Primitive::Type::Rule;
        rule.rect = juce::Rectangle<float>(pad, -(axis + thickness / 2.0f), inner, thickness);
        b.prims.push_back(rule);
        b.ascent = std::max(b.ascent, axis + thickness / 2.0f);
        b.depth = std::max(b.depth, thickness / 2.0f - axis);
        b.width = inner + 2.0f * pad;
        return b;
    }

    Box radical(const Node& n, Sty s)
    {
        const bool display = s.level == 0;
        const Box radicand = row(n.slots[0], crampedOf(s));
        const float thickness = C(math_font::RadicalRuleThickness, s);
        float gap = C(display ? math_font::RadicalDisplayStyleVerticalGap : math_font::RadicalVerticalGap, s);
        const float required = radicand.ascent + radicand.depth + gap + thickness;
        float measure = 0;
        Box sign = stretchy(0x221A, required, s, false, &measure);
        // the sign's top is the rule's top; extra height is shared above and below the radicand
        if (measure > required) gap += (measure - required) / 2.0f;
        const float top = radicand.ascent + gap + thickness;
        const float signOriginUp = top - sign.ascent;                      // sign baseline, up from the line baseline
        Box index;
        float indexX = 0, signX = 0;
        const bool hasIndex = n.slots[1] != nullptr;
        if (hasIndex)
        {
            index = row(n.slots[1], { 3, false });
            const float before = C(math_font::RadicalKernBeforeDegree, s), after = C(math_font::RadicalKernAfterDegree, s);
            indexX = before;
            signX = before + index.width + after;
            if (signX < 0) { indexX -= signX; signX = 0; }
        }
        Box b;
        place(b, sign, signX, -signOriginUp);
        if (hasIndex)
        {
            const float bottomUp = signOriginUp - sign.depth;
            const float raise = bottomUp + (float) font.constants().radicalDegreeBottomRaisePercent / 100.0f * measure;
            place(b, index, indexX, -(raise + index.depth));
        }
        const float radicandX = signX + sign.width;
        place(b, radicand, radicandX, 0);
        Primitive rule;
        rule.type = Primitive::Type::Rule;
        rule.rect = juce::Rectangle<float>(radicandX - 0.5f, -top, radicand.width + 0.5f, thickness);
        b.prims.push_back(rule);
        b.width = radicandX + radicand.width;
        b.ascent = std::max(b.ascent, top + C(math_font::RadicalExtraAscender, s));
        b.depth = std::max(b.depth, radicand.depth + std::max(0.0f, (measure - required) / 2.0f));
        return b;
    }

    Box script(const Node& n, Sty s)
    {
        const Box base = row(n.slots[0], s);
        const Sty ss = scriptStyle(s, s.cramped), sub_style = scriptStyle(s, true);
        const bool hasSub = n.slots[1] != nullptr, hasSup = n.slots[2] != nullptr;
        const Box sub = hasSub ? row(n.slots[1], sub_style) : Box(), sup = hasSup ? row(n.slots[2], ss) : Box();
        const bool kernel = base.single || (n.slots[0]->items.size() == 1 && n.slots[0]->items[0]->kind != Kind::Delimited
                                            && !isStructure(n.slots[0]->items[0]->kind));
        float u = kernel ? 0.0f : base.ascent - C(math_font::SuperscriptBaselineDropMax, ss);
        float v = kernel ? 0.0f : base.depth + C(math_font::SubscriptBaselineDropMin, ss);
        float p = 0, q = 0;
        if (hasSup)
            p = std::max({ u, C(s.cramped ? math_font::SuperscriptShiftUpCramped : math_font::SuperscriptShiftUp, s),
                           sup.depth + C(math_font::SuperscriptBottomMin, s) });
        if (hasSub)
            q = std::max({ v, C(math_font::SubscriptShiftDown, s), sub.ascent - C(math_font::SubscriptTopMax, s) });
        if (hasSub && hasSup)
        {
            const float gap = (p - sup.depth) - (sub.ascent - q);
            const float minGap = C(math_font::SubSuperscriptGapMin, s);
            if (gap < minGap) q += minGap - gap;
            const float psi = C(math_font::SuperscriptBottomMaxWithSubscript, s) - (p - sup.depth);
            if (psi > 0) { p += psi; q -= psi; }
        }
        Box b;
        place(b, base, 0, 0);
        const float supX = base.width + base.italic;
        float right = base.width;
        if (hasSup) { place(b, sup, supX, -p); right = std::max(right, supX + sup.width); }
        if (hasSub) { place(b, sub, base.width, q); right = std::max(right, base.width + sub.width); }
        b.width = right + C(math_font::SpaceAfterScript, s);
        return b;
    }

    Box delimited(const Node& n, Sty s)
    {
        const Box body = row(n.slots[0], s);
        const float axis = C(math_font::AxisHeight, s);
        const float delta = std::max(body.ascent - axis, body.depth + axis);
        float required = 2.0f * delta * 0.901f;
        const float minimum = (float) font.constants().delimitedSubFormulaMinHeight * k(s);
        if (body.ascent + body.depth <= minimum) required = 0.0f;
        Box b;
        float x = 0;
        auto delimiter = [&](const std::string& d) {
            if (d.empty() || d == ".") return;
            const auto cp = text(d)[0];
            Box g = stretchy(cp, required, s, true);
            place(b, g, x, 0);
            x += g.width;
        };
        delimiter(n.text);
        place(b, body, x, 0);
        x += body.width;
        delimiter(n.text2);
        b.width = x;
        b.ascent = std::max(b.ascent, body.ascent);
        b.depth = std::max(b.depth, body.depth);
        return b;
    }

    Box bigOperator(const Node& n, Sty s)
    {
        const bool display = s.level == 0;
        const auto cp = text(n.text)[0];
        const bool limitsAbove = display && (cp == 0x2211 || cp == 0x220F || cp == 0x2210);
        float required = 0;
        if (display) required = (float) font.constants().displayOperatorMinHeight * k(s);
        Box op = stretchy(cp, required, s, true);
        const bool hasLower = n.slots[0] != nullptr, hasUpper = n.slots[1] != nullptr;
        if (!hasLower && !hasUpper)
            return op;
        const Sty upStyle = scriptStyle(s, s.cramped), lowStyle = scriptStyle(s, true);
        const Box upper = hasUpper ? row(n.slots[1], upStyle) : Box(), lower = hasLower ? row(n.slots[0], lowStyle) : Box();
        Box b;
        if (limitsAbove)
        {
            const float width = std::max({ op.width, upper.width, lower.width });
            place(b, op, (width - op.width) / 2.0f, 0);
            if (hasUpper)
            {
                const float up = std::max(op.ascent + C(math_font::UpperLimitGapMin, s) + upper.depth, C(math_font::UpperLimitBaselineRiseMin, s));
                place(b, upper, (width - upper.width) / 2.0f, -up);
            }
            if (hasLower)
            {
                const float down = std::max(op.depth + C(math_font::LowerLimitGapMin, s) + lower.ascent, C(math_font::LowerLimitBaselineDropMin, s));
                place(b, lower, (width - lower.width) / 2.0f, down);
            }
            b.width = width;
            return b;
        }
        // limits at the side, like scripts
        place(b, op, 0, 0);
        float right = op.width;
        if (hasUpper)
        {
            const float p = std::max({ op.ascent - C(math_font::SuperscriptBaselineDropMax, upStyle), C(math_font::SuperscriptShiftUp, s), upper.depth + C(math_font::SuperscriptBottomMin, s) });
            place(b, upper, op.width + op.italic, -p);
            right = std::max(right, op.width + op.italic + upper.width);
        }
        if (hasLower)
        {
            const float q = std::max({ op.depth + C(math_font::SubscriptBaselineDropMin, lowStyle), C(math_font::SubscriptShiftDown, s), lower.ascent - C(math_font::SubscriptTopMax, s) });
            place(b, lower, op.width - op.italic * 0.5f, q);
            right = std::max(right, op.width - op.italic * 0.5f + lower.width);
        }
        b.width = right + C(math_font::SpaceAfterScript, s);
        return b;
    }

private:
    MathFont& font;
    Options opts;
    float upem;
};

Rendered toRendered(const Box& b)
{
    Rendered r;
    r.width = b.width;
    r.ascent = b.ascent;
    r.depth = b.depth;
    r.primitives = b.prims;
    r.nodes = b.rects;
    return r;
}
}

Rendered layoutRow(const NodePtr& rowNode, const Options& options)
{
    auto& font = MathFont::instance();
    if (!font.ok())
    {
        Rendered r;
        r.problem = font.error();
        return r;
    }
    Layout layout(options);
    return toRendered(layout.row(rowNode, { options.displayStyle ? 0 : 1, false }));
}

Rendered layoutCell(const std::vector<NodePtr>& lines, Role role, const Options& options)
{
    auto& font = MathFont::instance();
    Rendered out;
    if (!font.ok())
    {
        out.problem = font.error();
        return out;
    }
    Layout layout(options);
    const Sty style { options.displayStyle ? 0 : 1, false };
    std::vector<Box> boxes;
    for (const auto& line : lines)
        boxes.push_back(layout.row(line, style));
    float marker = 0;
    if (role == Role::System)
        for (const auto& b : boxes) marker = std::max(marker, b.markerX);
    float y = 0, width = 0;
    const float gap = 0.45f * options.fontSize;
    Box all;
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const float dx = role == Role::System && boxes[i].markerX >= 0 ? marker - boxes[i].markerX : 0.0f;
        if (i > 0) y += boxes[i - 1].depth + gap + boxes[i].ascent;
        Layout::place(all, boxes[i], dx, y);
        width = std::max(width, dx + boxes[i].width);
    }
    all.width = width;
    // the first line's baseline is the cell's baseline
    return toRendered(all);
}

void paint(juce::Graphics& g, const Rendered& r, juce::Point<float> origin, juce::Colour colour)
{
    auto& font = MathFont::instance();
    g.setColour(colour);
    for (const auto& p : r.primitives)
    {
        switch (p.type)
        {
            case Primitive::Type::Glyph:
            {
                const auto& path = font.outline(p.glyph);
                if (!path.isEmpty())
                    g.fillPath(path, juce::AffineTransform::scale(p.size).translated(origin.x + p.x, origin.y + p.y));
                break;
            }
            case Primitive::Type::Rule:
            {
                auto rect = p.rect.translated(origin.x, origin.y);
                if (rect.getHeight() < 1.0f) rect = rect.withSizeKeepingCentre(rect.getWidth(), 1.0f);
                g.fillRect(rect);
                break;
            }
            case Primitive::Type::Placeholder:
            {
                const auto rect = p.rect.translated(origin.x, origin.y);
                g.setColour(colour.withAlpha(0.55f));
                const float dash[] = { 3.0f, 2.5f };
                juce::Path outline;
                outline.addRoundedRectangle(rect, 2.5f);
                juce::Path dashed;
                juce::PathStrokeType(1.2f).createDashedStroke(dashed, outline, dash, 2);
                g.fillPath(dashed);
                g.setColour(colour);
                break;
            }
        }
    }
}
}
