#pragma once

#include "constants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace snowdesktop::widget_item_layout
{

struct Axis
{
    int start = 0;
    int extent = 1;
    int count = 1;
    int cell = 1;
    int gap = 0;
    int edge = 0;
};

struct Layout
{
    RECT viewport{};
    Axis horizontal{};
    Axis vertical{};
    bool scrolling = false;
};

inline bool IsCompactCollectionSpan(int columns, int rows)
{
    return columns <= 1 && rows <= 1;
}

inline bool CollectionUsesFullFrame(
    bool scrolling, int columns, int rows)
{
    return !scrolling && !IsCompactCollectionSpan(columns, rows);
}

inline int DesiredGap(int pitch, float percent, float spacingScale)
{
    return std::max(0, static_cast<int>(std::round(
        std::max(1, pitch) * percent *
        std::clamp(spacingScale, 0.5f, 2.0f))));
}

inline Axis ResolveBoundedAxis(int start, int extent, int count,
    int minimumCell, float gapPercent, float spacingScale)
{
    Axis result;
    result.start = start;
    result.extent = std::max(1, extent);
    result.count = std::max(1, count);
    const int pitch = std::max(1, result.extent / result.count);
    const int maximumGap = std::max(0,
        (result.extent - result.count * std::max(1, minimumCell)) /
            result.count);
    result.gap = std::min(
        DesiredGap(pitch, gapPercent, spacingScale), maximumGap);
    result.edge = result.gap / 2;
    const int usable = std::max(result.count,
        result.extent - result.edge * 2 -
            result.gap * (result.count - 1));
    result.cell = std::max(1, usable / result.count);
    return result;
}

inline Axis ResolveScrollingAxis(int start, int minimumCell,
    float gapPercent, float spacingScale)
{
    Axis result;
    result.start = start;
    result.extent = std::max(1, minimumCell);
    result.cell = std::max(1, minimumCell);
    result.gap = DesiredGap(
        result.cell, gapPercent, spacingScale);
    result.edge = result.gap / 2;
    return result;
}

inline Layout ResolveGrid(RECT viewport, int columns,
    int fixedRows, int minimumCellWidth, int minimumCellHeight,
    float spacingScale)
{
    Layout result;
    result.viewport = viewport;
    result.scrolling = fixedRows <= 0;
    result.horizontal = ResolveBoundedAxis(
        viewport.left, viewport.right - viewport.left,
        columns, minimumCellWidth, kGapPercentX, spacingScale);
    result.vertical = result.scrolling
        ? ResolveScrollingAxis(
            viewport.top, minimumCellHeight,
            kGapPercentY, spacingScale)
        : ResolveBoundedAxis(
            viewport.top, viewport.bottom - viewport.top,
            fixedRows, minimumCellHeight,
            kGapPercentY, spacingScale);
    return result;
}

inline Layout ResolveList(RECT viewport, int minimumRowHeight,
    float spacingScale)
{
    Layout result;
    result.viewport = viewport;
    result.scrolling = true;
    result.horizontal.start = viewport.left;
    result.horizontal.extent = std::max<int>(
        1, viewport.right - viewport.left);
    result.horizontal.count = 1;
    result.horizontal.edge = std::min(
        std::max(0, (result.horizontal.extent - 1) / 2),
        DesiredGap(minimumRowHeight, kGapPercentX, spacingScale) / 2);
    result.horizontal.cell = std::max(
        1, result.horizontal.extent - result.horizontal.edge * 2);
    result.horizontal.gap = 0;
    result.vertical = ResolveScrollingAxis(
        viewport.top, minimumRowHeight,
        kGapPercentY, spacingScale);
    return result;
}

inline int AxisCellStart(const Axis& axis, int index)
{
    const int clamped = std::max(0, index);
    return axis.start + axis.edge +
        clamped * (axis.cell + axis.gap);
}

inline RECT ItemRect(const Layout& layout, std::size_t index,
    int scrollOffset = 0)
{
    const int columns = std::max(1, layout.horizontal.count);
    const int column = static_cast<int>(index %
        static_cast<std::size_t>(columns));
    const int row = static_cast<int>(index /
        static_cast<std::size_t>(columns));
    const int left = AxisCellStart(layout.horizontal, column);
    const int top = AxisCellStart(layout.vertical, row) - scrollOffset;
    return RECT{ left, top,
        left + layout.horizontal.cell,
        top + layout.vertical.cell };
}

inline int ContentHeight(const Layout& layout, std::size_t itemCount)
{
    if (itemCount == 0) return 0;
    const std::size_t columns = static_cast<std::size_t>(
        std::max(1, layout.horizontal.count));
    const int rows = static_cast<int>(
        (itemCount + columns - 1) / columns);
    return layout.vertical.edge * 2 +
        rows * layout.vertical.cell +
        std::max(0, rows - 1) * layout.vertical.gap;
}

inline std::pair<std::size_t, std::size_t> VisibleRange(
    const Layout& layout, std::size_t itemCount,
    int scrollOffset, int visibleHeight)
{
    if (itemCount == 0) return { 0, 0 };
    const int stride = std::max(
        1, layout.vertical.cell + layout.vertical.gap);
    const int firstRow = std::max(
        0, (scrollOffset - layout.vertical.edge) / stride - 1);
    const int lastRow =
        (scrollOffset + std::max(1, visibleHeight) + stride - 1) /
            stride + 1;
    const std::size_t columns = static_cast<std::size_t>(
        std::max(1, layout.horizontal.count));
    return {
        std::min(itemCount,
            static_cast<std::size_t>(firstRow) * columns),
        std::min(itemCount,
            static_cast<std::size_t>(std::max(firstRow, lastRow)) * columns)
    };
}

inline int ScrollOffsetToReveal(
    RECT viewport, RECT target, int currentOffset, int maximumOffset)
{
    int result = std::clamp(
        currentOffset, 0, std::max(0, maximumOffset));
    if (target.top < viewport.top)
        result -= viewport.top - target.top;
    else if (target.bottom > viewport.bottom)
        result += target.bottom - viewport.bottom;
    return std::clamp(result, 0, std::max(0, maximumOffset));
}

inline bool SharesInsertionBoundary(
    RECT before, RECT after, bool verticalBar)
{
    if (verticalBar)
    {
        return before.right <= after.left &&
            std::max(before.top, after.top) <
                std::min(before.bottom, after.bottom);
    }
    return before.bottom <= after.top &&
        std::max(before.left, after.left) <
            std::min(before.right, after.right);
}

inline bool PointInInsertionGap(
    RECT before, RECT after, POINT point, bool verticalBar)
{
    if (!SharesInsertionBoundary(before, after, verticalBar))
        return false;
    if (verticalBar)
    {
        return point.x >= before.right &&
            point.x < after.left &&
            point.y >= std::max(before.top, after.top) &&
            point.y < std::min(before.bottom, after.bottom);
    }
    return point.y >= before.bottom &&
        point.y < after.top &&
        point.x >= std::max(before.left, after.left) &&
        point.x < std::min(before.right, after.right);
}

inline float InsertionBoundaryPad(
    RECT before, RECT after, bool verticalBar)
{
    if (!SharesInsertionBoundary(before, after, verticalBar))
        return 0.0f;
    const int gap = verticalBar
        ? after.left - before.right
        : after.top - before.bottom;
    return static_cast<float>(std::max(0, gap)) * 0.5f;
}

} // namespace snowdesktop::widget_item_layout
