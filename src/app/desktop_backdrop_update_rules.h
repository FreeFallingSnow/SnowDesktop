#pragma once

#include <windows.h>

#include <cstdint>

namespace snowdesktop::desktop_backdrop_update_rules
{

inline bool PanelIdentityMatches(
    std::uintptr_t existingOwnerKey,
    const RECT& existingFrame,
    std::uintptr_t requestedOwnerKey,
    const RECT& requestedFrame) noexcept
{
    if (existingOwnerKey != 0 || requestedOwnerKey != 0)
    {
        return existingOwnerKey != 0 &&
            existingOwnerKey == requestedOwnerKey;
    }
    return EqualRect(&existingFrame, &requestedFrame) != FALSE;
}

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
    bool forceCompleteCollection,
    bool dragSessionActive,
    bool widgetPreviewActive,
    bool desktopMarqueeActive,
    const RECT* updateRect,
    const RECT& clientRect) noexcept
{
    return forceCompleteCollection ||
        (!dragSessionActive &&
        !widgetPreviewActive &&
        !desktopMarqueeActive &&
        CoversClientArea(updateRect, clientRect));
}

} // namespace snowdesktop::desktop_backdrop_update_rules
