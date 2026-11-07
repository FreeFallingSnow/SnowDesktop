#pragma once

#include "core/drop_model.h"
#include "layout/grid_occupancy.h"
#include <functional>
#include <limits>

namespace snowdesktop::desktop_landings
{
struct Services
{
    std::function<bool(const DesktopWidget&)> grouped;
    std::function<bool(const DesktopItem&)> owned;
    std::function<std::wstring(const std::wstring&)> normalize;
    std::function<bool(GridSpan, grid_occupancy::Slots&, GridCell&, const std::wstring&, int)> findFree;
    std::function<std::wstring(const std::vector<std::wstring>&)> generateId;
};

// Actual host planner for files entering the desktop grid. The separate
// selected-desktop translation route remains in BuildDesktopLandings.
inline std::vector<DropLanding> Build(const DragSourceList& sourceList, GridCell targetCell, bool internalMove,
    const GridPage& requestedPage, const std::vector<DesktopWidget>& widgets_, const std::vector<DesktopItem>& items_,
    const std::unordered_map<std::wstring, int>& savedPageColumns_, const std::unordered_map<std::wstring, int>& savedPageRows_,
    const Services& services)
{
    std::vector<DropLanding> landings;
    const auto* page = &requestedPage;
    auto pageColumnsById = savedPageColumns_, pageRowsById = savedPageRows_;
    std::vector<std::wstring> reservedIds;
    std::unordered_map<std::wstring, GridSpan> plannedPages;
    // 起始列：拖放目标列，换行时从该列另起一行而非从 0 开始
    const int startCol = std::clamp(targetCell.column, 0, std::max(0, page->columns - 1));

    auto advanceCell = [&](GridCell cell, GridSpan span) {
        // 查找 cell 所在页的维度（支持跨页后 cursor 切到新页）
        int cols = page->columns, rows = page->rows;
        if (cell.pageId != page->id)
        {
            auto colIt = pageColumnsById.find(cell.pageId);
            auto rowIt = pageRowsById.find(cell.pageId);
            if (colIt != pageColumnsById.end()) cols = colIt->second;
            if (rowIt != pageRowsById.end()) rows = rowIt->second;
        }
        cell.column += std::max(1, span.columns);
        if (cell.column + span.columns > cols)
        {
            cell.column = startCol;
            cell.row += std::max(1, span.rows);
            // 到底后绕回起始行上方（搜索阶段会跳过已占位置）
            if (cell.row + span.rows > rows)
                cell.row = 0;
        }
        return cell;
    };

    std::unordered_set<std::wstring> sourceKeys;
    for (const auto& entry : sourceList.entries)
        if (!entry.desktopKey.empty())
            sourceKeys.insert(services.normalize(entry.desktopKey));

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!services.grouped(w))
            grid_occupancy::Mark(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (services.owned(item)) continue;
        if (internalMove && sourceKeys.contains(services.normalize(item.layoutKey))) continue;
        grid_occupancy::Mark(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell cursor = targetCell;

    // Prefer the closest available anchor on this page. Row wrapping can
    // otherwise send a drop in a component gap to the opposite screen edge.
    // Weight each axis by its actual pitch so non-square grids remain local.
    auto tryPlaceNearest = [&](GridSpan span, GridCell fromCell, GridCell& outCell) -> bool {
        int pageCols = page->columns, pageRows = page->rows;
        if (fromCell.pageId != page->id)
        {
            if (const auto it = pageColumnsById.find(fromCell.pageId); it != pageColumnsById.end())
                pageCols = it->second;
            if (const auto it = pageRowsById.find(fromCell.pageId); it != pageRowsById.end())
                pageRows = it->second;
        }
        const long long pitchX = std::max(1, page->itemPitchWidth);
        const long long pitchY = std::max(1, page->itemPitchHeight);
        long long bestDistance = std::numeric_limits<long long>::max();
        bool found = false;
        for (int row = 0; row + span.rows <= pageRows; ++row)
            for (int column = 0; column + span.columns <= pageCols; ++column)
            {
                const long long dx = (static_cast<long long>(column) - fromCell.column) * pitchX;
                const long long dy = (static_cast<long long>(row) - fromCell.row) * pitchY;
                const long long distance = dx * dx + dy * dy;
                if (distance >= bestDistance) continue;
                GridCell candidate{fromCell.pageId, column, row};
                if (grid_occupancy::Occupied(usedSlots, candidate, span)) continue;
                bestDistance = distance;
                outCell = candidate;
                found = true;
            }
        return found;
    };

    for (const auto& entry : sourceList.entries)
    {
        GridSpan span = entry.originalSpan;
        if (internalMove && entry.kind == DropSourceKind::DesktopIcon && !entry.desktopKey.empty())
            for (const auto& item : items_)
                if (services.normalize(item.layoutKey) == services.normalize(entry.desktopKey))
                {
                    if (item.largeIcon) span = {item.largeIcon->columns, item.largeIcon->rows};
                    break;
                }
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;

        // 获取 cursor 所在页的维度（支持跨页后 cursor 切到新页）
        auto getPageSize = [&](const std::wstring& pid) -> std::pair<int,int> {
            if (pid == page->id) return { page->columns, page->rows };
            auto colIt = pageColumnsById.find(pid);
            auto rowIt = pageRowsById.find(pid);
            int c = (colIt != pageColumnsById.end()) ? colIt->second : page->columns;
            int r = (rowIt != pageRowsById.end()) ? rowIt->second : page->rows;
            return { c, r };
        };

        // First keep the requested/cursor cell, then choose its nearest vacancy.
        auto [curCols, curRows] = getPageSize(cursor.pageId);
        if (grid_occupancy::Valid(cursor, span) && cursor.column + span.columns <= curCols &&
            cursor.row + span.rows <= curRows &&
            !grid_occupancy::Occupied(usedSlots, cursor, span))
        {
            cell = cursor;
            found = true;
        }
        else
        {
            found = tryPlaceNearest(span, cursor, cell);
        }

        // 本页无法容纳时跨页搜索。
        if (!found)
        {
            found = services.findFree(span, usedSlots, cell, cursor.pageId, 0);
        }

        // 所有现有页都满了时预分配新溢出页。
        if (!found)
        {
            const auto newPageId = services.generateId(reservedIds);
            if (!pageColumnsById.contains(newPageId) && !pageRowsById.contains(newPageId) && !newPageId.empty() &&
                span.columns <= page->columns && span.rows <= page->rows)
            {
                cell = { newPageId, 0, 0 };
                found = true;
                reservedIds.push_back(newPageId);
                pageColumnsById[newPageId] = page->columns; pageRowsById[newPageId] = page->rows;
                plannedPages[newPageId] = {page->columns, page->rows};
            }
        }

        if (!found) continue;

        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = entry.sourceIndex;
        landing.cell = cell;
        landing.span = span;
        if (const auto planned = plannedPages.find(cell.pageId); planned != plannedPages.end())
            landing.plannedPageSize = planned->second;
        landings.push_back(landing);

        grid_occupancy::Mark(usedSlots, cell, span);
        cursor = advanceCell(cell, span);
    }
    return landings;
}
}
