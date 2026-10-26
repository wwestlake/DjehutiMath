#include "MathDoc.h"

#include <algorithm>

namespace math_doc
{
namespace
{
struct KindInfo { Kind kind; const char* name; int slots; };
const KindInfo kKinds[] = {
    { Kind::Row, "row", 0 },          { Kind::Number, "num", 0 },       { Kind::Identifier, "id", 0 },
    { Kind::Operator, "op", 0 },      { Kind::Relation, "rel", 0 },     { Kind::Symbol, "sym", 0 },
    { Kind::Text, "text", 0 },        { Kind::Fraction, "frac", 2 },    { Kind::Radical, "sqrt", 2 },
    { Kind::Script, "script", 3 },    { Kind::Delimited, "delim", 1 },  { Kind::BigOperator, "big", 2 },
    { Kind::Placeholder, "hole", 0 },
};

const KindInfo& info(Kind kind)
{
    for (const auto& k : kKinds)
        if (k.kind == kind)
            return k;
    return kKinds[0];
}

// JSON keys of the slots, per structure.
const char* slotKey(Kind kind, int index)
{
    switch (kind)
    {
        case Kind::Fraction: return index == 0 ? "num" : "den";
        case Kind::Radical: return index == 0 ? "body" : "index";
        case Kind::Script: return index == 0 ? "base" : index == 1 ? "sub" : "sup";
        case Kind::Delimited: return "body";
        case Kind::BigOperator: return index == 0 ? "lower" : "upper";
        default: return "";
    }
}

std::shared_ptr<Node> blank(IdAllocator& ids, Kind kind)
{
    auto node = std::make_shared<Node>();
    node->kind = kind;
    node->id = ids.take();
    return node;
}

bool isAtom(Kind kind)
{
    return kind == Kind::Number || kind == Kind::Identifier || kind == Kind::Operator || kind == Kind::Relation
        || kind == Kind::Symbol || kind == Kind::Text;
}
}

int slotCount(Kind kind) { return info(kind).slots; }
bool isStructure(Kind kind) { return info(kind).slots > 0; }

bool slotIsOptional(Kind kind, int index)
{
    return (kind == Kind::Radical && index == 1) || (kind == Kind::Script && index >= 1) || kind == Kind::BigOperator;
}

const char* kindName(Kind kind) { return info(kind).name; }

bool kindFromName(const std::string& name, Kind& kind)
{
    for (const auto& k : kKinds)
        if (name == k.name)
        {
            kind = k.kind;
            return true;
        }
    return false;
}

const char* roleName(Role role)
{
    switch (role)
    {
        case Role::Expression: return "expression";
        case Role::Definition: return "definition";
        case Role::Equation: return "equation";
        case Role::Constraint: return "constraint";
        case Role::Assumption: return "assumption";
        case Role::System: return "system";
    }
    return "expression";
}

bool roleFromName(const std::string& name, Role& role)
{
    for (auto r : { Role::Expression, Role::Definition, Role::Equation, Role::Constraint, Role::Assumption, Role::System })
        if (name == roleName(r))
        {
            role = r;
            return true;
        }
    return false;
}

// ---- construction ------------------------------------------------------------------------------------------------
NodePtr makeRow(IdAllocator& ids, std::vector<NodePtr> items)
{
    auto node = blank(ids, Kind::Row);
    node->items = std::move(items);
    return node;
}

NodePtr makeAtom(IdAllocator& ids, Kind kind, std::string text, Style style)
{
    auto node = blank(ids, kind);
    node->text = std::move(text);
    node->style = style;
    return node;
}

NodePtr makePlaceholder(IdAllocator& ids, std::string hint)
{
    auto node = blank(ids, Kind::Placeholder);
    node->text2 = std::move(hint);
    return node;
}

NodePtr makeFraction(IdAllocator& ids, NodePtr numerator, NodePtr denominator)
{
    auto node = blank(ids, Kind::Fraction);
    node->slots = { std::move(numerator), std::move(denominator) };
    return node;
}

NodePtr makeRadical(IdAllocator& ids, NodePtr radicand, NodePtr index)
{
    auto node = blank(ids, Kind::Radical);
    node->slots = { std::move(radicand), std::move(index) };
    return node;
}

NodePtr makeScript(IdAllocator& ids, NodePtr base, NodePtr sub, NodePtr sup)
{
    auto node = blank(ids, Kind::Script);
    node->slots = { std::move(base), std::move(sub), std::move(sup) };
    return node;
}

NodePtr makeDelimited(IdAllocator& ids, std::string open, std::string close, NodePtr body)
{
    auto node = blank(ids, Kind::Delimited);
    node->text = std::move(open);
    node->text2 = std::move(close);
    node->slots = { std::move(body) };
    return node;
}

NodePtr makeBigOperator(IdAllocator& ids, std::string op, NodePtr lower, NodePtr upper)
{
    auto node = blank(ids, Kind::BigOperator);
    node->text = std::move(op);
    node->slots = { std::move(lower), std::move(upper) };
    return node;
}

NodePtr placeholderRow(IdAllocator& ids) { return makeRow(ids, { makePlaceholder(ids) }); }

NodePtr withId(const NodePtr& node, NodeId id)
{
    auto copy = std::make_shared<Node>(*node);
    copy->id = id;
    return copy;
}

NodePtr withSlots(const NodePtr& node, std::vector<NodePtr> slots)
{
    auto copy = std::make_shared<Node>(*node);
    copy->slots = std::move(slots);
    return copy;
}

NodePtr withItems(const NodePtr& node, std::vector<NodePtr> items)
{
    auto copy = std::make_shared<Node>(*node);
    copy->items = std::move(items);
    return copy;
}

// ---- inspection ------------------------------------------------------------------------------------------------------
void forEach(const NodePtr& root, const std::function<void(const Node&)>& visit)
{
    if (root == nullptr)
        return;
    visit(*root);
    for (const auto& item : root->items) forEach(item, visit);
    for (const auto& slot : root->slots) forEach(slot, visit);
}

int countNodes(const NodePtr& root)
{
    int n = 0;
    forEach(root, [&n](const Node&) { ++n; });
    return n;
}

NodeId maxId(const NodePtr& root)
{
    NodeId m = 0;
    forEach(root, [&m](const Node& n) { m = std::max(m, n.id); });
    return m;
}

bool hasPlaceholder(const NodePtr& root)
{
    bool found = false;
    forEach(root, [&found](const Node& n) { found = found || n.kind == Kind::Placeholder; });
    return found;
}

NodePtr find(const NodePtr& root, NodeId id)
{
    if (root == nullptr)
        return nullptr;
    if (root->id == id)
        return root;
    for (const auto& item : root->items)
        if (auto hit = find(item, id)) return hit;
    for (const auto& slot : root->slots)
        if (auto hit = find(slot, id)) return hit;
    return nullptr;
}

bool sameStructure(const NodePtr& a, const NodePtr& b)
{
    if (a == nullptr || b == nullptr)
        return a == b;
    if (a->kind != b->kind || a->text != b->text || a->text2 != b->text2 || a->style != b->style
        || a->items.size() != b->items.size() || a->slots.size() != b->slots.size())
        return false;
    for (size_t i = 0; i < a->items.size(); ++i)
        if (!sameStructure(a->items[i], b->items[i])) return false;
    for (size_t i = 0; i < a->slots.size(); ++i)
        if (!sameStructure(a->slots[i], b->slots[i])) return false;
    return true;
}

namespace
{
bool validateNode(const NodePtr& node, int depth, std::set<NodeId>& seen, int& budget, juce::String& error)
{
    if (node == nullptr) { error = "A node is missing."; return false; }
    if (depth > kMaxDepth) { error = "Mathematics nested more than " + juce::String(kMaxDepth) + " levels deep."; return false; }
    if (--budget < 0) { error = "A math cell has more than " + juce::String(kMaxNodesPerCell) + " nodes."; return false; }
    if (node->id == 0) { error = "A node has no identifier."; return false; }
    if (!seen.insert(node->id).second) { error = "Node identifier " + juce::String((juce::int64) node->id) + " is used twice."; return false; }
    const auto kind = node->kind;
    if (kind == Kind::Row)
    {
        if (!node->slots.empty()) { error = "A row has slots."; return false; }
        for (const auto& item : node->items)
        {
            if (item != nullptr && item->kind == Kind::Row) { error = "A row directly contains a row."; return false; }
            if (!validateNode(item, depth + 1, seen, budget, error)) return false;
        }
        return true;
    }
    if (!node->items.empty()) { error = "A " + juce::String(kindName(kind)) + " has items."; return false; }
    if (isAtom(kind))
    {
        if (!node->slots.empty()) { error = "An atom has slots."; return false; }
        if (node->text.empty() || node->text.size() > (kind == Kind::Text ? 4096u : 64u)) { error = "An atom has empty or oversized text."; return false; }
        if (kind == Kind::Number)
            for (const char c : node->text)
                if (!((c >= '0' && c <= '9') || c == '.')) { error = "A number contains characters other than digits and a decimal point."; return false; }
        return true;
    }
    if (kind == Kind::Placeholder)
        return node->slots.empty() && node->text.empty() && node->text2.size() <= 256 ? true : (error = "A placeholder is malformed.", false);
    const int count = slotCount(kind);
    if ((int) node->slots.size() != count) { error = "A " + juce::String(kindName(kind)) + " has the wrong number of slots."; return false; }
    for (int i = 0; i < count; ++i)
    {
        const auto& slot = node->slots[(size_t) i];
        if (slot == nullptr)
        {
            if (!slotIsOptional(kind, i)) { error = "A required " + juce::String(kindName(kind)) + " slot is missing."; return false; }
            continue;
        }
        if (slot->kind != Kind::Row) { error = "A structure slot is not a row."; return false; }
        if (!validateNode(slot, depth + 1, seen, budget, error)) return false;
    }
    if (kind == Kind::Script && node->slots[1] == nullptr && node->slots[2] == nullptr) { error = "A script has neither a subscript nor a superscript."; return false; }
    if (kind == Kind::Delimited && (node->text.empty() || node->text.size() > 8 || node->text2.size() > 8)) { error = "A delimited group has bad delimiters."; return false; }
    if (kind == Kind::BigOperator && (node->text.empty() || node->text.size() > 8)) { error = "A large operator has no operator character."; return false; }
    return true;
}
}

bool validate(const NodePtr& root, juce::String& error)
{
    std::set<NodeId> seen;
    int budget = kMaxNodesPerCell;
    if (root == nullptr || root->kind != Kind::Row) { error = "A line must be a row."; return false; }
    return validateNode(root, 0, seen, budget, error);
}

// ---- identity across edits ------------------------------------------------------------------------------------------------
namespace
{
bool shallowSame(const Node& a, const Node& b)
{
    if (a.kind != b.kind)
        return false;
    if (isAtom(a.kind) || a.kind == Kind::BigOperator || a.kind == Kind::Delimited)
        if (a.text != b.text || (a.kind == Kind::Delimited && a.text2 != b.text2))
            return false;
    if (a.slots.size() != b.slots.size())
        return false;
    for (size_t i = 0; i < a.slots.size(); ++i)
        if ((a.slots[i] == nullptr) != (b.slots[i] == nullptr))
            return false;
    return true;
}

NodePtr adopt(const NodePtr& old, const NodePtr& fresh);

std::vector<NodePtr> adoptItems(const std::vector<NodePtr>& old, const std::vector<NodePtr>& fresh)
{
    size_t prefix = 0;
    while (prefix < old.size() && prefix < fresh.size() && shallowSame(*old[prefix], *fresh[prefix]))
        ++prefix;
    size_t suffix = 0;
    while (suffix < old.size() - prefix && suffix < fresh.size() - prefix
           && shallowSame(*old[old.size() - 1 - suffix], *fresh[fresh.size() - 1 - suffix]))
        ++suffix;
    std::vector<NodePtr> out;
    for (size_t i = 0; i < prefix; ++i)
        out.push_back(adopt(old[i], fresh[i]));
    const size_t oldMiddle = old.size() - prefix - suffix, freshMiddle = fresh.size() - prefix - suffix;
    for (size_t i = 0; i < freshMiddle; ++i)
        out.push_back(oldMiddle == freshMiddle && old[prefix + i]->kind == fresh[prefix + i]->kind ? adopt(old[prefix + i], fresh[prefix + i]) : fresh[prefix + i]);
    for (size_t i = 0; i < suffix; ++i)
        out.push_back(adopt(old[old.size() - suffix + i], fresh[fresh.size() - suffix + i]));
    return out;
}

NodePtr adopt(const NodePtr& old, const NodePtr& fresh)
{
    if (old == nullptr || fresh == nullptr || old->kind != fresh->kind)
        return fresh;
    auto node = std::make_shared<Node>(*fresh);
    node->id = old->id;
    if (fresh->kind == Kind::Row)
        node->items = adoptItems(old->items, fresh->items);
    else
        for (size_t i = 0; i < node->slots.size() && i < old->slots.size(); ++i)
            node->slots[i] = adopt(old->slots[i], fresh->slots[i]);
    return node;
}
}

NodePtr adoptIds(const NodePtr& old, const NodePtr& fresh) { return adopt(old, fresh); }

// ---- persistence ------------------------------------------------------------------------------------------------------------------------
juce::var toVar(const NodePtr& node)
{
    auto* o = new juce::DynamicObject();
    if (node == nullptr)
        return juce::var(o);
    o->setProperty("n", (juce::int64) node->id);
    o->setProperty("k", kindName(node->kind));
    if (node->kind == Kind::Row)
    {
        juce::Array<juce::var> items;
        for (const auto& item : node->items) items.add(toVar(item));
        o->setProperty("c", items);
        return juce::var(o);
    }
    if (isAtom(node->kind))
    {
        o->setProperty("t", juce::String::fromUTF8(node->text.c_str()));
        if (node->style != Style::Auto)
            o->setProperty("s", node->style == Style::Italic ? "italic" : node->style == Style::Upright ? "upright" : "bold");
        return juce::var(o);
    }
    if (node->kind == Kind::Placeholder)
    {
        if (!node->text2.empty()) o->setProperty("hint", juce::String::fromUTF8(node->text2.c_str()));
        return juce::var(o);
    }
    if (node->kind == Kind::Delimited)
    {
        o->setProperty("open", juce::String::fromUTF8(node->text.c_str()));
        o->setProperty("close", juce::String::fromUTF8(node->text2.c_str()));
    }
    if (node->kind == Kind::BigOperator)
        o->setProperty("op", juce::String::fromUTF8(node->text.c_str()));
    for (int i = 0; i < slotCount(node->kind); ++i)
        if (node->slots[(size_t) i] != nullptr)
            o->setProperty(slotKey(node->kind, i), toVar(node->slots[(size_t) i]));
    return juce::var(o);
}

namespace
{
bool parseNode(const juce::var& v, IdAllocator& ids, std::set<NodeId>& seen, int depth, int& budget, bool wantRow,
               NodePtr& out, juce::String& error)
{
    if (!v.isObject()) { error = "A math node is not an object."; return false; }
    if (depth > kMaxDepth) { error = "Mathematics nested more than " + juce::String(kMaxDepth) + " levels deep."; return false; }
    if (--budget < 0) { error = "A math cell has more than " + juce::String(kMaxNodesPerCell) + " nodes."; return false; }
    Kind kind;
    if (!kindFromName(v["k"].toString().toStdString(), kind)) { error = "Unknown math node kind '" + v["k"].toString() + "'."; return false; }
    if (wantRow && kind != Kind::Row) { error = "A structure slot is not a row."; return false; }
    const auto rawId = v["n"];
    if (!(rawId.isInt() || rawId.isInt64()) || (juce::int64) rawId <= 0) { error = "A math node has no valid identifier."; return false; }
    const auto id = (NodeId) (juce::int64) rawId;
    if (!seen.insert(id).second) { error = "Node identifier " + juce::String((juce::int64) id) + " is used twice."; return false; }
    ids.next = std::max(ids.next, id + 1);
    auto node = std::make_shared<Node>();
    node->id = id;
    node->kind = kind;
    if (kind == Kind::Row)
    {
        const auto* children = v["c"].getArray();
        if (children == nullptr) { error = "A row has no item list."; return false; }
        for (const auto& child : *children)
        {
            NodePtr item;
            if (!parseNode(child, ids, seen, depth + 1, budget, false, item, error)) return false;
            if (item->kind == Kind::Row) { error = "A row directly contains a row."; return false; }
            node->items.push_back(std::move(item));
        }
    }
    else if (isAtom(kind))
    {
        node->text = v["t"].toString().toStdString();
        const auto style = v["s"].toString();
        node->style = style == "italic" ? Style::Italic : style == "upright" ? Style::Upright : style == "bold" ? Style::Bold : Style::Auto;
    }
    else if (kind == Kind::Placeholder)
        node->text2 = v["hint"].toString().toStdString();
    else
    {
        if (kind == Kind::Delimited) { node->text = v["open"].toString().toStdString(); node->text2 = v["close"].toString().toStdString(); }
        if (kind == Kind::BigOperator) node->text = v["op"].toString().toStdString();
        for (int i = 0; i < slotCount(kind); ++i)
        {
            const auto slot = v[juce::Identifier(slotKey(kind, i))];
            if (slot.isVoid() || slot.isUndefined())
            {
                node->slots.push_back(nullptr);
                continue;
            }
            NodePtr row;
            if (!parseNode(slot, ids, seen, depth + 1, budget, true, row, error)) return false;
            node->slots.push_back(std::move(row));
        }
    }
    // The structural checks of validate(), without re-walking: rebuild a one-node view of the rules.
    NodePtr result = node;
    if (kind != Kind::Row)
    {
        std::set<NodeId> scratch;
        int scratchBudget = 4;
        // Only this node's own shape is checked here (children were checked as they were parsed).
        auto shallow = std::make_shared<Node>(*node);
        for (auto& slot : shallow->slots)
            if (slot != nullptr) slot = std::make_shared<Node>(Node { slot->id, Kind::Row, {}, {}, Style::Auto, {}, {} });
        if (!validateNode(shallow, 0, scratch, scratchBudget, error)) return false;
    }
    out = std::move(result);
    return true;
}
}

bool fromVar(const juce::var& value, IdAllocator& ids, std::set<NodeId>& seen, NodePtr& out, juce::String& error)
{
    int budget = kMaxNodesPerCell;
    NodePtr node;
    if (!parseNode(value, ids, seen, 0, budget, true, node, error))
        return false;
    out = std::move(node);
    return true;
}

juce::var toVar(const CellMath& cell)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("schema", kSchemaVersion);
    o->setProperty("role", roleName(cell.role));
    juce::Array<juce::var> lines;
    for (const auto& line : cell.lines) lines.add(toVar(line));
    o->setProperty("lines", lines);
    return juce::var(o);
}

bool fromVar(const juce::var& value, IdAllocator& ids, std::set<NodeId>& seen, CellMath& out, juce::String& error)
{
    if (!value.isObject() || (int) value["schema"] != kSchemaVersion) { error = "Unsupported math cell schema."; return false; }
    CellMath cell;
    if (!roleFromName(value["role"].toString().toStdString(), cell.role)) { error = "Unknown math cell role '" + value["role"].toString() + "'."; return false; }
    const auto* lines = value["lines"].getArray();
    if (lines == nullptr || lines->isEmpty() || lines->size() > kMaxLinesPerCell) { error = "A math cell needs 1 to " + juce::String(kMaxLinesPerCell) + " lines."; return false; }
    if (cell.role != Role::System && lines->size() != 1) { error = "Only a system has more than one line."; return false; }
    int total = 0;
    for (const auto& line : *lines)
    {
        NodePtr row;
        if (!fromVar(line, ids, seen, row, error)) return false;
        total += countNodes(row);
        if (total > kMaxNodesPerCell) { error = "A math cell has more than " + juce::String(kMaxNodesPerCell) + " nodes."; return false; }
        cell.lines.push_back(std::move(row));
    }
    out = std::move(cell);
    return true;
}
}
