#pragma once

// STIX Two Math (SIL OFL 1.1, bundled unmodified in Resources/fonts) as a source of mathematical typesetting
// data: character-to-glyph mapping, advances, the OpenType MATH table (layout constants, italic corrections,
// size variants and glyph assemblies for stretchy symbols) and the glyph outlines themselves, read from the
// font's CFF table. Nothing here draws or measures text with a platform text engine, so glyph numbers from the
// MATH table and the shapes drawn always refer to the same font.
//
// All measurements are in font design units (1000 per em for STIX Two Math) unless noted.

#include <juce_graphics/juce_graphics.h>

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace math_font
{
// Indices into Constants::value, in OpenType MATH order.
enum Constant
{
    MathLeading, AxisHeight, AccentBaseHeight, FlattenedAccentBaseHeight, SubscriptShiftDown, SubscriptTopMax,
    SubscriptBaselineDropMin, SuperscriptShiftUp, SuperscriptShiftUpCramped, SuperscriptBottomMin, SuperscriptBaselineDropMax,
    SubSuperscriptGapMin, SuperscriptBottomMaxWithSubscript, SpaceAfterScript, UpperLimitGapMin, UpperLimitBaselineRiseMin,
    LowerLimitGapMin, LowerLimitBaselineDropMin, StackTopShiftUp, StackTopDisplayStyleShiftUp, StackBottomShiftDown,
    StackBottomDisplayStyleShiftDown, StackGapMin, StackDisplayStyleGapMin, StretchStackTopShiftUp, StretchStackBottomShiftDown,
    StretchStackGapAboveMin, StretchStackGapBelowMin, FractionNumeratorShiftUp, FractionNumeratorDisplayStyleShiftUp,
    FractionDenominatorShiftDown, FractionDenominatorDisplayStyleShiftDown, FractionNumeratorGapMin, FractionNumDisplayStyleGapMin,
    FractionRuleThickness, FractionDenominatorGapMin, FractionDenomDisplayStyleGapMin, SkewedFractionHorizontalGap,
    SkewedFractionVerticalGap, OverbarVerticalGap, OverbarRuleThickness, OverbarExtraAscender, UnderbarVerticalGap,
    UnderbarRuleThickness, UnderbarExtraDescender, RadicalVerticalGap, RadicalDisplayStyleVerticalGap, RadicalRuleThickness,
    RadicalExtraAscender, RadicalKernBeforeDegree, RadicalKernAfterDegree,
    ConstantCount
};

struct Constants
{
    int scriptPercentScaleDown = 70, scriptScriptPercentScaleDown = 50;
    int delimitedSubFormulaMinHeight = 0, displayOperatorMinHeight = 0;
    int radicalDegreeBottomRaisePercent = 60;
    int value[ConstantCount] {};
};

struct Variant { std::uint16_t glyph = 0; int measure = 0; };           // measure: the glyph's extent along the stretch direction
struct Part { std::uint16_t glyph = 0; int startConnector = 0, endConnector = 0, fullAdvance = 0; bool extender = false; };
struct Construction
{
    std::vector<Variant> variants;   // ascending size, the base glyph first
    std::vector<Part> parts;         // bottom to top; empty when the font has no assembly
};

class MathFont
{
public:
    // The bundled font, loaded on first use. `ok()` is false (with `error()`) when the file is missing or unusable.
    static MathFont& instance();
    ~MathFont();
    // Tests may point at a specific font file before first use.
    static void useFile(const juce::File& file);

    bool ok() const { return loaded; }
    const juce::String& error() const { return problem; }
    int unitsPerEm() const { return upem; }

    std::uint16_t glyphFor(juce::juce_wchar codepoint) const;     // 0 when the font has no such character
    int advance(std::uint16_t glyph) const;
    int italicCorrection(std::uint16_t glyph) const;
    const Constants& constants() const { return mathConstants; }
    int minConnectorOverlap() const { return connectorOverlap; }
    const Construction* verticalConstruction(std::uint16_t glyph) const;

    // The outline in em units: 1.0 = one em, baseline at y = 0, y grows downward (JUCE convention).
    const juce::Path& outline(std::uint16_t glyph);
    // Ink bounds of the glyph in em units, same orientation. Empty for spaces.
    juce::Rectangle<float> inkBounds(std::uint16_t glyph);

private:
    MathFont();
    bool load(const juce::File& file);
    void fail(const juce::String& why) { loaded = false; problem = why; }

    bool loaded = false;
    juce::String problem;
    juce::MemoryBlock fileData;
    struct Cff;
    std::unique_ptr<Cff> cff;
    int upem = 1000;
    std::unordered_map<juce::juce_wchar, std::uint16_t> cmap;
    std::vector<std::uint16_t> advances;
    Constants mathConstants;
    int connectorOverlap = 0;
    std::unordered_map<std::uint16_t, int> italics;
    std::unordered_map<std::uint16_t, Construction> vertical;
    std::unordered_map<std::uint16_t, juce::Path> paths;
    std::unordered_map<std::uint16_t, juce::Rectangle<float>> bounds;
    static juce::File overrideFile;
};

// Where the bundled font lives: beside the executable, else the source tree's Resources/fonts.
juce::File bundledFontFile();
}
