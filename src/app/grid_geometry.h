#pragma once

#include "../types.h"
#include "../utils.h"
#include "../font_cu_rules.h"
#include "../grid_spacing_rules.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// Pure grid geometry shared by DesktopApp layout and Dock placement.

/**
 * @brief 判断指定的网格区域是否在页面范围内。
 * @param page  目标网格页面。
 * @param cell  起始单元格。
 * @param span  跨度（列数 x 行数）。
 * @return 如果区域完全在页面边界内返回 true，否则返回 false。
 */
inline bool GridAreaFitsPage(const GridPage& page, const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.pageId != page.id) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return cell.column + span.columns <= page.columns &&
        cell.row + span.rows <= page.rows;
}

/**
 * @brief 在不改变跨度的前提下，将网格起点收回到页面范围内。
 *
 * 用于组件拖拽：指针命中页面右侧或下侧、剩余行列不足时移动起点，避免
 * GetGridRect 为适配边界缩小预览区域。跨度大于整页时仍锚定到零点，由调用方
 * 按自身规则拒绝或限制该放置。
 */
inline GridCell ClampGridCellToFitPage(
    const GridPage& page, GridCell cell, GridSpan span)
{
    span.columns = std::max(1, span.columns);
    span.rows = std::max(1, span.rows);
    cell.column = std::clamp(
        cell.column, 0,
        std::max(0, page.columns - span.columns));
    cell.row = std::clamp(
        cell.row, 0,
        std::max(0, page.rows - span.rows));
    return cell;
}

/**
 * @brief 根据屏幕坐标查找所在的网格页面。
 * @param point 客户区坐标。
 * @return 指向对应 GridPage 的指针，未找到时返回第一个页面或 nullptr。
 */
inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId)
{
    for (auto& p : pages) if (p.id == pageId) return &p;
    return nullptr;
}

/**
 * @brief 获取页面不含网格间距的标准 cu 缩放比例。
 */
inline float GetGridPageCuScale(const GridPage& page)
{
    return snowdesktop::font_cu_rules::CellScale(
        page.itemPitchWidth, page.itemPitchHeight);
}

/**
 * @brief 根据项目边界获取所在页面的标准 cu 缩放比例。
 */
inline float GetGridCuScaleForBounds(
    const std::vector<GridPage>& pages, RECT bounds)
{
    const POINT center = {
        bounds.left + (bounds.right - bounds.left) / 2,
        bounds.top + (bounds.bottom - bounds.top) / 2
    };
    for (const auto& page : pages)
    {
        if (PtInRect(&page.bounds, center))
            return GetGridPageCuScale(page);
    }
    return 1.0f;
}

/**
 * @brief 获取网格轴上指定索引的偏移像素值。
 * @param page 网格页面。
 * @param index 索引。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 偏移像素值。
 */
inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    const int cellSize = horizontal ? page.cellWidth : page.cellHeight;
    const int extent = horizontal
        ? static_cast<int>(page.workArea.right - page.workArea.left)
        : static_cast<int>(page.workArea.bottom - page.workArea.top);
    const int margin = horizontal ? page.marginX : page.marginY;
    const int gap = horizontal ? page.gapX : page.gapY;
    snowdesktop::grid_spacing_rules::AxisGeometry axis;
    axis.extent = std::max(1, extent);
    axis.count = std::max(1, count);
    axis.baseMargin = std::max(0, margin - gap / 2);
    axis.margin = margin;
    axis.cell = std::max(1, cellSize);
    axis.gap = std::max(0, gap);

    // Track centers come only from the page pitch. Changing the requested gap
    // shrinks the cell around the same center instead of redistributing integer
    // remainders and making icons oscillate by one or two pixels.
    return snowdesktop::grid_spacing_rules::CellStart(axis, index) - margin;
}

/**
 * @brief 根据网格页面和单元格计算对应的矩形区域。
 * @param pages 页面列表。
 * @param cell 起始单元格。
 * @param span 跨度（默认 {1,1}）。
 * @return 计算出的 RECT。
 */
inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span = {})
{
    auto* page = FindGridPage(pages, cell.pageId);
    if (!page) return {};
    int col = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
    int row = std::clamp(cell.row,    0, std::max(0, page->rows    - 1));
    int sc  = std::clamp(span.columns, 1, std::max(1, page->columns - col));
    int sr  = std::clamp(span.rows,    1, std::max(1, page->rows    - row));
    int x = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col, true);
    int y = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row, false);
    int r = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col + sc - 1, true)  + page->cellWidth;
    int b = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row + sr - 1, false) + page->cellHeight;
    return {x, y, r, b};
}

