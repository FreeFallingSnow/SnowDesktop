#pragma once

#include "common/types.h"
#include <algorithm>
#include <optional>
#include <unordered_set>

namespace snowdesktop::grid_occupancy
{
using Slots = std::unordered_set<std::wstring>;
inline bool Valid(const GridCell& cell, GridSpan span)
{
    return span.columns >= 1 && span.rows >= 1 && cell.column >= 0 && cell.row >= 0;
}
inline bool Fits(const GridPage& page, const GridCell& cell, GridSpan span)
{
    return Valid(cell, span) && cell.pageId == page.id &&
        cell.column + span.columns <= page.columns && cell.row + span.rows <= page.rows;
}
inline void Mark(Slots& slots, const GridCell& cell, GridSpan span)
{
    for (int column = cell.column; column < cell.column + span.columns; ++column)
        for (int row = cell.row; row < cell.row + span.rows; ++row)
            slots.insert(cell.pageId + L":" + std::to_wstring(column) + L"," + std::to_wstring(row));
}
inline bool Occupied(const Slots& slots, const GridCell& cell, GridSpan span)
{
    for (int column = cell.column; column < cell.column + span.columns; ++column)
        for (int row = cell.row; row < cell.row + span.rows; ++row)
            if (slots.contains(cell.pageId + L":" + std::to_wstring(column) + L"," + std::to_wstring(row))) return true;
    return false;
}
inline bool FindFree(const std::vector<GridPage>& pages, std::vector<size_t> pageOrder,
    GridSpan span, const Slots& usedSlots, GridCell& result,
    const std::wstring& preferredPageId = {}, int preferredStartSlot = 0)
{
    // Automatic placement follows the configured first-page monitor order,
    // not the physical left-to-right monitor order.

    if (pageOrder.empty())
    {
        pageOrder.reserve(pages.size());
        for (size_t i = 0; i < pages.size(); ++i)
            pageOrder.push_back(i);
    }

    auto tryPage = [&](const GridPage& page, int startSlot, GridCell& found) -> bool {
        const int capacity = std::max(1, page.columns * page.rows);
        for (int slot = std::clamp(startSlot, 0, capacity - 1); slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = page.id;
            candidate.column = slot / std::max(1, page.rows);
            candidate.row = slot % std::max(1, page.rows);
            if (Fits(page, candidate, span) && !Occupied(usedSlots, candidate, span))
            {
                found = candidate;
                return true;
            }
        }
        return false;
    };

    if (!preferredPageId.empty())
    {
        for (const auto& page : pages)
        {
            if (page.id == preferredPageId && tryPage(page, preferredStartSlot, result))
                return true;
        }
    }

    for (size_t pageIndex : pageOrder)
    {
        if (pageIndex >= pages.size()) continue;
        const auto& page = pages[pageIndex];
        if (!preferredPageId.empty() && page.id == preferredPageId) continue;
        if (tryPage(page, 0, result))
            return true;
    }

    if (!preferredPageId.empty())
    {
        for (const auto& page : pages)
        {
            if (page.id == preferredPageId && tryPage(page, 0, result))
                return true;
        }
    }
    return false;
}
}
