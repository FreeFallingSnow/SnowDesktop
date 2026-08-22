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
}
