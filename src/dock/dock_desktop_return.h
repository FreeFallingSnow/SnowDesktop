#pragma once

#include "common/constants.h"
#include "common/types.h"
#include "layout/grid_occupancy.h"
#include <algorithm>
#include <functional>
#include <unordered_set>

namespace snowdesktop::dock_desktop_return
{
struct State
{
    const std::vector<DockEntry>& entries;
    const std::vector<DesktopWidget>& widgets;
    const std::vector<DesktopItem>& items;
    const std::vector<GridPage>& pages;
};
struct Services
{
    std::function<std::wstring(const std::wstring&)> normalize;
    std::function<bool(const DesktopWidget&)> grouped;
    std::function<bool(const DesktopItem&)> owned;
    std::function<size_t(const std::wstring&)> itemIndex;
};
struct Landing
{
    size_t entryIndex = 0;
    DockEntryType type = DockEntryType::DesktopItem;
    std::wstring reference;
    GridCell cell;
    GridSpan span;
    bool accepted = false;
};

inline size_t FindWidget(const std::vector<DesktopWidget>& widgets, const std::wstring& id)
{
    for (size_t index = 0; index < widgets.size(); ++index)
        if (widgets[index].id == id) return index;
    return widgets.size();
}

// Shared by the actual Dock preview and final return. An accepted landing is
// an existing identity; no physical file operation is part of this transaction.
inline std::vector<Landing> Plan(State state, std::vector<size_t> indices, GridCell target, const Services& services)
{
    const auto page = std::find_if(state.pages.begin(), state.pages.end(), [&](const auto& value) { return value.id == target.pageId; });
    if (page == state.pages.end()) return {};
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    std::erase_if(indices, [&](size_t index) { return index >= state.entries.size(); });
    std::unordered_set<std::wstring> movingKeys;
    for (size_t index : indices)
        if (state.entries[index].type == DockEntryType::DesktopItem) movingKeys.insert(services.normalize(state.entries[index].reference));

    grid_occupancy::Slots used;
    for (const auto& widget : state.widgets)
        if (!services.grouped(widget) && widget.gridCell.pageId != kDockPageId)
            grid_occupancy::Mark(used, widget.gridCell, widget.gridSpan);
    for (const auto& item : state.items)
        if (!item.name.empty() && item.gridCell.pageId != kDockPageId && !services.owned(item) &&
            !movingKeys.contains(services.normalize(item.layoutKey)))
            grid_occupancy::Mark(used, item.gridCell, item.gridSpan);

    int slot = std::max(0, target.column) * std::max(1, page->rows) + std::max(0, target.row);
    std::vector<Landing> result;
    for (size_t index : indices)
    {
        const auto& entry = state.entries[index];
        const auto widgetIndex = IsWidgetDockEntryType(entry.type) ? FindWidget(state.widgets, entry.reference) : state.widgets.size();
        const auto itemIndex = entry.type == DockEntryType::DesktopItem ? services.itemIndex(entry.reference) : state.items.size();
        Landing landing{index, entry.type, entry.reference, {target.pageId, 0, 0}, {1, 1}, false};
        if (widgetIndex < state.widgets.size()) landing.span = state.widgets[widgetIndex].gridSpan;
        else if (itemIndex < state.items.size()) landing.span = state.items[itemIndex].gridSpan;
        landing.cell.column = std::clamp(slot / std::max(1, page->rows), 0, std::max(0, page->columns - std::max(1, landing.span.columns)));
        landing.cell.row = std::clamp(slot % std::max(1, page->rows), 0, std::max(0, page->rows - std::max(1, landing.span.rows)));
        if (grid_occupancy::Fits(*page, landing.cell, landing.span) && !grid_occupancy::Occupied(used, landing.cell, landing.span))
        {
            grid_occupancy::Mark(used, landing.cell, landing.span);
            ++slot;
            landing.accepted = widgetIndex < state.widgets.size() || itemIndex < state.items.size();
        }
        result.push_back(std::move(landing));
    }
    return result;
}

inline bool Apply(std::vector<DockEntry>& entries, std::vector<DesktopWidget>& widgets,
    std::vector<DesktopItem>& items, const std::vector<Landing>& landings, const Services& services)
{
    std::vector<size_t> restored;
    for (const auto& landing : landings)
    {
        if (!landing.accepted || landing.entryIndex >= entries.size()) continue;
        const auto& entry = entries[landing.entryIndex];
        if (entry.type != landing.type || services.normalize(entry.reference) != services.normalize(landing.reference)) continue;
        if (entry.type == DockEntryType::DesktopItem)
        {
            const auto index = services.itemIndex(entry.reference);
            if (index >= items.size()) continue;
            items[index].gridCell = landing.cell;
        }
        else
        {
            const auto index = FindWidget(widgets, entry.reference);
            if (index >= widgets.size()) continue;
            widgets[index].gridCell = landing.cell;
        }
        restored.push_back(landing.entryIndex);
    }
    std::sort(restored.begin(), restored.end());
    restored.erase(std::unique(restored.begin(), restored.end()), restored.end());
    for (auto it = restored.rbegin(); it != restored.rend(); ++it)
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*it));
    return !restored.empty();
}
}
