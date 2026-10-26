#pragma once

// Native mathematical layout and rendering, derived entirely from the document tree (math_doc) and the
// OpenType MATH data of the bundled font (math_font). Layout follows the TeX/OpenType MATH model: boxes with a
// baseline, atom classes with the TeX spacing table, four style levels with cramped variants, fraction,
// script, radical and large-operator rules from the font's constants, and stretchy delimiters chosen from the
// font's size variants or assembled from its glyph parts.
//
// The result is a flat list of primitives (glyphs, rules, placeholder marks) in pixels plus a rectangle for
// every node, so the same layout can later drive hit testing and caret placement. The tree is never modified
// and holds no positions.

#include "MathDoc.h"

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace math_layout
{
struct Primitive
{
    enum class Type { Glyph, Rule, Placeholder } type = Type::Glyph;
    std::uint16_t glyph = 0;
    float x = 0, y = 0;       // glyph origin (baseline-left), y grows downward
    float size = 0;           // em size in pixels for a glyph
    juce::Rectangle<float> rect;   // Rule / Placeholder
};

struct NodeRect
{
    math_doc::NodeId id = 0;
    juce::Rectangle<float> rect;
};

struct Rendered
{
    float width = 0, ascent = 0, depth = 0;      // extents around the baseline at y = 0
    std::vector<Primitive> primitives;
    std::vector<NodeRect> nodes;
    juce::String problem;                         // set when the math font is unavailable
    float height() const { return ascent + depth; }
    const NodeRect* rectOf(math_doc::NodeId id) const;
    // The innermost node whose rectangle contains the point (relative to the baseline-left origin).
    const NodeRect* nodeAt(juce::Point<float> p) const;
};

struct Options
{
    float fontSize = 22.0f;     // pixels per em
    bool displayStyle = true;   // fractions and large operators in display size
};

// One row (a line of mathematics).
Rendered layoutRow(const math_doc::NodePtr& row, const Options& options);
// A cell's lines: one line, or a System aligned on its '&' marks.
Rendered layoutCell(const std::vector<math_doc::NodePtr>& lines, math_doc::Role role, const Options& options);

// `origin` is where the baseline-left of the first line goes.
void paint(juce::Graphics& g, const Rendered& rendered, juce::Point<float> origin, juce::Colour colour);
}
