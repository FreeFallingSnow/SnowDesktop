#pragma once

#include "taskbar_hook_protocol.h"

namespace snowdesktop::taskbar_hook::native
{
inline constexpr wchar_t kAttachedProperty[] = L"SnowDesktop.Taskbar.Native.v9";

// Called on the owning window thread. The mapping must outlive the subclass.
bool Attach(HWND window, SharedState* state, bool classic);
bool IsClassicTaskbarPlatform() noexcept;
}
