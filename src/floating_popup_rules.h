#pragma once

#include <windows.h>

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
}
