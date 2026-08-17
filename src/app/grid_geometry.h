#pragma once

#include "../types.h"
#include "../utils.h"
#include "../font_cu_rules.h"
#include "../grid_spacing_rules.h"

#include <algorithm>
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
    if (!page) return MakeRect(0, 0, 0, 0);
    int col = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
    int row = std::clamp(cell.row,    0, std::max(0, page->rows    - 1));
    int sc  = std::clamp(span.columns, 1, std::max(1, page->columns - col));
    int sr  = std::clamp(span.rows,    1, std::max(1, page->rows    - row));
    int x = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col, true);
    int y = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row, false);
    int r = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col + sc - 1, true)  + page->cellWidth;
    int b = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row + sr - 1, false) + page->cellHeight;
    return MakeRect(x, y, r, b);
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
