#pragma once

#include "app/drag_drop/desktop_request.h"

namespace pointer_geometry
{
// Exact, divisible tracks make coordinate expectations independent of the
// production spacing and nearest-cell algorithms, including negative screens.
inline void Configure(std::vector<GridPage>& pages, bool mixed = true)
{
    for (size_t index = 0; index < pages.size(); ++index)
    {
        auto& page = pages[index]; const int dpi = mixed && index == 1 ? 144 : 96;
        const int width = page.columns * 100 * dpi / 96, height = page.rows * 100 * dpi / 96;
        page.bounds = page.workArea = pages.size() > 1 && index == 0 ? RECT{-width, -height, 0, 0} : RECT{0, 0, width, height};
        page.visualWorkArea = page.workArea;
        page.dpiX = page.dpiY = static_cast<UINT>(dpi);
        page.cellWidth = page.cellHeight = 80 * dpi / 96;
        page.marginX = page.marginY = 10 * dpi / 96;
        page.gapX = page.gapY = 20 * dpi / 96;
    }
}
inline POINT Center(const GridPage& page, int column, int row)
{
    return {page.bounds.left + (column * 100 + 50) * static_cast<int>(page.dpiX) / 96,
        page.bounds.top + (row * 100 + 50) * static_cast<int>(page.dpiY) / 96};
}
inline RECT Frame(const GridPage& page, GridCell cell, GridSpan span)
{
    const int dpi = static_cast<int>(page.dpiX);
    return {page.bounds.left + (cell.column * 100 + 10) * dpi / 96,
        page.bounds.top + (cell.row * 100 + 10) * dpi / 96,
        page.bounds.left + ((cell.column + span.columns) * 100 - 10) * dpi / 96,
        page.bounds.top + ((cell.row + span.rows) * 100 - 10) * dpi / 96};
}
}
