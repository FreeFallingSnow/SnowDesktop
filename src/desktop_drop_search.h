#pragma once

#include "desktop_drop_cache.h"

#include <cstdlib>

namespace snowdesktop::desktop_drop_cache
{

// Exact placement leaves an invalid request unchanged so the landing planner
// can reject it. This must precede cache reuse and ordinary neighbour search.
template <typename CanPlace>
GridCell FindBestCell(
    BestCellEntry& cache, bool cacheActive, const BestCellKey& key,
    bool exactPlacement, int columns, int rows, CanPlace&& canPlace)
{
    if (exactPlacement) return key.requested;

    GridCell cached;
    if (cache.TryGet(cacheActive, key, cached)) return cached;
    const auto finish = [&](const GridCell& result) {
        cache.Store(cacheActive, key, result);
        return result;
    };

    if (canPlace(key.requested)) return finish(key.requested);
    if (columns <= 0 || rows <= 0) return finish(key.requested);
    const auto inside = [&](const GridCell& cell) {
        return cell.column >= 0 && cell.column < columns &&
            cell.row >= 0 && cell.row < rows;
    };

    for (int sign : { 1, -1 })
    {
        for (int distance = 1; distance <= 8; ++distance)
        {
            GridCell probe = key.requested;
            probe.column += key.direction.column * distance * sign;
            probe.row += key.direction.row * distance * sign;
            if (!inside(probe)) break;
            if (canPlace(probe)) return finish(probe);
        }
    }
    for (int distance = 1; distance <= 6; ++distance)
    {
        for (int dc = -distance; dc <= distance; ++dc)
        {
            for (int dr = -distance; dr <= distance; ++dr)
            {
                if (std::abs(dc) != distance && std::abs(dr) != distance) continue;
                GridCell probe = key.requested;
                probe.column += dc;
                probe.row += dr;
                if (inside(probe) && canPlace(probe)) return finish(probe);
            }
        }
    }
    return finish(key.requested);
}

} // namespace snowdesktop::desktop_drop_cache
