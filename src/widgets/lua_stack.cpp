#include "lua_stack.h"

#include <algorithm>
#include <set>

namespace snowdesktop::lua_stack
{
bool IsSurface(DesktopWidgetType type) noexcept
{
    return type == DesktopWidgetType::LuaScript || type == DesktopWidgetType::LuaStack;
}

size_t Owner(const std::vector<DesktopWidget>& widgets, std::wstring_view member)
{
    for (size_t i = 0; i < widgets.size(); ++i)
        if (widgets[i].type == DesktopWidgetType::LuaStack &&
            std::find(widgets[i].childWidgetIds.begin(), widgets[i].childWidgetIds.end(), member) !=
                widgets[i].childWidgetIds.end()) return i;
    return widgets.size();
}

size_t Active(const std::vector<DesktopWidget>& widgets, size_t owner)
{
    if (owner >= widgets.size()) return widgets.size();
    const auto& widget = widgets[owner];
    if (widget.type == DesktopWidgetType::LuaScript) return owner;
    if (widget.type != DesktopWidgetType::LuaStack) return widgets.size();
    const auto find = [&](const std::wstring& id) {
        const auto it = std::find_if(widgets.begin(), widgets.end(), [&](const auto& child) {
            return child.id == id && child.type == DesktopWidgetType::LuaScript;
        });
        return static_cast<size_t>(it - widgets.begin());
    };
    if (std::find(widget.childWidgetIds.begin(), widget.childWidgetIds.end(), widget.activeCategoryId) !=
            widget.childWidgetIds.end())
        if (const size_t active = find(widget.activeCategoryId); active < widgets.size()) return active;
    for (const auto& id : widget.childWidgetIds)
        if (const size_t active = find(id); active < widgets.size()) return active;
    return widgets.size();
}

std::vector<std::wstring> Members(const DesktopWidget& widget)
{
    return widget.type == DesktopWidgetType::LuaStack ? widget.childWidgetIds
        : std::vector<std::wstring>{widget.id};
}

bool Supports(const DesktopWidget& widget, GridSpan span) noexcept
{
    return span.columns >= widget.minGridSpan.columns && span.rows >= widget.minGridSpan.rows &&
        (!widget.maxGridSpan.columns || span.columns <= widget.maxGridSpan.columns) &&
        (!widget.maxGridSpan.rows || span.rows <= widget.maxGridSpan.rows);
}

bool CanCombine(const std::vector<DesktopWidget>& widgets, size_t source, size_t target,
    bool checkCachedSizes)
{
    if (source >= widgets.size() || target >= widgets.size() || source == target ||
        !IsSurface(widgets[source].type) || !IsSurface(widgets[target].type) ||
        Owner(widgets, widgets[source].id) < widgets.size() ||
        Owner(widgets, widgets[target].id) < widgets.size()) return false;
    const auto destination = Members(widgets[target]);
    const auto incoming = Members(widgets[source]);
    if (destination.empty() || incoming.empty()) return false;
    std::set<std::wstring> unique;
    for (const auto& ids : {destination, incoming})
        for (const auto& id : ids)
        {
            const auto child = std::find_if(widgets.begin(), widgets.end(), [&](const auto& value) {
                return value.id == id && value.type == DesktopWidgetType::LuaScript;
            });
            if (id.empty() || !unique.insert(id).second || child == widgets.end() ||
                (checkCachedSizes && !Supports(*child, widgets[target].gridSpan))) return false;
        }
    return true;
}

bool Combine(std::vector<DesktopWidget>& widgets, size_t source, size_t target, std::wstring newId)
{
    if (!CanCombine(widgets, source, target)) return false;
    auto destination = Members(widgets[target]);
    const auto incoming = Members(widgets[source]);
    const std::wstring sourceId = widgets[source].id;
    const bool removeSource = widgets[source].type == DesktopWidgetType::LuaStack;
    if (widgets[target].type == DesktopWidgetType::LuaScript)
    {
        if (newId.empty() || std::any_of(widgets.begin(), widgets.end(), [&](const auto& w) { return w.id == newId; }))
            return false;
        DesktopWidget wrapper;
        wrapper.id = std::move(newId);
        wrapper.type = DesktopWidgetType::LuaStack;
        wrapper.gridCell = widgets[target].gridCell;
        wrapper.gridSpan = widgets[target].gridSpan;
        wrapper.showOnHoverOnly = widgets[target].showOnHoverOnly;
        wrapper.keepWhenDesktopHidden = widgets[target].keepWhenDesktopHidden;
        wrapper.activeCategoryId = widgets[target].id;
        wrapper.childWidgetIds = destination;
        widgets.push_back(std::move(wrapper));
        target = widgets.size() - 1;
    }
    auto& wrapper = widgets[target];
    wrapper.childWidgetIds.insert(wrapper.childWidgetIds.end(), incoming.begin(), incoming.end());
    for (auto& child : widgets)
        if (std::find(wrapper.childWidgetIds.begin(), wrapper.childWidgetIds.end(), child.id) != wrapper.childWidgetIds.end())
            child.selected = false;
    wrapper.selected = true;
    if (removeSource) std::erase_if(widgets, [&](const auto& value) { return value.id == sourceId; });
    return true;
}

void Normalize(std::vector<DesktopWidget>& widgets)
{
    std::set<std::wstring> claimed;
    for (auto& wrapper : widgets)
    {
        if (wrapper.type != DesktopWidgetType::LuaStack) continue;
        std::erase_if(wrapper.childWidgetIds, [&](const auto& id) {
            const auto child = std::find_if(widgets.begin(), widgets.end(), [&](const auto& w) {
                return w.id == id && w.type == DesktopWidgetType::LuaScript;
            });
            return child == widgets.end() || !claimed.insert(id).second;
        });
        if (std::find(wrapper.childWidgetIds.begin(), wrapper.childWidgetIds.end(), wrapper.activeCategoryId) == wrapper.childWidgetIds.end())
            wrapper.activeCategoryId = wrapper.childWidgetIds.empty() ? L"" : wrapper.childWidgetIds.front();
    }
}

void Dissolve(std::vector<DesktopWidget>& widgets)
{
    for (size_t i = widgets.size(); i-- > 0;)
    {
        const auto& wrapper = widgets[i];
        if (wrapper.type != DesktopWidgetType::LuaStack || wrapper.childWidgetIds.size() > 1) continue;
        if (!wrapper.childWidgetIds.empty())
            for (auto& child : widgets)
                if (child.id == wrapper.childWidgetIds.front() && child.type == DesktopWidgetType::LuaScript)
                {
                    child.gridCell = wrapper.gridCell;
                    child.gridSpan = wrapper.gridSpan;
                    child.selected = wrapper.selected;
                    child.showOnHoverOnly = wrapper.showOnHoverOnly;
                    child.keepWhenDesktopHidden = wrapper.keepWhenDesktopHidden;
                    break;
                }
        widgets.erase(widgets.begin() + static_cast<std::ptrdiff_t>(i));
    }
}
}
