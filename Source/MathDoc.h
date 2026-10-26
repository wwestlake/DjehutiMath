#pragma once

// The mathematical document model of Djehuti Math worksheets.
//
// A math cell holds structured, immutable trees. Every node has a persistent identifier; every structural
// slot is a Row; infix expressions stay flat inside rows (precedence and meaning belong to the future
// analysis layer); an incomplete expression is valid and is represented by Placeholder nodes. Nothing in
// this file knows about fonts, glyphs or positions: rendering is derived from the tree (MathLayout), and
// linear text (MathLinear) is only an authoring convenience.
//
// Trees are immutable (shared_ptr<const Node>), so a snapshot handed to a background analysis cannot change
// under it. An edit builds a new tree that shares unchanged subtrees and keeps the identifiers of the nodes
// that are still the same.

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace math_doc
{
using NodeId = std::uint64_t;

constexpr int kMaxDepth = 64;
constexpr int kMaxNodesPerCell = 20000;
constexpr int kMaxLinesPerCell = 200;
constexpr int kSchemaVersion = 1;

enum class Kind
{
    Row,          // items: an ordered list of nodes (never a Row)
    Number,       // text: digits and an optional decimal point
    Identifier,   // text: a name (a letter, a Greek letter, a function name, a multi-letter name)
    Operator,     // text: + - . x / , ; : and other non-relational operators
    Relation,     // text: = < > and other relations
    Symbol,       // text: a mathematical symbol or punctuation such as infinity, a prime, an alignment mark
    Text,         // text: literal words inside mathematics
    Fraction,     // slots: numerator, denominator
    Radical,      // slots: radicand, index (absent for a square root)
    Script,       // slots: base, subscript (optional), superscript (optional); at least one script
    Delimited,    // text/text2: open/close delimiter ("" = missing); slots: body
    BigOperator,  // text: the operator character; slots: lower limit, upper limit (both optional)
    Placeholder   // text2: an optional hint; an empty position that still needs an expression
};

enum class Style : std::uint8_t { Auto, Italic, Upright, Bold };

struct Node;
using NodePtr = std::shared_ptr<const Node>;

struct Node
{
    NodeId id = 0;
    Kind kind = Kind::Row;
    std::string text;            // see Kind
    std::string text2;           // see Kind
    Style style = Style::Auto;
    std::vector<NodePtr> items;  // Row only
    std::vector<NodePtr> slots;  // structures only; each a Row, or null for an absent optional slot
};

// Number of slots a structure has (0 for atoms and rows), and whether slot `index` may be absent.
int slotCount(Kind kind);
bool slotIsOptional(Kind kind, int index);
bool isStructure(Kind kind);
const char* kindName(Kind kind);
bool kindFromName(const std::string& name, Kind& kind);

// Document-wide allocator. Identifiers are never reused; the next value is saved with the worksheet.
class IdAllocator
{
public:
    NodeId take() { return next++; }
    NodeId next = 1;
};

// ---- construction -----------------------------------------------------------------------------------------
NodePtr makeRow(IdAllocator& ids, std::vector<NodePtr> items = {});
NodePtr makeAtom(IdAllocator& ids, Kind kind, std::string text, Style style = Style::Auto);
NodePtr makePlaceholder(IdAllocator& ids, std::string hint = {});
NodePtr makeFraction(IdAllocator& ids, NodePtr numerator, NodePtr denominator);
NodePtr makeRadical(IdAllocator& ids, NodePtr radicand, NodePtr index = nullptr);
NodePtr makeScript(IdAllocator& ids, NodePtr base, NodePtr sub, NodePtr sup);
NodePtr makeDelimited(IdAllocator& ids, std::string open, std::string close, NodePtr body);
NodePtr makeBigOperator(IdAllocator& ids, std::string op, NodePtr lower, NodePtr upper);
// A row holding exactly one Placeholder, for a slot the user has not filled yet.
NodePtr placeholderRow(IdAllocator& ids);
// The same node with a different id (copy of a node, keeping its content).
NodePtr withId(const NodePtr& node, NodeId id);
// The same structure with different slots / items.
NodePtr withSlots(const NodePtr& node, std::vector<NodePtr> slots);
NodePtr withItems(const NodePtr& node, std::vector<NodePtr> items);

// ---- inspection -----------------------------------------------------------------------------------------------
void forEach(const NodePtr& root, const std::function<void(const Node&)>& visit);
int countNodes(const NodePtr& root);
NodeId maxId(const NodePtr& root);
bool hasPlaceholder(const NodePtr& root);
NodePtr find(const NodePtr& root, NodeId id);
// Structural equality ignoring identifiers.
bool sameStructure(const NodePtr& a, const NodePtr& b);
// Every id appears once and every node is well formed. `error` says why not.
bool validate(const NodePtr& root, juce::String& error);

// ---- identity across edits ------------------------------------------------------------------------------------
// `fresh` was produced from new input (new ids). Returns the same tree but with the identifiers of `old`
// wherever the node is still the same thing: rows are matched from both ends, and positions that changed in
// place keep their identity when the kind is unchanged.
NodePtr adoptIds(const NodePtr& old, const NodePtr& fresh);

// ---- persistence --------------------------------------------------------------------------------------------------
juce::var toVar(const NodePtr& node);
// Strict: rejects unknown kinds, wrong slots, duplicate or zero ids, excess depth or size.
bool fromVar(const juce::var& value, IdAllocator& ids, std::set<NodeId>& seen, NodePtr& out, juce::String& error);

// ---- cells, snapshots and the compiler boundary -------------------------------------------------------------------
enum class Role { Expression, Definition, Equation, Constraint, Assumption, System };
const char* roleName(Role role);
bool roleFromName(const std::string& name, Role& role);

// The authoritative mathematics of one worksheet cell. A System cell has one line per equation; any other role
// has exactly one line.
struct CellMath
{
    Role role = Role::Expression;
    std::vector<NodePtr> lines;     // Rows
    std::uint64_t revision = 1;     // changes on every edit of this cell; never goes backwards in a session
};

// A reference into a document: a node, or a range of the items of a Row node.
struct SourceRef
{
    NodeId node = 0;
    std::uint32_t childBegin = 0, childEnd = 0;   // [begin, end) items of a Row; 0,0 = the node itself
    bool operator==(const SourceRef& o) const { return node == o.node && childBegin == o.childBegin && childEnd == o.childEnd; }
};

// What the mathematical compiler receives: immutable, versioned, self-describing. The compiler never sees the
// editor, and its results come back keyed by `cellId` and `cellRevision`, so a stale result is recognisable.
struct MathSnapshot
{
    std::string cellId;
    std::uint64_t cellRevision = 0;
    Role role = Role::Expression;
    std::vector<NodePtr> lines;
};

struct Diagnostic
{
    SourceRef where;
    enum class Severity { Info, Warning, Error } severity = Severity::Error;
    std::string code, message;
};

struct AnalysisResult
{
    std::string cellId;
    std::uint64_t forRevision = 0;     // the revision of the snapshot this answers
    bool complete = false;             // false while the expression has placeholders or errors
    std::vector<Diagnostic> diagnostics;
    // The compiler's own representation (MathIR) is deliberately absent: it is not part of the document.
};

// The future frontend implements this. Not implemented in this milestone.
class MathFrontend
{
public:
    virtual ~MathFrontend() = default;
    virtual void analyze(const MathSnapshot& snapshot, std::function<void(AnalysisResult)> done) = 0;
};

juce::var toVar(const CellMath& cell);
bool fromVar(const juce::var& value, IdAllocator& ids, std::set<NodeId>& seen, CellMath& out, juce::String& error);
}
