#pragma once

#include "taskbar_autohide_trace.h"
#include "taskbar_autohide_adapter.h"

namespace snowdesktop::taskbar_hook::autohide_observer
{
inline constexpr wchar_t kActivationProtectionProperty[] = L"SnowDesktop.Taskbar.AutoHideActivation.v8";
// Unsupported images retain message-only observations. Native focus hooks and
// reveal hooks must all be available before activation filtering can run.
void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool enabled,
    bool protectActivation, const AutoHideAdapter& adapter = {},
    DWORD resolutionError = ERROR_IO_PENDING) noexcept;
// Restore our native entry points without freeing in-flight trampolines.
bool Disable() noexcept;
LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
}
