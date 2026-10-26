#pragma once

// Converts computational results back into mathematical presentation trees (math_doc), which the existing
// typesetting displays unchanged. Every node created gets a fresh identifier from the document's allocator, and
// `derivedFrom` records, for the nodes whose origin is known, the place in the user's notation they came from
// (a result of a computation points at the cell that was asked; a symbolic expression points, node by node, at
// the notation its parts were translated from).
//
// A result is a presentation, not a document: nothing is written into a worksheet by this module.

#include "MathDoc.h"
#include "MathIR.h"
#include "MathValue.h"

namespace math_result
{
struct Options
{
    // Show a dimensioned value in this unit ("mV", "km/h") instead of coherent SI. Ignored when the unit is
    // unknown or its dimension does not match.
    std::string displayUnit;
    int significantDigits = 15;
};

struct Tree
{
    std::vector<math_doc::NodePtr> lines;                    // one Row per line; a System has several
    std::map<math_doc::NodeId, math_ir::Source> derivedFrom; // new node id -> where in the notation it came from
    std::string problem;                                     // set when something could not be displayed
};

Tree fromExpr(const math_ir::ExprPtr& expr, math_doc::IdAllocator& ids);
// `origin` (when valid) is recorded against the root of the result.
Tree fromValue(const math_value::Value& value, math_doc::IdAllocator& ids, const math_ir::Source& origin = {}, const Options& options = {});
}
