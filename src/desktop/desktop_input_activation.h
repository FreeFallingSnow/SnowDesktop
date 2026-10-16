#pragma once

#include <windows.h>

namespace snowdesktop::desktop_input_activation
{
// Passive refreshes must preserve an open panel's keyboard focus. An explicit
// pointer press is different: cancel the panel before restoring host input,
// even while its closing animation still reports it as open.
template <typename DismissPanel, typename RestoreHost>
void RestoreInteractionFocus(bool panelOpen, bool pointerPress,
    DismissPanel dismissPanel, RestoreHost restoreHost)
{
    if (panelOpen)
    {
        if (!pointerPress) return;
        dismissPanel();
    }
    restoreHost();
}

// Internal host policy: the Explorer-owned rendering child must never become
// a popup's native owner or its post-menu keyboard focus target. Keep explicit
// non-desktop owners (tray/settings) and floating-session input ownership.
inline HWND ResolveMenuOwner(HWND requested, HWND desktopRender,
    HWND desktopInput, HWND floatingInput, bool floatingSession)
{
    if (!requested || requested != desktopRender)
        return requested;
    if (floatingSession && floatingInput && IsWindow(floatingInput))
        return floatingInput;
    return desktopInput && IsWindow(desktopInput) ? desktopInput : nullptr;
}
}
