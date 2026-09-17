#pragma once

#include "taskbar_autohide_trace.h"

namespace snowdesktop::taskbar_hook::autohide_observer
{
// All lifecycle calls run on the taskbar UI thread. No function suppresses a
// native call. Unsupported images retain message-only observations.
void Configure(AutoHideTraceBuffer* buffer, bool enabled) noexcept;
LRESULT Dispatch(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
}
