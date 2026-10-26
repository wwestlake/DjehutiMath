#pragma once

// Linear mathematical input: conventional notation typed as text, turned into structured trees.
//
//   (x^2+1)/(x+3)     sqrt(x^2+1)     F=m*a     f(x)=sqrt(x^2+1)/(x+3)     sum_(i=1)^n i^2     int_0^1 x dx
//
// This is an authoring mechanism only. The result is a tree of math_doc nodes; the text is kept beside it as
// informational origin and never read back as the document. Incomplete input is legal: a missing operand
// becomes a Placeholder. Rules a user should know (also in Help):
//   - "/" divides the factors written just before it (a run with no operator between them, such as 2x or
//     f(x)) by the single factor after it; use parentheses to group: a+b/c divides only b, (a+b)/c divides both.
//   - "^" and "_" attach to the factor before them; x^(n+1) takes a group. A trailing apostrophe is a prime.
//   - Letters written together are one name (rate, dv); separate letters with a space or * to multiply.
//   - Greek letters by name (alpha, Omega), function names (sin, ln) and symbols (infty, partial) are recognised.

#include "MathDoc.h"

#include <string>
#include <vector>

namespace math_linear
{
// One line of input. `notes` receives plain-language remarks (an unclosed bracket, a missing operand).
math_doc::NodePtr parseLine(const juce::String& text, math_doc::IdAllocator& ids, std::vector<std::string>& notes);
// A whole cell: a System has one line per text line; any other role has exactly one line.
std::vector<math_doc::NodePtr> parseCell(const juce::String& text, math_doc::Role role, math_doc::IdAllocator& ids,
                                         std::vector<std::string>& notes);
// The inverse: linear text that parses back to the same structure.
std::string toLinear(const math_doc::NodePtr& row);
std::string toLinear(const std::vector<math_doc::NodePtr>& lines);

// Names the layout also needs.
bool isFunctionName(const std::string& name);
}
