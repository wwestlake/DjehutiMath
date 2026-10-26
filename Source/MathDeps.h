#pragma once

// The dependency graph of a worksheet's mathematics: which cell defines which names, which cells use them, in
// what order they can be computed, where definitions are circular or conflicting, and which results are still
// current after an edit. It knows only strings and revisions: no document, GUI or solver dependency.
//
// Variables and functions share one namespace of names. A cell depends on every cell that defines a name it
// uses (including itself: `x = x + 1` is a cycle). Worksheet order does not matter: a cell may use a name
// defined further down.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace math_deps
{
struct CellNames
{
    std::string id;
    std::uint64_t revision = 0;
    std::set<std::string> defines, declares, uses;   // `declares`: assumptions about a name (they are not definitions)
};

struct Conflict
{
    std::string name;
    std::vector<std::string> cells;     // more than one cell defines the name
};

class Graph
{
public:
    // Adds the cell or replaces its entry. Returns true when anything about it changed (revision, names).
    bool set(const CellNames& cell);
    bool remove(const std::string& id);
    bool has(const std::string& id) const { return cells.count(id) > 0; }
    std::vector<std::string> ids() const;                                       // sorted
    const CellNames* find(const std::string& id) const;

    std::vector<std::string> definers(const std::string& name) const;           // cells defining the name
    std::vector<std::string> providers(const std::string& name) const;         // cells defining or declaring the name
    std::set<std::string> dependenciesOf(const std::string& id) const;          // direct
    std::set<std::string> dependentsOf(const std::string& id) const;            // direct
    std::set<std::string> transitiveDependencies(const std::string& id) const;
    std::set<std::string> transitiveDependents(const std::string& id) const;    // everything a change to `id` invalidates

    // Groups of cells that depend on each other (and cells that depend on themselves), each sorted.
    std::vector<std::vector<std::string>> cycles() const;
    bool inCycle(const std::string& id) const;
    // True when the cell is, or depends (transitively) on, a cycle: its value cannot be computed.
    bool blocked(const std::string& id) const;
    std::vector<Conflict> conflicts() const;
    // Every cell, dependencies first. Members of a cycle are adjacent; ties are broken by id.
    std::vector<std::string> order() const;

    // Identifies the exact state a result was computed from: the cell's revision and the stamps of everything
    // it depends on. A stored result is current only while its stamp still equals this.
    std::uint64_t stamp(const std::string& id) const;

private:
    std::map<std::string, CellNames> cells;
    void components(std::vector<std::vector<std::string>>& out) const;
};
}
