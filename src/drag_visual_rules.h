#pragma once

#include <windows.h>

#include <cstddef>

namespace snowdesktop::drag_visual_rules
{

inline constexpr std::size_t kMaximumStackedPreviewItems = 4;
inline constexpr LONG kStackedPreviewOffset = 6;

inline constexpr DWORD kPreviewWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
    WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT;

constexpr bool ShouldShowPreview(
    bool active, bool visualVisible,
    bool hasItems) noexcept
{
    return active && visualVisible && hasItems;
}

constexpr bool ShouldCompactPreview(
    std::size_t itemCount) noexcept
{
    return itemCount > 1;
}

constexpr bool EqualRect(
    const RECT& left, const RECT& right) noexcept
{
    return left.left == right.left &&
        left.top == right.top &&
        left.right == right.right &&
        left.bottom == right.bottom;
}

constexpr bool ShouldApplyPreviewWindowPlacement(
    bool visible, bool cacheValid,
    const RECT& applied, const RECT& requested) noexcept
{
    return !visible || !cacheValid ||
        !EqualRect(applied, requested);
}

struct PreviewWindowZOrderPolicy
{
    HWND insertAfter = nullptr;
    UINT flags = SWP_NOACTIVATE;
};

constexpr PreviewWindowZOrderPolicy
ResolvePreviewWindowZOrderPolicy(bool visible) noexcept
{
    return visible
        ? PreviewWindowZOrderPolicy{
            nullptr, SWP_NOACTIVATE | SWP_NOZORDER }
        : PreviewWindowZOrderPolicy{
            HWND_TOPMOST, SWP_NOACTIVATE };
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
