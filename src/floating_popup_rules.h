#pragma once

#include <windows.h>

#include <bit>
#include <cstdint>

namespace snowdesktop::floating_popup_rules
{
inline constexpr DWORD kWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;

constexpr bool HostsCollectionPopup(bool popupOpen)
{
    return popupOpen;
}

constexpr bool HostsLuaPanel(bool panelOpen)
{
    return panelOpen;
}

constexpr bool ShouldShow(
    bool hostsCollectionPopup,
    bool hostsLuaPanel)
{
    return hostsCollectionPopup || hostsLuaPanel;
}

constexpr bool ShouldRevealHost(
    bool wasVisible,
    bool immediatePresent)
{
    // A hidden shared host can still own the last popup type's committed
    // DComp surface. Keep staging passes hidden until the new content has
    // been presented, otherwise the stale surface is exposed once while
    // switching between collection and Lua popups.
    return !wasVisible && immediatePresent;
}

constexpr bool ShouldBeTopmost(
    bool visible,
    int shellPopupMenuLayerDepth)
{
    return visible && shellPopupMenuLayerDepth == 0;
}

constexpr POINT AnimationVisualOffset(
    const RECT& animationBounds,
    const RECT& hostBounds)
{
    return POINT{
        animationBounds.left - hostBounds.left,
        animationBounds.top - hostBounds.top,
    };
}

constexpr std::uint64_t PackScreenPoint(POINT point)
{
    return static_cast<std::uint64_t>(
               static_cast<std::uint32_t>(point.x)) |
        (static_cast<std::uint64_t>(
             static_cast<std::uint32_t>(point.y)) << 32);
}

constexpr POINT UnpackScreenPoint(std::uint64_t packed)
{
    return POINT{
        static_cast<LONG>(std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(packed))),
        static_cast<LONG>(std::bit_cast<std::int32_t>(
            static_cast<std::uint32_t>(packed >> 32))),
    };
}

constexpr bool IsCurrentPointerNotification(
    std::uint32_t notificationGeneration,
    std::uint32_t currentGeneration)
{
    return notificationGeneration != 0 &&
        notificationGeneration == currentGeneration;
}

constexpr bool ContainsHostedPopupPoint(
    const RECT& bounds,
    POINT point,
    LONG padding = 3)
{
    return bounds.right > bounds.left &&
        bounds.bottom > bounds.top &&
        point.x >= bounds.left - padding &&
        point.x < bounds.right + padding &&
        point.y >= bounds.top - padding &&
        point.y < bounds.bottom + padding;
}

constexpr bool IsPointOnHostedPopupSurface(
    POINT point,
    const RECT& collectionBounds,
    const RECT& luaPanelBounds)
{
    return ContainsHostedPopupPoint(
            collectionBounds, point) ||
        ContainsHostedPopupPoint(
            luaPanelBounds, point);
}

constexpr bool ShouldDismissForExternalPointerDown(
    bool popupVisible,
    bool targetBelongsToCurrentProcess,
    bool dragActive)
{
    return popupVisible &&
        !targetBelongsToCurrentProcess &&
        !dragActive;
}

constexpr bool ShouldDismissLuaPanelForExternalPointerDown(
    bool panelVisible,
    bool dismissOnOutside,
    bool targetBelongsToCurrentProcess,
    bool dragActive)
{
    return dismissOnOutside &&
        ShouldDismissForExternalPointerDown(
            panelVisible,
            targetBelongsToCurrentProcess,
            dragActive);
}

template <typename Handle>
constexpr bool ShouldReleaseRecordedPanelCapture(
    Handle recordedCapture,
    Handle currentCapture)
{
    return recordedCapture != Handle{} &&
        recordedCapture == currentCapture;
}
}
