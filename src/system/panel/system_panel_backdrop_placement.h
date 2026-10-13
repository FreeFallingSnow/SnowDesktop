#pragma once

#include "app/render/desktop_backdrop_compositor.h"

namespace snowdesktop
{
// Activation can raise the content HWND after the pair was shown. Follow its
// final placement without raising the content again or crossing a tray menu.
inline void MaintainSystemPanelBackdropPlacement(DesktopBackdropCompositor& backdrop,
    HWND content, const WINDOWPOS& position, bool glassVisible)
{
    if (glassVisible && IsWindowVisible(content) &&
        ((position.flags & SWP_NOZORDER) == 0 ||
            (position.flags & SWP_SHOWWINDOW) != 0))
        backdrop.Reattach(content);
}
}
