#pragma once

#include "widget_item_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>
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
    int outerPadding = 0;
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

inline bool SupportsCommonOuterPadding(
    int extent, int count, int iconSize, int outerPadding)
{
    extent = std::max(1, extent);
    count = std::max(1, count);
    iconSize = std::max(1, iconSize);
    outerPadding = std::max(0, outerPadding);
    if (count == 1)
        return std::abs(extent - iconSize - outerPadding * 2) <= 1;
    return extent - outerPadding * 2 >= count * iconSize;
}

inline int DistributedVisualStart(
    int extent, int count, int iconSize,
    int outerPadding, int index)
{
    extent = std::max(1, extent);
    count = std::max(1, count);
    iconSize = std::clamp(iconSize, 1, extent);
    outerPadding = std::max(0, outerPadding);
    index = std::clamp(index, 0, count - 1);
    if (count == 1)
        return outerPadding;

    const int intervals = count - 1;
    const int span = std::max(
        0, extent - outerPadding * 2 - iconSize);
    const long long numerator =
        static_cast<long long>(index) * span;
    return outerPadding + static_cast<int>(
        (numerator + intervals / 2) / intervals);
}

inline std::vector<int> ResolveVisualGaps(
    int extent, int count, int iconSize, int outerPadding)
{
    count = std::max(1, count);
    std::vector<int> gaps;
    gaps.reserve(static_cast<std::size_t>(count + 1));
    int previousEnd = 0;
    for (int index = 0; index < count; ++index)
    {
        const int start = DistributedVisualStart(
            extent, count, iconSize, outerPadding, index);
        gaps.push_back(start - previousEnd);
        previousEnd = start + iconSize;
    }
    gaps.push_back(std::max(1, extent) - previousEnd);
    return gaps;
}

inline std::optional<int> ResolveCommonOuterPadding(
    int width, int height, int columns, int rows, int iconSize)
{
    width = std::max(1, width);
    height = std::max(1, height);
    columns = std::max(1, columns);
    rows = std::max(1, rows);
    iconSize = std::max(1, iconSize);

    const int maximumPadding = std::max(
        0, std::min(width, height) - iconSize);
    std::optional<int> bestPadding;
    int bestSpread = std::numeric_limits<int>::max();
    long long bestDeviation = std::numeric_limits<long long>::max();
    for (int padding = 0; padding <= maximumPadding; ++padding)
    {
        if (!SupportsCommonOuterPadding(
                width, columns, iconSize, padding) ||
            !SupportsCommonOuterPadding(
                height, rows, iconSize, padding))
            continue;

        auto gaps = ResolveVisualGaps(
            width, columns, iconSize, padding);
        auto verticalGaps = ResolveVisualGaps(
            height, rows, iconSize, padding);
        gaps.insert(gaps.end(),
            verticalGaps.begin(), verticalGaps.end());
        const auto [minimum, maximum] =
            std::minmax_element(gaps.begin(), gaps.end());
        const int spread = *maximum - *minimum;
        long long deviation = 0;
        for (const int gap : gaps)
            deviation += std::abs(gap - padding);
        if (spread < bestSpread ||
            (spread == bestSpread && deviation < bestDeviation))
        {
            bestPadding = padding;
            bestSpread = spread;
            bestDeviation = deviation;
        }
    }
    return bestPadding;
}

inline widget_item_layout::Axis ResolveBalancedIconAxis(
    int start, int extent, int count, int iconSize,
    int cellPadding, int outerPadding)
{
    widget_item_layout::Axis result;
    result.start = start;
    result.extent = std::max(1, extent);
    result.count = std::max(1, count);
    iconSize = std::clamp(
        iconSize, 1,
        std::max(1, result.extent / result.count));
    cellPadding = std::max(0, cellPadding);

    outerPadding = std::max(0, outerPadding);
    result.visualItemSize = iconSize;
    result.visualOuterPadding = outerPadding;
    result.gap = 0;

    int maximumCellInset = outerPadding;
    if (result.count > 1)
    {
        const auto gaps = ResolveVisualGaps(
            result.extent, result.count,
            iconSize, outerPadding);
        const auto firstInternal = gaps.begin() + 1;
        const auto lastInternal = gaps.end() - 1;
        const int minimumInternalGap = firstInternal == lastInternal
            ? 0 : *std::min_element(firstInternal, lastInternal);
        maximumCellInset = std::min(
            maximumCellInset, minimumInternalGap / 2);
    }
    const int cellInset = std::min(
        cellPadding, std::max(0, maximumCellInset));
    result.cell = iconSize + cellInset * 2;
    result.edge = std::max(0, outerPadding - cellInset);
    return result;
}

inline widget_item_layout::Layout ResolveBalancedIconGrid(
    RECT viewport, int columns, int rows, int iconSize,
    int horizontalInset, int verticalInset, int outerPadding)
{
    widget_item_layout::Layout result;
    result.viewport = viewport;
    result.horizontal = ResolveBalancedIconAxis(
        viewport.left, viewport.right - viewport.left,
        columns, iconSize, horizontalInset, outerPadding);
    result.vertical = ResolveBalancedIconAxis(
        viewport.top, viewport.bottom - viewport.top,
        rows, iconSize, verticalInset, outerPadding);
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
    auto resolvePaddingForIcon = [&](
        int columns, int rows, int& iconSize) {
        for (; iconSize >= 1; --iconSize)
        {
            const auto padding = ResolveCommonOuterPadding(
                width, height, columns, rows, iconSize);
            if (padding.has_value())
                return padding;
        }
        return std::optional<int>{};
    };
    if (const auto padding = resolvePaddingForIcon(
            result.columns, result.rows, result.iconSize))
        result.outerPadding = *padding;

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
            int iconSize = ResolveFittedIconSize(
                geometry, baseIconSize,
                horizontalInset, verticalInset);
            const auto padding = resolvePaddingForIcon(
                columns, rows, iconSize);
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
            result.outerPadding = padding.value_or(0);
        }
    }

    result.geometry = ResolveBalancedIconGrid(
        viewport, result.columns, result.rows,
        result.iconSize, horizontalInset, verticalInset,
        result.outerPadding);

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
