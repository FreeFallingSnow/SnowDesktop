#pragma once

#include <windows.h>

namespace snowdesktop::power_user_menu
{
// Takes ownership of dockMenu only on success. The caller destroys the root.
HMENU Create(HMENU dockMenu);
// Returns false for commands belonging to the nested Dock menu.
bool Execute(UINT command, HWND owner);
}
