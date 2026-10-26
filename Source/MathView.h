#pragma once

#include "MathLayout.h"

#include <JuceHeader.h>

// Shows the typeset mathematics of one worksheet cell. The component owns no mathematics: it is handed the
// cell's immutable lines, lays them out once (MathLayout) and paints the result. Hovering a node reports its
// persistent identifier, which M1's structural editing will build on.
class MathView final : public juce::Component
{
public:
    MathView();

    void setContent(std::vector<math_doc::NodePtr> lines, math_doc::Role role);
    // The height this content needs at the current font size, including padding.
    int preferredHeight() const;
    const math_layout::Rendered& rendered() const { return result; }
    const std::vector<math_doc::NodePtr>& lines() const { return content; }

    std::function<void(const juce::String&)> onHover;      // "node 42  fraction", or "" when over nothing

    void paint(juce::Graphics&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;

    static constexpr float fontSize = 24.0f;

private:
    juce::Point<float> origin() const;

    std::vector<math_doc::NodePtr> content;
    math_doc::Role role = math_doc::Role::Expression;
    math_layout::Rendered result;
};
