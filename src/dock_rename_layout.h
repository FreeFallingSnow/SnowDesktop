#pragma once

#include "dock_settings.h"

#include <algorithm>
#include <windows.h>

namespace snowdesktop::dock_rename_layout
{

inline RECT CalculateAdjacentEditRect(
    const RECT& anchorScreen, const RECT& workArea,
    DockPosition position, int desiredWidth,
    int desiredHeight, int gap, int margin)
{
    const int safeMargin = std::max(0, margin);
    const int availableWidth = std::max(
        1, static_cast<int>(
            workArea.right - workArea.left) - safeMargin * 2);
    const int availableHeight = std::max(
        1, static_cast<int>(
            workArea.bottom - workArea.top) - safeMargin * 2);
    const int width = std::clamp(
        desiredWidth, 1, availableWidth);
    const int height = std::clamp(
        desiredHeight, 1, availableHeight);
    const int spacing = std::max(0, gap);
    const int anchorCenterX =
        (anchorScreen.left + anchorScreen.right) / 2;
    const int anchorCenterY =
        (anchorScreen.top + anchorScreen.bottom) / 2;

    int left = anchorCenterX - width / 2;
    int top = anchorScreen.top - spacing - height;
    switch (position)
    {
    case DockPosition::Top:
        top = anchorScreen.bottom + spacing;
        break;
    case DockPosition::Left:
        left = anchorScreen.right + spacing;
        top = anchorCenterY - height / 2;
        break;
    case DockPosition::Right:
        left = anchorScreen.left - spacing - width;
        top = anchorCenterY - height / 2;
        break;
    case DockPosition::Bottom:
    default:
        break;
    }

    const int minimumLeft =
        static_cast<int>(workArea.left) + safeMargin;
    const int maximumLeft = std::max(
        minimumLeft,
        static_cast<int>(workArea.right) -
            safeMargin - width);
    const int minimumTop =
        static_cast<int>(workArea.top) + safeMargin;
    const int maximumTop = std::max(
        minimumTop,
        static_cast<int>(workArea.bottom) -
            safeMargin - height);
    left = std::clamp(left, minimumLeft, maximumLeft);
    top = std::clamp(top, minimumTop, maximumTop);
    return { left, top, left + width, top + height };
}

} // namespace snowdesktop::dock_rename_layout
