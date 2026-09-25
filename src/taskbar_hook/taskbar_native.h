#pragma once

#include "taskbar_hook_protocol.h"
#include <shellapi.h>

namespace snowdesktop::taskbar_hook::native
{
inline constexpr wchar_t kAttachedProperty[] = L"SnowDesktop.Taskbar.Native.v11";
inline constexpr wchar_t kContextMenuProperty[] = L"SnowDesktop.Taskbar.ContextMenu.v1";

// Called on the owning window thread. The mapping must outlive the subclass.
using AppBarMessage = UINT_PTR(WINAPI*)(DWORD, PAPPBARDATA);
bool Attach(HWND window, SharedState* state, bool classic,
    AppBarMessage appBarMessage = &SHAppBarMessage);
bool IsClassicTaskbarPlatform() noexcept;
// Private Explorer hook entry for menus owned by taskbar child windows.
void ObserveMenuMessage(HWND source, UINT message) noexcept;
}
