#pragma once

#include "taskbar_autohide_trace.h"
#include "taskbar_autohide_adapter.h"

namespace snowdesktop::taskbar_hook::autohide_observer
{
// Unsupported images retain message-only observations. Native focus hooks and
// reveal hooks must all be available before activation filtering can run.
void Configure(AutoHideTraceBuffer* buffer, HWND taskbar, bool enabled,
    bool protectActivation, const AutoHideAdapter& adapter = {},
    DWORD resolutionError = ERROR_IO_PENDING) noexcept;
void Disable() noexcept;
LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
}
