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

constexpr bool ShouldSyncPreviewBeforePresentation(
    bool alreadySyncedForInput) noexcept
{
    return !alreadySyncedForInput;
}

constexpr bool ShouldCompactPreview(
    std::size_t itemCount) noexcept
{
    return itemCount > 1;
}

constexpr bool ShouldSkipPreviewFallbackCandidate(
    bool visible,
    bool enabled,
    bool cloaked,
    bool presentationOnly) noexcept
{
    return !visible || !enabled || cloaked || presentationOnly;
}

constexpr bool IsLayeredTransparentPresentationWindow(
    DWORD extendedStyle) noexcept
{
    constexpr DWORD kPassthroughStyle =
        WS_EX_LAYERED | WS_EX_TRANSPARENT;
    return (extendedStyle & kPassthroughStyle) ==
        kPassthroughStyle;
}

constexpr bool PreviewFallbackRegionContainsPoint(
    int regionType,
    bool pointInRegion) noexcept
{
    // GetWindowRgn reports ERROR both for windows without an explicit region
    // and for an API failure. Keep the ordinary rectangular-window fallback,
    // but never treat a documented empty region as a hit.
    return regionType == ERROR ||
        (regionType != NULLREGION && pointInRegion);
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
