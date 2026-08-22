#pragma once

#include <windows.h>

namespace snowdesktop::drag_visual_rules
{

inline constexpr DWORD kPreviewWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
    WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;

constexpr bool ShouldShowPreview(
    bool active, bool visualVisible,
    bool hasItems) noexcept
{
    return active && visualVisible && hasItems;
}

constexpr bool DropPreviewBelongsToRenderSurface(
    bool renderingFloatingDock,
    bool floatingDockOwnsDesktopCopy,
    bool targetIsFloatingDock) noexcept
{
    const bool belongsToFloatingDock =
        floatingDockOwnsDesktopCopy && targetIsFloatingDock;
    return renderingFloatingDock == belongsToFloatingDock;
}

} // namespace snowdesktop::drag_visual_rules
