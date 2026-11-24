#pragma once

#include <windows.h>
#include <shlobj.h>
#include <string>

namespace snowdesktop::shell_open_command
{
// Internal command boundary shared by the launch helper and its regression test.
// This prepares only the default action, never a menu for display.
bool Invoke(IContextMenu* contextMenu, HWND owner, int showCommand,
    const std::wstring& directory, HANDLE acceptedEvent = nullptr);
}
