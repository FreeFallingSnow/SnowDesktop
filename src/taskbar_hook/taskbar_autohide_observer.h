#pragma once

#include "taskbar_autohide_trace.h"

namespace snowdesktop::taskbar_hook::autohide_observer
{
inline constexpr wchar_t kActivationProtectionProperty[] = L"SnowDesktop.Taskbar.AutoHideActivation.v8";
// Message-only diagnostics. Visibility is owned by the native cloak controller;
// no Taskbar.dll internal entry points or downloaded symbols are needed.
void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool enabled,
    bool protectActivation) noexcept;
bool Disable() noexcept;
LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
}