inline const GridPage* FindDragGridPage(const std::vector<GridPage>& pages,
    POINT pointer, const GridPage* fallback = nullptr)
{
    for (const auto& page : pages)
        if (PtInRect(&page.bounds, pointer) || PtInRect(&page.workArea, pointer))
            return &page;
    return fallback;
}

// A drag origin can still be on the source display while the pointer has
// entered the destination. Choose the page with the pointer, then snap the
// translated origin to the nearest cell origin (not a cell's far edge).
inline GridCell ResolveGridDragCell(const std::vector<GridPage>& pages,
    POINT pointer, POINT origin, const GridPage* fallback = nullptr)
{
    const GridPage* target = FindDragGridPage(pages, pointer, fallback);
    if (!target) return {};
    const auto nearestOrigin = [&](bool horizontal) {
        const int count = horizontal ? target->columns : target->rows;
        const LONG coordinate = horizontal ? origin.x : origin.y;
        const LONG start = horizontal
            ? target->workArea.left + target->marginX
            : target->workArea.top + target->marginY;
        int best = 0;
        auto distance = [&](int index) {
            return std::abs(static_cast<long long>(coordinate) - start -
                GetGridAxisOffset(*target, index, horizontal));
        };
        auto bestDistance = distance(0);
        for (int index = 1; index < count; ++index)
        {
            const auto nextDistance = distance(index);
            if (nextDistance < bestDistance)
            {
                best = index;
                bestDistance = nextDistance;
            }
        }
        return best;
    };
    return {target->id, nearestOrigin(true), nearestOrigin(false)};
}

// Capture once from the original widget bounds, before paging can relayout or
// hide them. Independent fractions retain the grab point on non-square grids;
// a single CU/DPI scale cannot represent different horizontal/vertical ratios.
struct GridDragAnchor
{
    double x = 0.0;
    double y = 0.0;
};

inline GridDragAnchor CaptureGridDragAnchor(RECT bounds, POINT pointer)
{
    // Handles may extend outside the frame, so signed fractions are intentional.
    return {
        (static_cast<double>(pointer.x) - bounds.left) /
            std::max(1L, bounds.right - bounds.left),
        (static_cast<double>(pointer.y) - bounds.top) /
            std::max(1L, bounds.bottom - bounds.top)
    };
}

inline GridCell ResolveGridSpanDragCell(const std::vector<GridPage>& pages,
    POINT pointer, GridDragAnchor anchor, GridSpan span,
    const GridPage* fallback = nullptr)
{
    const GridPage* target = FindDragGridPage(pages, pointer, fallback);
    if (!target) return {};
    const auto nearestAnchor = [&](bool horizontal) {
        const int count = std::max(1, horizontal ? target->columns : target->rows);
        const int length = std::clamp(horizontal ? span.columns : span.rows, 1, count);
        const int cellSize = horizontal ? target->cellWidth : target->cellHeight;
        const LONG coordinate = horizontal ? pointer.x : pointer.y;
        const LONG start = horizontal
            ? target->workArea.left + target->marginX
            : target->workArea.top + target->marginY;
        const double fraction = horizontal ? anchor.x : anchor.y;
        const auto distance = [&](int index) {
            const int offset = GetGridAxisOffset(*target, index, horizontal);
            // Match GetGridRect, including gaps and per-track integer rounding.
            const int size = GetGridAxisOffset(*target, index + length - 1, horizontal)
                + cellSize - offset;
            return std::abs(static_cast<double>(coordinate) - start - offset - fraction * size);
        };
        int best = 0;
        auto bestDistance = distance(0);
        for (int index = 1; index <= count - length; ++index)
        {
            const auto nextDistance = distance(index);
            if (nextDistance < bestDistance)
            {
                best = index;
                bestDistance = nextDistance;
            }
        }
        return best;
    };
    return {target->id, nearestAnchor(true), nearestAnchor(false)};
}

/**
 * @brief 根据网格页面和单元格计算槽位索引。
 * @param pages 页面列表。
 * @param cell 单元格。
 * @return 槽位索引。
 */
inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell)
{
    auto* page = FindGridPage(pages, cell.pageId);
    int rows = page ? page->rows : 1;
    return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
}
