#pragma once

#include <algorithm>
#include <optional>
#include <windows.h>

namespace snowdesktop::collection_titleless_rules
{

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
    std::optional<bool> globalMode, bool anyLegacyWidgetEnabled)
{
    return globalMode.value_or(anyLegacyWidgetEnabled);
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
