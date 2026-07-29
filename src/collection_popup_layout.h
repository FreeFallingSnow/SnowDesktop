#pragma once

#include <algorithm>
#include <cstddef>

namespace snowdesktop::collection_popup_layout
{

inline constexpr int kMaximumColumns = 5;
inline constexpr int kEmptyColumns = 3;
inline constexpr int kEmptyRows = 2;

inline int PreferredColumnCount(
    std::size_t itemCount, int maximumColumns)
{
    maximumColumns = std::max(1, maximumColumns);
    if (itemCount == 0)
        return std::min(
            kEmptyColumns, maximumColumns);
    return std::clamp(
        static_cast<int>(std::min<std::size_t>(
            itemCount, kMaximumColumns)),
        1, maximumColumns);
}

inline int RequiredRowCount(
    std::size_t itemCount, int columns)
{
    columns = std::max(1, columns);
    if (itemCount == 0)
        return kEmptyRows;
    return std::max(
        1, (static_cast<int>(itemCount) +
               columns - 1) /
            columns);
}

} // namespace snowdesktop::collection_popup_layout
