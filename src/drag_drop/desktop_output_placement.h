#pragma once

#include "pending_drop_placement.h"
#include "common/constants.h"

namespace snowdesktop::desktop_output_placement
{
struct State
{
    std::vector<std::wstring>& ids;
    std::unordered_map<std::wstring, int>& columns;
    std::unordered_map<std::wstring, int>& rows;
    const std::vector<GridPage>& pages;
};
struct Services
{
    std::function<const GridPage*()> reference;
    std::function<bool(GridSpan, grid_occupancy::Slots&, GridCell&, const std::wstring&, int)> findFree;
    std::function<int(const GridCell&)> slot;
};

template<class Normalize, class Grouped, class Owned>
grid_occupancy::Slots Used(const std::vector<DesktopWidget>& widgets, const std::vector<DesktopItem>& items,
    const std::unordered_set<std::wstring>& claimed, const std::unordered_set<std::wstring>& existing,
    Normalize normalize, Grouped grouped, Owned owned)
{
    grid_occupancy::Slots used;
    for (const auto& widget : widgets)
        if (!grouped(widget)) grid_occupancy::Mark(used, widget.gridCell, widget.gridSpan);
    for (const auto& item : items)
    {
        const auto key = normalize(item.layoutKey);
        if (!key.empty() && !claimed.contains(key) && !existing.contains(key)) continue;
        if (!item.name.empty() && !owned(item)) grid_occupancy::Mark(used, item.gridCell, item.gridSpan);
    }
    return used;
}

// Actual host page registration and final output placement. Reference-monitor
// selection and the live visible-grid allocator remain explicit services.
inline bool Place(State state, DesktopItem& item, const PendingLandingEntry& landing,
    grid_occupancy::Slots& used, const Services& services)
{
    const auto& id = landing.cell.pageId;
    std::optional<GridPage> planned;
    if (!id.empty() && id != kDockPageId && std::find(state.ids.begin(), state.ids.end(), id) == state.ids.end())
    {
        GridSpan dimensions;
        if (landing.plannedPageSize)
        {
            dimensions = *landing.plannedPageSize;
            if (dimensions.columns < 1 || dimensions.rows < 1) return false;
        }
        else
        {
            // Older/non-grid pending entries have no captured virtual-page
            // geometry. Retain their reference-monitor fallback without an
            // early mutation when the reference is unavailable.
            const auto* reference = services.reference();
            if (!reference) return false;
            dimensions = {std::max(1, reference->columns), std::max(1, reference->rows)};
        }
        planned.emplace(); planned->id = id;
        planned->columns = dimensions.columns; planned->rows = dimensions.rows;
    }
    const bool placed = pending_drop::PlaceDesktop(item, landing, used,
        [&](const auto& pageId) -> std::optional<GridPage> {
            if (planned && planned->id == pageId) return planned;
            for (const auto& page : state.pages) if (page.id == pageId) return page;
            const auto columns = state.columns.find(pageId), rows = state.rows.find(pageId);
            if (columns == state.columns.end() || rows == state.rows.end()) return std::nullopt;
            GridPage page; page.id = pageId; page.columns = columns->second; page.rows = rows->second;
            return page;
        }, services.findFree, services.slot);
    // Failed placement or a legal fallback to an existing page must not
    // publish an unused virtual page merely because one was previewed.
    if (placed && planned && item.gridCell.pageId == planned->id)
    {
        state.ids.push_back(id);
        state.columns[id] = planned->columns; state.rows[id] = planned->rows;
    }
    return placed;
}
}
