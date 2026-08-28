#pragma once

#include "widget_item_layout.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <windows.h>

namespace snowdesktop::collection_titleless_rules
{

inline constexpr float kMinimumIconScale = 0.8f;

struct DenseLayout
{
    widget_item_layout::Layout geometry{};
    int columns = 1;
    int rows = 1;
    int iconSize = 1;
    int minimumIconSize = 1;
};

inline bool IsLargeFolderMode(
    bool scrollContainerMode, int columns, int rows)
{
    return !scrollContainerMode &&
        !(columns <= 1 && rows <= 1);
}

inline bool IsActive(bool enabled,
    bool scrollContainerMode, int columns, int rows)
{
    return enabled && IsLargeFolderMode(
        scrollContainerMode, columns, rows);
}

inline bool ResolveStoredMode(
    std::optional<bool> globalMode, bool widgetMode)
{
    return globalMode.value_or(widgetMode);
}

inline int ResolveFittedIconSize(
    const widget_item_layout::Layout& layout,
    int baseIconSize, int horizontalInset, int verticalInset)
{
    return std::clamp(std::min({
        std::max(1, baseIconSize),
        std::max(1, layout.horizontal.cell -
            std::max(0, horizontalInset) * 2),
        std::max(1, layout.vertical.cell -
            std::max(0, verticalInset) * 2)
    }), 1, std::max(1, baseIconSize));
}

inline widget_item_layout::Axis ResolveBalancedIconAxis(
    int start, int extent, int count, int iconSize, int cellPadding)
{
    widget_item_layout::Axis result;
    result.start = start;
    result.extent = std::max(1, extent);
    result.count = std::max(1, count);
    iconSize = std::clamp(
        iconSize, 1,
        std::max(1, result.extent / result.count));
    cellPadding = std::max(0, cellPadding);

    // The visual gap is shared by both frame edges and every adjacent icon
    // pair. BoundedAxisCellStart distributes track centers through the inner
    // extent, so half of the ideal visual gap is the matching base margin.
    const int remaining = std::max(
        0, result.extent - result.count * iconSize);
    const double visualGap = static_cast<double>(remaining) /
        static_cast<double>(result.count + 1);
    result.edge = std::max(0,
        static_cast<int>(std::round(visualGap * 0.5)));
    result.gap = 0;

    const int pitch = std::max(1,
        (result.extent - result.edge * 2) / result.count);
    result.cell = std::clamp(
        iconSize + cellPadding * 2,
        iconSize, std::max(iconSize, pitch));
    return result;
}

inline widget_item_layout::Layout ResolveBalancedIconGrid(
    RECT viewport, int columns, int rows, int iconSize,
    int horizontalInset, int verticalInset)
{
    widget_item_layout::Layout result;
    result.viewport = viewport;
    result.horizontal = ResolveBalancedIconAxis(
        viewport.left, viewport.right - viewport.left,
        columns, iconSize, horizontalInset);
    result.vertical = ResolveBalancedIconAxis(
        viewport.top, viewport.bottom - viewport.top,
        rows, iconSize, verticalInset);
    return result;
}

inline bool IsHandoffDwellReady(
    bool targetMatches, bool alreadyReady,
    DWORD elapsed, DWORD delay)
{
    return targetMatches &&
        (alreadyReady || elapsed >= delay);
}

inline DenseLayout ResolveDenseLayout(
    RECT viewport, int baseColumns, int baseRows,
    int baseIconSize, int horizontalInset, int verticalInset,
    float spacingScale)
{
    baseColumns = std::max(1, baseColumns);
    baseRows = std::max(1, baseRows);
    baseIconSize = std::max(1, baseIconSize);
    horizontalInset = std::max(0, horizontalInset);
    verticalInset = std::max(0, verticalInset);

    DenseLayout result;
    result.columns = baseColumns;
    result.rows = baseRows;
    result.minimumIconSize = std::max(1,
        static_cast<int>(std::ceil(
            baseIconSize * kMinimumIconScale)));

    const int minimumCellWidth =
        result.minimumIconSize + horizontalInset * 2;
    const int minimumCellHeight =
        result.minimumIconSize + verticalInset * 2;
    result.geometry = widget_item_layout::ResolveGrid(
        viewport, baseColumns, baseRows,
        minimumCellWidth, minimumCellHeight, spacingScale);
    result.iconSize = ResolveFittedIconSize(
        result.geometry, baseIconSize,
        horizontalInset, verticalInset);

    const int width = std::max<LONG>(
        1, viewport.right - viewport.left);
    const int height = std::max<LONG>(
        1, viewport.bottom - viewport.top);
    const int maximumColumns = std::max(
        baseColumns, width / std::max(1, minimumCellWidth));
    const int maximumRows = std::max(
        baseRows, height / std::max(1, minimumCellHeight));

    bool foundQualified = false;
    int bestCapacity = 0;
    for (int columns = baseColumns;
         columns <= maximumColumns; ++columns)
    {
        for (int rows = baseRows; rows <= maximumRows; ++rows)
        {
            const auto geometry = widget_item_layout::ResolveGrid(
                viewport, columns, rows,
                minimumCellWidth, minimumCellHeight, spacingScale);
            const int iconSize = ResolveFittedIconSize(
                geometry, baseIconSize,
                horizontalInset, verticalInset);
            if (iconSize < result.minimumIconSize)
                continue;

            const int capacity = columns * rows;
            if (foundQualified &&
                (capacity < bestCapacity ||
                 (capacity == bestCapacity &&
                  iconSize <= result.iconSize)))
                continue;

            foundQualified = true;
            bestCapacity = capacity;
            result.geometry = geometry;
            result.columns = columns;
            result.rows = rows;
            result.iconSize = iconSize;
        }
    }

    result.geometry = ResolveBalancedIconGrid(
        viewport, result.columns, result.rows,
        result.iconSize, horizontalInset, verticalInset);

    return result;
}

inline RECT ResolveTooltipBounds(
    RECT anchor, RECT frame, int width, int height,
    int gap, int frameInset)
{
    const int availableWidth = std::max(1,
        static_cast<int>(frame.right - frame.left) -
            frameInset * 2);
    const int availableHeight = std::max(1,
        static_cast<int>(frame.bottom - frame.top) -
            frameInset * 2);
    width = std::clamp(width, 1, availableWidth);
    height = std::clamp(height, 1, availableHeight);

    const int minimumLeft = frame.left + frameInset;
    const int maximumLeft = std::max(
        minimumLeft,
        static_cast<int>(frame.right) - frameInset - width);
    int left = (anchor.left + anchor.right - width) / 2;
    left = std::clamp(left, minimumLeft, maximumLeft);

    const int minimumTop = frame.top + frameInset;
    const int maximumTop = std::max(
        minimumTop,
        static_cast<int>(frame.bottom) - frameInset - height);
    int top = anchor.bottom + std::max(0, gap);
    if (top > maximumTop)
        top = anchor.top - std::max(0, gap) - height;
    top = std::clamp(top, minimumTop, maximumTop);

    return RECT{ left, top, left + width, top + height };
}

} // namespace snowdesktop::collection_titleless_rules
