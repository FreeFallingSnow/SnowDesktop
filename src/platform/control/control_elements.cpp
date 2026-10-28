#include "control_elements.h"
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

namespace snowdesktop::control
{
std::string ElementKey(std::string_view text)
{
    constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    for (const unsigned char value : text)
    {
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_') encoded += static_cast<char>(value);
        else { encoded += '%'; encoded += hex[value >> 4]; encoded += hex[value & 15]; }
    }
    return encoded;
}
RECT VisibleBounds(const Element& element)
{
    RECT result{};
    IntersectRect(&result, &element.bounds, &element.clip);
    return result;
}
POINT ElementCenter(const Element& element)
{
    const auto bounds = VisibleBounds(element);
    return {bounds.left + (bounds.right - bounds.left) / 2, bounds.top + (bounds.bottom - bounds.top) / 2};
}
bool Supports(const Element& element, std::string_view action)
{
    return std::find(element.actions.begin(), element.actions.end(), action) != element.actions.end();
}
Value ElementJson(const Element& element)
{
    const auto rect = [](RECT value) { return Object({{"left", Number(value.left)}, {"top", Number(value.top)},
        {"right", Number(value.right)}, {"bottom", Number(value.bottom)}}); };
    auto actions = Array();
    const auto visibleBounds = VisibleBounds(element);
    for (const auto& action : element.actions) actions.array.push_back(String(action));
    auto value = Object({{"id", String(element.id)}, {"parentId", String(element.parent)},
        {"surface", String(element.surface)}, {"role", String(element.role)}, {"name", String(element.name)},
        {"bounds", rect(element.bounds)}, {"clip", rect(element.clip)},
        {"enabled", Boolean(element.enabled)}, {"visible", Boolean(element.visible && !IsRectEmpty(&visibleBounds))},
        {"focused", Boolean(element.focused)}, {"selected", Boolean(element.selected)},
        {"password", Boolean(element.password)}, {"actions", std::move(actions)}});
    if (!element.password) value.object.emplace("value", String(element.value));
    if (element.checked) value.object.emplace("checked", Boolean(*element.checked));
    if (element.expanded) value.object.emplace("expanded", Boolean(*element.expanded));
    return value;
}
void ElementSnapshot::Finish()
{
    std::unordered_set<std::string> ids;
    auto identity = Array();
    for (const auto& element : elements)
    {
        if (element.id.empty() || !ids.insert(element.id).second)
            throw Error("ambiguous_element", "Element catalog contains a duplicate identity");
        identity.array.push_back(ElementJson(element));
    }
    for (const auto& element : elements)
        if (!element.parent.empty() && !ids.contains(element.parent))
            throw Error("invalid_element_tree", "Element parent is missing");
    std::unordered_map<std::string_view, std::string_view> parents;
    for (const auto& element : elements) parents.emplace(element.id, element.parent);
    for (const auto& element : elements)
    {
        std::string_view parent = element.parent;
        for (std::size_t depth = 0; !parent.empty(); ++depth)
        {
            if (depth >= elements.size()) throw Error("invalid_element_tree", "Element parents contain a cycle");
            parent = parents.at(parent);
        }
    }
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : Serialize(identity)) { hash ^= byte; hash *= 1099511628211ULL; }
    revision = std::to_string(hash);
}
Value ElementSnapshot::ToJson() const
{
    auto nodes = Array(), covered = Array(), pending = Array();
    for (const auto& element : elements) nodes.array.push_back(ElementJson(element));
    for (const auto& surface : coveredSurfaces) covered.array.push_back(String(surface));
    for (const auto& surface : pendingSurfaces) pending.array.push_back(String(surface));
    return Object({{"snapshotRevision", String(revision)}, {"coordinateSpace", String("physical-screen-pixels")},
        {"elements", std::move(nodes)}, {"coveredSurfaces", std::move(covered)}, {"pendingSurfaces", std::move(pending)}});
}
const Element& ElementSnapshot::Resolve(std::string_view id, std::string_view action) const
{
    const auto found = std::find_if(elements.begin(), elements.end(), [&](const auto& entry) { return entry.id == id; });
    if (found == elements.end()) throw Error("element_not_found", "Element is absent from the current snapshot");
    if (!action.empty())
    {
        if (!found->enabled) throw Error("element_disabled", "Element is disabled");
        const auto bounds = VisibleBounds(*found);
        if (!found->visible || IsRectEmpty(&bounds)) throw Error("element_not_visible", "Element is not visible");
        if (!Supports(*found, action)) throw Error("unsupported_action", "Element does not support this action");
    }
    return *found;
}
}
