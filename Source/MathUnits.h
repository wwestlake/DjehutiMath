#pragma once

// Physical units for MathIR: a small SI-based catalogue and a parser for unit expressions such as "m/s^2",
// "kg*m/s^2", "kΩ" or "mV". Depends on MathIR only.
//
// Units are linear (a factor to the SI coherent unit). Offset units such as degrees Celsius are deliberately
// absent: they need quantity-kind semantics that a later milestone will define.

#include "MathIR.h"

namespace math_units
{
struct UnitInfo
{
    std::string symbol;
    math_ir::Dim dim;
    double scale = 1.0;    // value in this unit * scale = value in coherent SI
};

// An exact catalogue symbol ("N", "min") or a prefixed one ("kΩ", "mm"). False when unknown.
bool lookup(const std::string& symbol, UnitInfo& out);
// Every catalogue symbol (without prefixes), for documentation and tests.
std::vector<UnitInfo> catalogue();

struct Parsed
{
    bool ok = false;
    std::string error;
    math_ir::Dim dim;
    double scale = 1.0;
    math_ir::ExprPtr expr;     // the unit as MathIR: Unit leaves combined with Mul, Div and Pow
};
// `src` is attached to every node created.
Parsed parse(const std::string& text, const math_ir::Source& src = {});
}
