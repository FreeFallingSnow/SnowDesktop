#pragma once

#include <algorithm>
#include <cstddef>

namespace snowdesktop::page_navigation_rules
{

template <typename HasContentAtPageIndex>
int NextNonEmptyOffset(
    int fromOffset,
    int direction,
    std::size_t savedPageCount,
    std::size_t gridPageCount,
    HasContentAtPageIndex&& hasContentAtPageIndex)
{
    if (savedPageCount == 0 || gridPageCount == 0 ||
        (direction != -1 && direction != 1))
        return fromOffset;

    const int visiblePageCount = static_cast<int>(
        std::min(savedPageCount, gridPageCount));
    const int rawMaximum = std::max(
        0, static_cast<int>(savedPageCount) - visiblePageCount);
    int offset = fromOffset;
    while (true)
    {
        offset += direction;
        if (offset < 0 || offset > rawMaximum)
            return fromOffset;
        const std::size_t pageIndex = static_cast<std::size_t>(
            visiblePageCount - 1 + offset);
        if (pageIndex < savedPageCount &&
            hasContentAtPageIndex(pageIndex))
            return offset;
    }
}

template <typename HasContentAtPageIndex>
int MaximumOffset(
    std::size_t savedPageCount,
    std::size_t gridPageCount,
    HasContentAtPageIndex&& hasContentAtPageIndex)
{
    if (savedPageCount == 0 || gridPageCount == 0)
        return 0;

    const int visiblePageCount = static_cast<int>(
        std::min(savedPageCount, gridPageCount));
    const int rawMaximum = std::max(
        0, static_cast<int>(savedPageCount) - visiblePageCount);
    for (int offset = rawMaximum; offset > 0; --offset)
    {
        const std::size_t pageIndex = static_cast<std::size_t>(
            visiblePageCount - 1 + offset);
        if (pageIndex < savedPageCount &&
            hasContentAtPageIndex(pageIndex))
            return offset;
    }
    return 0;
}

} // namespace snowdesktop::page_navigation_rules
