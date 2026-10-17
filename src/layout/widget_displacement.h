#pragma once

#include "grid_occupancy.h"
#include <functional>
#include <unordered_map>

namespace snowdesktop::widget_displacement
{
inline GridSpan ClampSpan(const DesktopWidget& widget, GridSpan span, int columns, int rows)
{
    span.columns = std::max(1, span.columns);
    span.rows = std::max(1, span.rows);
    if (span.columns == 1 && span.rows == 1 && IsWidgetDockEntryType(DockEntryTypeForWidget(widget.type))) return span;
    const int maxColumns = widget.maxGridSpan.columns > 0 ? std::min(std::max(1, columns), widget.maxGridSpan.columns) : std::max(1, columns);
    const int maxRows = widget.maxGridSpan.rows > 0 ? std::min(std::max(1, rows), widget.maxGridSpan.rows) : std::max(1, rows);
    span.columns = std::clamp(span.columns, std::min(maxColumns, std::max(1, widget.minGridSpan.columns)), maxColumns);
    span.rows = std::clamp(span.rows, std::min(maxRows, std::max(1, widget.minGridSpan.rows)), maxRows);
    return span;
}

inline const GridPage* FindPage(const std::vector<GridPage>& pages, const std::wstring& id)
{
    const auto found = std::find_if(pages.begin(), pages.end(), [&](const auto& page) { return page.id == id; });
    return found == pages.end() ? nullptr : &*found;
}
inline int Slot(const std::vector<GridPage>& pages, const GridCell& cell)
{
    const auto* page = FindPage(pages, cell.pageId);
    return std::max(0, cell.column) * std::max(1, page ? page->rows : 1) + std::max(0, cell.row);
}
inline bool Overlaps(const GridCell& a, GridSpan as, const GridCell& b, GridSpan bs)
{
    return a.pageId == b.pageId && a.column < b.column + bs.columns && b.column < a.column + as.columns &&
        a.row < b.row + bs.rows && b.row < a.row + as.rows;
}
struct State
{
    std::vector<DesktopWidget>& widgets;
    std::vector<DesktopItem>& items;
    const std::vector<GridPage>& pages;
    const std::vector<size_t>& pageOrder;
    std::vector<std::wstring>& savedIds;
    std::unordered_map<std::wstring, int>& columns;
    std::unordered_map<std::wstring, int>& rows;
};
struct Services
{
    std::function<bool(const DesktopWidget&)> grouped;
    std::function<bool(const DesktopItem&)> owned;
    std::function<std::wstring()> generatePageId;
    std::function<void()> committed;
};

// Actual component placement and item displacement transaction. Keep proposed
// coordinates/page records local until every displaced item has a legal place.
// Runtime membership/cache lookup, page naming and final host refresh are services.
inline bool Place(State state, size_t widgetIndex, GridCell targetCell, GridSpan targetSpan,
    bool isMove, const Services& services)
{
    if (widgetIndex >= state.widgets.size()) return false;
    const auto* page = FindPage(state.pages, targetCell.pageId);
    if (!page) return false;
    targetSpan = ClampSpan(state.widgets[widgetIndex], targetSpan,
        page->columns - targetCell.column, page->rows - targetCell.row);
    if (!grid_occupancy::Fits(*page, targetCell, targetSpan)) return false;
    for (size_t index = 0; index < state.widgets.size(); ++index)
        if (index != widgetIndex && !services.grouped(state.widgets[index]) &&
            Overlaps(targetCell, targetSpan, state.widgets[index].gridCell, state.widgets[index].gridSpan)) return false;

    struct Position { GridCell cell; int slot; };
    std::vector<Position> positions;
    positions.reserve(state.items.size());
    std::vector<size_t> displaced;
    for (size_t index = 0; index < state.items.size(); ++index)
    {
        const auto& item = state.items[index];
        positions.push_back({item.gridCell, item.slot});
        if (!item.name.empty() && !services.owned(item) && Overlaps(targetCell, targetSpan, item.gridCell, item.gridSpan))
            displaced.push_back(index);
    }
    const auto oldCell = state.widgets[widgetIndex].gridCell;
    const auto oldSpan = state.widgets[widgetIndex].gridSpan;
    grid_occupancy::Slots used;
    for (size_t index = 0; index < state.widgets.size(); ++index)
        if (index != widgetIndex && !services.grouped(state.widgets[index]))
            grid_occupancy::Mark(used, state.widgets[index].gridCell, state.widgets[index].gridSpan);
    grid_occupancy::Mark(used, targetCell, targetSpan);
    for (size_t index = 0; index < state.items.size(); ++index)
    {
        const auto& item = state.items[index];
        if (item.name.empty() || services.owned(item) || std::find(displaced.begin(), displaced.end(), index) != displaced.end()) continue;
        if (isMove && Overlaps(item.gridCell, item.gridSpan, oldCell, oldSpan)) continue;
        grid_occupancy::Mark(used, item.gridCell, item.gridSpan);
    }

    auto savedIds = state.savedIds;
    auto columns = state.columns;
    auto rows = state.rows;
    std::unordered_map<std::wstring, int> newPageSlots;
    auto overflow = [&](GridSpan span, GridCell& found) {
        auto trySaved = [&](const std::wstring& id, int cols, int rowCount, int start, int* next) {
            GridPage dimensions; dimensions.id = id; dimensions.columns = cols; dimensions.rows = rowCount;
            for (int slot = start; slot < cols * rowCount; ++slot)
            {
                GridCell candidate{id, slot / std::max(1, rowCount), slot % std::max(1, rowCount)};
                if (grid_occupancy::Fits(dimensions, candidate, span) && !grid_occupancy::Occupied(used, candidate, span))
                {
                    found = candidate;
                    if (next) *next = slot + 1;
                    return true;
                }
            }
            return false;
        };
        for (const auto& id : savedIds)
            if (!FindPage(state.pages, id) && columns.contains(id) && rows.contains(id) &&
                trySaved(id, columns.at(id), rows.at(id), 0, nullptr)) return true;
        for (auto& [id, next] : newPageSlots)
            if (trySaved(id, columns.at(id), rows.at(id), next, &next)) return true;
        if (state.pageOrder.empty() || state.pageOrder.back() >= state.pages.size()) return false;
        const auto& last = state.pages[state.pageOrder.back()];
        if (span.columns < 1 || span.rows < 1 || span.columns > last.columns || span.rows > last.rows) return false;
        const auto id = services.generatePageId();
        if (id.empty() || std::find(savedIds.begin(), savedIds.end(), id) != savedIds.end()) return false;
        savedIds.push_back(id); columns[id] = last.columns; rows[id] = last.rows;
        found = {id, 0, 0}; newPageSlots[id] = 1;
        return true;
    };
    std::sort(displaced.begin(), displaced.end(), [&](size_t a, size_t b) {
        if (state.items[a].gridCell.pageId != state.items[b].gridCell.pageId)
            return state.items[a].gridCell.pageId < state.items[b].gridCell.pageId;
        return Slot(state.pages, state.items[a].gridCell) < Slot(state.pages, state.items[b].gridCell);
    });
    for (size_t index : displaced)
    {
        const auto span = state.items[index].gridSpan;
        GridCell found;
        const bool placed = isMove
            ? grid_occupancy::FindFree(state.pages, state.pageOrder, span, used, found, oldCell.pageId, Slot(state.pages, oldCell)) ||
                grid_occupancy::FindFree(state.pages, state.pageOrder, span, used, found, targetCell.pageId, 0)
            : grid_occupancy::FindFree(state.pages, state.pageOrder, span, used, found, targetCell.pageId,
                Slot(state.pages, targetCell) + std::max(1, targetSpan.rows));
        if (!placed && !overflow(span, found)) return false;
        positions[index] = {found, Slot(state.pages, found)};
        grid_occupancy::Mark(used, found, span);
    }
    state.widgets[widgetIndex].gridCell = targetCell;
    state.widgets[widgetIndex].gridSpan = targetSpan;
    for (size_t index = 0; index < positions.size(); ++index)
    {
        state.items[index].gridCell = positions[index].cell;
        state.items[index].slot = positions[index].slot;
    }
    state.savedIds = std::move(savedIds); state.columns = std::move(columns); state.rows = std::move(rows);
    if (state.widgets[widgetIndex].type == DesktopWidgetType::Collection && state.widgets[widgetIndex].scrollContainerMode &&
        (targetSpan.columns < 2 || targetSpan.rows < 2))
    { state.widgets[widgetIndex].scrollContainerMode = false; state.widgets[widgetIndex].scrollOffset = 0; }
    services.committed();
    return true;
}
}
