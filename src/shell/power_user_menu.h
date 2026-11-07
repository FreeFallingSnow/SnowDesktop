#pragma once

#include <windows.h>
#include <functional>

namespace snowdesktop::power_user_menu
{
using IconSetter = std::function<void(HMENU, UINT_PTR, const wchar_t*)>;
// Takes ownership of dockMenu only on success. The caller destroys the root.
HMENU Create(HMENU dockMenu, const IconSetter& setIcon);
bool IsRootMenu(HMENU menu) noexcept;
// Returns false for commands belonging to the nested Dock menu.
bool Execute(UINT command, HWND owner);
// Shared host entry for the status-bar system menu; owner must be durable.
bool OpenTerminal(HWND owner, bool administrator);
}
