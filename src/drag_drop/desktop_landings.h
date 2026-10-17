#pragma once

#include "core/drop_model.h"
#include "layout/grid_occupancy.h"
#include <functional>

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

    // 阶段式搜索：1) 从 cursor 向右 + 下方行从 startCol 开始
    //             1b) 上方行从 startCol 开始（绕回页面顶部填间隙）
    //             1c) 下方/上方行从列 0..startCol-1 补扫（覆盖左侧空位）
    //             2) 全页行优先搜索（兜底）
    //             3) TryFindFreeCell 跨页搜索
    auto tryPlaceRightward = [&](GridSpan span, GridCell fromCell, GridCell& outCell) -> bool {
        // 获取 fromCell 所在页的维度（支持跨页后 cursor 切到新页）
        int pageCols = page->columns, pageRows = page->rows;
        if (fromCell.pageId != page->id)
        {
            auto colIt = pageColumnsById.find(fromCell.pageId);
            auto rowIt = pageRowsById.find(fromCell.pageId);
            if (colIt != pageColumnsById.end()) pageCols = colIt->second;
            if (rowIt != pageRowsById.end()) pageRows = rowIt->second;
        }

        // 当前行从 fromCell.column 向右找
        for (int c = fromCell.column; c + span.columns <= pageCols; ++c)
        {
            GridCell candidate{ fromCell.pageId, c, fromCell.row };
            if (!grid_occupancy::Occupied(usedSlots, candidate, span))
            {
                outCell = candidate;
                return true;
            }
        }

        // 下方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = fromCell.row + 1; r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!grid_occupancy::Occupied(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!grid_occupancy::Occupied(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }

        // 上方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = 0; r < fromCell.row && r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!grid_occupancy::Occupied(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!grid_occupancy::Occupied(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }
        return false;
    };

    for (const auto& entry : sourceList.entries)
    {
        GridSpan span = entry.originalSpan;
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

        // 阶段 1：从 cursor 向右 + 下方行从 startCol
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
            found = tryPlaceRightward(span, cursor, cell);
        }

        // 阶段 2：全页行优先搜索（兜底，优先本页）
        if (!found)
        {
            for (int r = 0; r + span.rows <= curRows && !found; ++r)
            {
                for (int c = 0; c + span.columns <= curCols && !found; ++c)
                {
                    GridCell candidate{ cursor.pageId, c, r };
                    if (!grid_occupancy::Occupied(usedSlots, candidate, span))
                    {
                        cell = candidate;
                        found = true;
                    }
                }
            }
        }

        // 阶段 3：跨页搜索（TryFindFreeCell 内部遍历其他显示页）
        if (!found)
        {
            found = services.findFree(span, usedSlots, cell, cursor.pageId, 0);
        }

        // 阶段 4：所有现有页都满了 → 预分配新溢出页
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
