#pragma once

#include <windows.h>

namespace snowdesktop::taskbar_monitor
{
inline HMONITOR Resolve(HWND taskbar) noexcept
{
    if (!taskbar || !IsWindow(taskbar)) return nullptr;
    // Auto-hide can move the taskbar wholly beyond a monitor edge. A null
    // intersection must not discard that taskbar's panel and Dock scope rules.
    return MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST);
}
}
