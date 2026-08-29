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

template <typename Handle>
constexpr bool ShouldCancelPointerPressForHostMessage(
    bool cancelMode,
    Handle receivingHost,
    Handle currentCapture,
    bool currentCaptureOwnedByApp,
    bool nextCaptureOwnedByApp)
{
    if (!cancelMode)
        return !nextCaptureOwnedByApp;

    // Hiding one popup host can emit WM_CANCELMODE after the physical press
    // has already transferred capture to another SnowDesktop host. That old
    // window no longer owns the press and must not clear the new host's state.
    return currentCapture == Handle{} ||
        currentCapture == receivingHost ||
        !currentCaptureOwnedByApp;
}

constexpr bool ShouldBeTopmost(
    bool visible,
    int shellPopupMenuLayerDepth)
{
    return visible && shellPopupMenuLayerDepth == 0;
}

template <typename Handle>
constexpr Handle ResolveMenuZOrderOwner(
    bool popupHostVisible,
    Handle popupHost,
    bool dockHostVisible,
    bool dockHostEffectivelyFloating,
    Handle dockHost)
{
    // Menus opened from the shared popup are routed through the desktop input
    // window, so their focus owner does not describe the surface they must
    // outrank. Prefer the active popup host, then retain the floating Dock as
    // the fallback for menus opened from that host. A persistent DockHost can
    // also be visible in the desktop band; using that window as an owner would
    // let an ordinary menu raise the Dock even though no summon occurred.
    if (popupHostVisible && popupHost != Handle{})
        return popupHost;
    if (dockHostVisible && dockHostEffectivelyFloating &&
        dockHost != Handle{})
        return dockHost;
    return Handle{};
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

/**
 * Application-level windows owned by the SnowDesktop process behave like
 * external applications for desktop popup dismissal. Other same-process
 * windows remain internal interaction surfaces.
 */
constexpr bool IsInternalPointerTarget(
    bool targetBelongsToCurrentProcess,
    bool applicationLevelWindow)
{
    return targetBelongsToCurrentProcess &&
        !applicationLevelWindow;
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
