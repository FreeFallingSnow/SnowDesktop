#pragma once

#include "widget/runtime/widget_item_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <windows.h>

namespace snowdesktop::collection_titleless_rules
{

inline constexpr float kMinimumIconScale = 0.8f;
// Preserve the preferred density while allowing smaller, legible icons to
// fit a grid whose pitches follow the frame aspect ratio.
inline constexpr float kMinimumBalancedIconScale = 2.0f / 3.0f;

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

inline widget_item_layout::Axis ResolveHalfGapIconAxis(
    int start, int extent, int count, int iconSize, int cellPadding)
{
    widget_item_layout::Axis result;
    result.start = start;
    result.extent = std::max(1, extent);
    result.count = std::max(1, count);
    const int pitch = std::max(1, result.extent / result.count);
    iconSize = std::clamp(iconSize, 1, pitch);
    const int cellInset = std::min(std::max(0, cellPadding),
        std::max(0, (pitch - iconSize) / 2));
    result.cell = iconSize + cellInset * 2;
    // Track-center rounding followed by icon-center rounding otherwise shifts
    // odd-sized hit cells twice at fractional DPI. An even hit cell lets the
    // existing centered-icon path round only once, without changing icon size.
    if (result.cell % 2 != 0)
    {
        if (result.cell < pitch) ++result.cell;
        else if (result.cell > iconSize) --result.cell;
    }
    result.gap = std::max(0, pitch - result.cell);
    result.edge = result.gap / 2;
    // Bounded track centers divide the complete extent into equal pitches.
    // Centering each icon in its pitch leaves half a visual gap at each edge.
    // Keep baseMargin zero; a common four-sided padding would break this ratio.
    return result;
}

inline widget_item_layout::Layout ResolveHalfGapIconGrid(
    RECT viewport, int columns, int rows, int iconSize,
    int horizontalInset, int verticalInset)
{
    widget_item_layout::Layout result;
    result.viewport = viewport;
    result.horizontal = ResolveHalfGapIconAxis(
        viewport.left, viewport.right - viewport.left,
        columns, iconSize, horizontalInset);
    result.vertical = ResolveHalfGapIconAxis(
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

inline bool CollectionOwnsHandoffDwell(
    std::wstring_view dwellWidgetId,
    std::wstring_view collectionWidgetId)
{
    return !dwellWidgetId.empty() &&
        dwellWidgetId == collectionWidgetId;
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
    const int width = std::max<LONG>(1, viewport.right - viewport.left);
    const int height = std::max<LONG>(1, viewport.bottom - viewport.top);
    const float spacing = layout_spacing_rules::ClampScale(spacingScale);
    const int minimumGapX = std::max(1,
        static_cast<int>(std::round(horizontalInset * 2 * spacing)));
    const int minimumGapY = std::max(1,
        static_cast<int>(std::round(verticalInset * 2 * spacing)));

    DenseLayout result;
    result.columns = baseColumns;
    result.rows = baseRows;
    result.minimumIconSize = std::max(1,
        static_cast<int>(std::ceil(baseIconSize * kMinimumBalancedIconScale)));
    result.iconSize = std::max(1, std::min({ baseIconSize,
        width / baseColumns - minimumGapX,
        height / baseRows - minimumGapY }));

    // Keep at least the capacity available at the preferred 80% icon floor.
    // Among legible denser grids, prefer square pitches so horizontal and
    // vertical visual gaps agree as closely as the frame's aspect ratio allows.
    const int preferredIconSize = std::max(1,
        static_cast<int>(std::ceil(baseIconSize * kMinimumIconScale)));
    const int minimumCapacity =
        std::max(baseColumns, width / (preferredIconSize + minimumGapX)) *
        std::max(baseRows, height / (preferredIconSize + minimumGapY));
    const int maximumColumns = std::max(baseColumns,
        width / (result.minimumIconSize + minimumGapX));
    const int maximumRows = std::max(baseRows,
        height / (result.minimumIconSize + minimumGapY));
    int bestPitchMismatch = std::numeric_limits<int>::max();
    for (int columns = baseColumns; columns <= maximumColumns; ++columns)
    {
        for (int rows = baseRows; rows <= maximumRows; ++rows)
        {
            const int capacity = columns * rows;
            if (capacity < minimumCapacity) continue;
            const int iconSize = std::min({ baseIconSize,
                width / columns - minimumGapX,
                height / rows - minimumGapY });
            if (iconSize < result.minimumIconSize) continue;
            const long long difference = std::abs(
                static_cast<long long>(width) * rows -
                static_cast<long long>(height) * columns);
            const int pitchMismatch = static_cast<int>(
                (difference + capacity - 1) / capacity);
            // Differences below one physical pixel are only rounding noise.
            if (pitchMismatch > bestPitchMismatch ||
                (pitchMismatch == bestPitchMismatch &&
                 (iconSize < result.iconSize ||
                  (iconSize == result.iconSize &&
                   capacity <= result.columns * result.rows))))
                continue;
            bestPitchMismatch = pitchMismatch;
            result.columns = columns;
            result.rows = rows;
            result.iconSize = iconSize;
        }
    }
    result.geometry = ResolveHalfGapIconGrid(viewport,
        result.columns, result.rows, result.iconSize,
        horizontalInset, verticalInset);
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
