#pragma once

#include <windows.h>

namespace snowdesktop::tray_notification
{
// Windows can display a tray icon owner's caption as the notification's
// application name. Keep that UI identity separate from the control window
// used by existing versions, hooks and tools for message routing.
// The caller owns the returned hidden window and must destroy it.
HWND CreateOwnerWindow(HWND callbackWindow);
}
