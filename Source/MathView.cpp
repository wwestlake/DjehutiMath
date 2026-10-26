#include "MathView.h"

namespace
{
constexpr float kPad = 14.0f;
}

MathView::MathView()
{
    setOpaque(false);
}

void MathView::setContent(std::vector<math_doc::NodePtr> lines, math_doc::Role r)
{
    content = std::move(lines);
    role = r;
    math_layout::Options options;
    options.fontSize = fontSize;
    options.displayStyle = true;
    result = math_layout::layoutCell(content, role, options);
    repaint();
}

int MathView::preferredHeight() const
{
    if (!result.problem.isEmpty())
        return 40;
    return (int) std::ceil(std::max(result.height(), fontSize) + 2.0f * kPad);
}

juce::Point<float> MathView::origin() const
{
    // left-aligned with a margin; the cell's first baseline sits `ascent` below the top padding
    return { kPad, kPad + std::max(result.ascent, fontSize * 0.7f) };
}

void MathView::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff10161d));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    if (!result.problem.isEmpty())
    {
        g.setColour(juce::Colour(0xffffb36b));
        g.setFont(juce::Font(13.0f));
        g.drawFittedText(result.problem, getLocalBounds().reduced(10), juce::Justification::centredLeft, 2);
        return;
    }
    math_layout::paint(g, result, origin(), juce::Colour(0xffeaf1f7));
}

void MathView::mouseMove(const juce::MouseEvent& e)
{
    if (!onHover)
        return;
    const auto local = e.position - origin();
    if (const auto* hit = result.nodeAt(local))
    {
        juce::String kind = "node";
        for (const auto& line : content)
            if (const auto node = math_doc::find(line, hit->id))
            {
                kind = math_doc::kindName(node->kind);
                break;
            }
        onHover("node " + juce::String((juce::int64) hit->id) + "  " + kind);
    }
    else
        onHover({});
}

void MathView::mouseExit(const juce::MouseEvent&)
{
    if (onHover)
        onHover({});
}
