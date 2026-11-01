#pragma once

#include "app/render/desktop_backdrop_compositor.h"

namespace snowdesktop
{
// An elevated tray menu cannot be used as hWndInsertAfter by the host, even
// when only the host's own HWND is being moved. A confirmed topmost menu needs
// no foreign anchor: lower our pair to the top of the ordinary band instead.
// Ordinary menus still need their concrete anchor to preserve relative order.
inline bool PlaceSystemPanelBelowTrayMenu(DesktopBackdropCompositor& backdrop,
    HWND content, HWND menu)
{
    if (!menu || !IsWindowVisible(menu)) return false;
    const bool menuTopmost = (GetWindowLongPtrW(menu, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    return backdrop.SetPopupWindowPairZOrder(content,
        menuTopmost ? HWND_NOTOPMOST : menu, false,
        menuTopmost ? nullptr : menu);
}

// Activation can raise the content HWND after the pair was shown. Follow its
// final placement without raising the content again or crossing a tray menu.
inline void MaintainSystemPanelBackdropPlacement(DesktopBackdropCompositor& backdrop,
    HWND content, bool glassVisible)
{
    // Another window can split the pair without changing content placement.
    // Reattach checks the actual pair and is a no-op when already synchronized.
    if (glassVisible && IsWindowVisible(content))
        backdrop.Reattach(content);
}
}
