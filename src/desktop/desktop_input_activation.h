#pragma once

#include <windows.h>

namespace snowdesktop::desktop_input_activation
{
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
