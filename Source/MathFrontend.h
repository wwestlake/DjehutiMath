#pragma once

// The mathematical frontend: translates the structured presentation trees of M0 (math_doc) into MathIR.
//
// For one cell snapshot it validates the structure, resolves symbols (bound variables, definitions made in
// other cells, declared assumptions, constants, built-in functions, free variables), computes types and
// physical dimensions, and reports diagnostics that point back at the exact node (or item range) the user
// wrote. It never modifies the snapshot: the trees are immutable and analysis only reads them.
//
// Notation conventions the frontend gives meaning to (also in the Help and the reference cards):
//   - a Definition cell is `name = expr`, `name := expr`, or `f(x, y) = expr`;
//   - a quantity is a number followed by a quoted unit: 9.81 "m/s^2";
//   - an Assumption cell declares symbols: `x ∈ R`, `v ∈ "m/s"`, `x > 0`, `n ∈ Z`;
//   - `sum_(i=1)^n body`, `prod_(i=1)^n body`: the body is the factors that follow, up to + - , ; or a relation;
//   - `int_a^b body dx`: the body runs to the differential `dx` (or `d x`);
//   - `dv/dt` and `∂f/∂x` are derivatives unless those names are defined or declared as ordinary symbols;
//   - [a, b, c] is a vector, [a, b; c, d] a matrix, (a, b) a tuple;
//   - π, e and i are the constants unless the name is bound, defined or declared.
//
// math_frontend depends on math_doc and MathIR only: no GUI, solver or FRust dependency.

#include "MathDoc.h"
#include "MathIR.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace math_frontend
{
// A name defined by a Definition cell.
struct Definition
{
    std::string name, cell;
    bool isFunction = false;
    std::vector<std::string> params;
    math_ir::Type type;              // the type of the value (a variable) or of the result (a function)
    math_ir::ExprPtr body;
    math_ir::Source where;
};

// What an Assumption cell says about a symbol.
struct Declaration
{
    std::string name, cell;
    bool hasDomain = false, hasSign = false, hasDim = false;
    math_ir::Domain domain = math_ir::Domain::Unknown;
    math_ir::Sign sign = math_ir::Sign::Unknown;
    math_ir::Dim dim;
    std::string unit;                // the unit text the dimension came from
    math_ir::Source where;
};

// The result of analysing one cell.
struct CellAnalysis
{
    std::string cell;
    std::uint64_t revision = 0;
    math_doc::Role role = math_doc::Role::Expression;
    std::vector<math_ir::ExprPtr> lines;    // one per line of the snapshot; null for a line that only declares
    math_ir::ExprPtr ir;                    // the whole cell: the line itself, or a System of the lines
    math_ir::Type type;
    std::vector<math_ir::Diagnostic> diagnostics;
    bool hasPlaceholder = false;
    bool complete = false;                  // no placeholders and no errors
    std::set<std::string> defines, declares, uses;
    std::map<std::string, math_ir::Source> defineSites, useSites;
    std::vector<Definition> definitions;
    std::vector<Declaration> declarations;
    bool hasErrors() const;
};

// Looks up what other cells define and declare. The worksheet workspace implements this; a frontend without
// one analyses a cell in isolation.
class Resolver
{
public:
    virtual ~Resolver() = default;
    virtual bool definition(const std::string& name, const std::string& fromCell, Definition& out) = 0;
    virtual bool declaration(const std::string& name, Declaration& out) = 0;
};

struct Options
{
    math_ir::Domain defaultDomain = math_ir::Domain::Real;    // the domain of a symbol nothing has declared
};

std::shared_ptr<const CellAnalysis> analyzeCell(const math_doc::MathSnapshot& snapshot, Resolver* resolver = nullptr, const Options& options = {});

// The existing M0 interface: diagnostics and completeness only (MathIR is not part of the document).
class Frontend final : public math_doc::MathFrontend
{
public:
    explicit Frontend(Resolver* resolver = nullptr, Options options = {}) : resolver(resolver), options(options) {}
    void analyze(const math_doc::MathSnapshot& snapshot, std::function<void(math_doc::AnalysisResult)> done) override;
    std::shared_ptr<const CellAnalysis> analyzeFull(const math_doc::MathSnapshot& snapshot) const { return analyzeCell(snapshot, resolver, options); }
private:
    Resolver* resolver;
    Options options;
};

math_doc::AnalysisResult toAnalysisResult(const CellAnalysis& analysis);
math_ir::Source toSource(const std::string& cell, const math_doc::SourceRef& ref);
math_doc::SourceRef toSourceRef(const math_ir::Source& source);

// Names of the built-in functions and constants, for documentation and checks.
bool isBuiltinFunction(const std::string& name);
}
