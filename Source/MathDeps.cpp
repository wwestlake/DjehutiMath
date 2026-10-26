#include "MathDeps.h"

#include <algorithm>
#include <functional>

namespace math_deps
{
namespace
{
std::uint64_t mix(std::uint64_t h, std::uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

std::uint64_t hashString(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}
}

bool Graph::set(const CellNames& cell)
{
    const auto it = cells.find(cell.id);
    if (it != cells.end() && it->second.revision == cell.revision && it->second.defines == cell.defines && it->second.declares == cell.declares && it->second.uses == cell.uses)
        return false;
    cells[cell.id] = cell;
    return true;
}

bool Graph::remove(const std::string& id) { return cells.erase(id) > 0; }

std::vector<std::string> Graph::ids() const
{
    std::vector<std::string> out;
    for (const auto& c : cells) out.push_back(c.first);
    return out;
}

const CellNames* Graph::find(const std::string& id) const
{
    const auto it = cells.find(id);
    return it == cells.end() ? nullptr : &it->second;
}

std::vector<std::string> Graph::definers(const std::string& name) const
{
    std::vector<std::string> out;
    for (const auto& c : cells)
        if (c.second.defines.count(name) > 0) out.push_back(c.first);
    return out;
}

std::vector<std::string> Graph::providers(const std::string& name) const
{
    std::vector<std::string> out;
    for (const auto& c : cells)
        if (c.second.defines.count(name) > 0 || c.second.declares.count(name) > 0) out.push_back(c.first);
    return out;
}

std::set<std::string> Graph::dependenciesOf(const std::string& id) const
{
    std::set<std::string> out;
    const auto* cell = find(id);
    if (cell == nullptr) return out;
    for (const auto& name : cell->uses)
        for (const auto& d : providers(name)) out.insert(d);
    return out;
}

std::set<std::string> Graph::dependentsOf(const std::string& id) const
{
    std::set<std::string> out;
    const auto* cell = find(id);
    if (cell == nullptr) return out;
    for (const auto& other : cells)
    {
        for (const auto& name : cell->defines)
            if (other.second.uses.count(name) > 0) { out.insert(other.first); break; }
        for (const auto& name : cell->declares)
            if (other.second.uses.count(name) > 0) { out.insert(other.first); break; }
    }
    return out;
}

std::set<std::string> Graph::transitiveDependencies(const std::string& id) const
{
    std::set<std::string> seen;
    std::vector<std::string> pending(1, id);
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        for (const auto& d : dependenciesOf(current))
            if (seen.insert(d).second) pending.push_back(d);
    }
    return seen;
}

std::set<std::string> Graph::transitiveDependents(const std::string& id) const
{
    std::set<std::string> seen;
    std::vector<std::string> pending(1, id);
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        for (const auto& d : dependentsOf(current))
            if (seen.insert(d).second) pending.push_back(d);
    }
    return seen;
}

// Tarjan's strongly connected components over the edges cell -> its dependencies. A component is emitted only
// after every component it depends on, which is exactly dependencies-first order.
void Graph::components(std::vector<std::vector<std::string>>& out) const
{
    std::map<std::string, int> index, low;
    std::map<std::string, bool> onStack;
    std::vector<std::string> stack;
    int counter = 0;
    std::map<std::string, std::set<std::string>> edges;
    for (const auto& c : cells) edges[c.first] = dependenciesOf(c.first);

    std::function<void(const std::string&)> visit = [&](const std::string& v) {
        index[v] = low[v] = counter++;
        stack.push_back(v);
        onStack[v] = true;
        for (const auto& w : edges[v])
        {
            if (index.count(w) == 0) { visit(w); low[v] = std::min(low[v], low[w]); }
            else if (onStack[w]) low[v] = std::min(low[v], index[w]);
        }
        if (low[v] == index[v])
        {
            std::vector<std::string> component;
            for (;;)
            {
                const auto w = stack.back();
                stack.pop_back();
                onStack[w] = false;
                component.push_back(w);
                if (w == v) break;
            }
            std::sort(component.begin(), component.end());
            out.push_back(std::move(component));
        }
    };
    for (const auto& c : cells)
        if (index.count(c.first) == 0) visit(c.first);
}

std::vector<std::vector<std::string>> Graph::cycles() const
{
    std::vector<std::vector<std::string>> all, out;
    components(all);
    for (auto& component : all)
    {
        if (component.size() > 1) out.push_back(component);
        else if (dependenciesOf(component[0]).count(component[0]) > 0) out.push_back(component);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Graph::inCycle(const std::string& id) const
{
    for (const auto& c : cycles())
        if (std::find(c.begin(), c.end(), id) != c.end()) return true;
    return false;
}

bool Graph::blocked(const std::string& id) const
{
    if (inCycle(id)) return true;
    for (const auto& d : transitiveDependencies(id))
        if (inCycle(d)) return true;
    return false;
}

std::vector<Conflict> Graph::conflicts() const
{
    std::map<std::string, std::vector<std::string>> byName;
    for (const auto& c : cells)
        for (const auto& name : c.second.defines) byName[name].push_back(c.first);
    std::vector<Conflict> out;
    for (auto& entry : byName)
        if (entry.second.size() > 1) out.push_back({ entry.first, std::move(entry.second) });
    return out;
}

std::vector<std::string> Graph::order() const
{
    std::vector<std::vector<std::string>> all;
    components(all);
    std::vector<std::string> out;
    for (const auto& component : all)
        for (const auto& id : component) out.push_back(id);
    return out;
}

std::uint64_t Graph::stamp(const std::string& id) const
{
    std::map<std::string, std::uint64_t> memo;
    std::set<std::string> active;
    std::function<std::uint64_t(const std::string&)> compute = [&](const std::string& v) -> std::uint64_t {
        const auto cached = memo.find(v);
        if (cached != memo.end()) return cached->second;
        const auto* cell = find(v);
        if (cell == nullptr) return 0;
        if (!active.insert(v).second) return 0x7f4a7c15ull;      // a cycle: a stable constant
        std::uint64_t h = mix(hashString(v), cell->revision);
        for (const auto& d : dependenciesOf(v))
            h = mix(h, d == v ? 0x51ull : compute(d));
        active.erase(v);
        memo[v] = h;
        return h;
    };
    return compute(id);
}
}
