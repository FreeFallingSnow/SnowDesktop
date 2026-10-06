#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace snowdesktop::windows_desktop_layout
{
inline constexpr int kMaximumGridAxis = 50;

inline int MonitorSpacing(int spacing, unsigned sourceDpi, unsigned targetDpi) noexcept
{
    if (spacing <= 0) return 0;
    if (!sourceDpi || !targetDpi) return spacing;
    return static_cast<int>(std::clamp<std::int64_t>(
        (static_cast<std::int64_t>(spacing) * targetDpi + sourceDpi / 2) / sourceDpi,
        1, std::numeric_limits<int>::max()));
}

inline int AxisCount(int extent, int spacing) noexcept
{
    if (extent <= 0 || spacing <= 0) return 0;
    return std::clamp(extent / spacing, 1, kMaximumGridAxis);
}

// Explorer reports the upper-left position of an item, not its visual center.
// Round to the nearest native cell before mapping into SnowDesktop's grid.
inline int AxisIndex(int position, int origin, int spacing) noexcept
{
    if (spacing <= 0) return 0;
    const auto delta = static_cast<std::int64_t>(position) - origin;
    return static_cast<int>(std::clamp<std::int64_t>(
        std::llround(static_cast<double>(delta) / spacing),
        0, kMaximumGridAxis - 1));
}

struct Cell
{
    int column = 0;
    int row = 0;
    bool operator==(const Cell&) const = default;
};

// Free-positioned Explorer items may round into the same cell. Keep both
// items by choosing the closest vacant cell, with deterministic column-first
// ties matching Explorer's usual fill direction.
inline std::optional<Cell> ClaimNearestCell(
    Cell requested, int columns, int rows, std::vector<bool>& occupied)
{
    if (columns < 1 || rows < 1 || columns > kMaximumGridAxis ||
        rows > kMaximumGridAxis || occupied.size() != static_cast<size_t>(columns * rows))
        return std::nullopt;
    requested.column = std::clamp(requested.column, 0, columns - 1);
    requested.row = std::clamp(requested.row, 0, rows - 1);
    std::optional<Cell> nearest;
    int distance = std::numeric_limits<int>::max();
    for (int column = 0; column < columns; ++column)
    {
        for (int row = 0; row < rows; ++row)
        {
            if (occupied[column * rows + row]) continue;
            const int dx = column - requested.column;
            const int dy = row - requested.row;
            const int candidate = dx * dx + dy * dy;
            if (candidate < distance)
            {
                distance = candidate;
                nearest = Cell{column, row};
            }
        }
    }
    if (nearest) occupied[nearest->column * rows + nearest->row] = true;
    return nearest;
}
}
