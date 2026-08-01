#pragma once

#include <windows.h>

namespace snowdesktop::desktop_backdrop_update_rules
{

inline bool CoversClientArea(
    const RECT* updateRect,
    const RECT& clientRect) noexcept
{
    if (!updateRect)
        return true;

    return updateRect->left <= clientRect.left &&
        updateRect->top <= clientRect.top &&
        updateRect->right >= clientRect.right &&
        updateRect->bottom >= clientRect.bottom;
}

inline bool ShouldCollectAllPanels(
    bool dragSessionActive,
    bool widgetPreviewActive,
    bool desktopMarqueeActive,
    const RECT* updateRect,
    const RECT& clientRect) noexcept
{
    return !dragSessionActive &&
        !widgetPreviewActive &&
        !desktopMarqueeActive &&
        CoversClientArea(updateRect, clientRect);
}

} // namespace snowdesktop::desktop_backdrop_update_rules
