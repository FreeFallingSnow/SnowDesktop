#pragma once

#include "app/render/desktop_backdrop_compositor.h"

namespace snowdesktop
{
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
