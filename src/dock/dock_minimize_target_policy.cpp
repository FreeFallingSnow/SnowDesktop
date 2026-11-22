#include "dock_minimize_target_policy.h"
#include "../platform/foreground_fullscreen.h"

namespace snowdesktop::dock_minimize
{
bool UsesNativeTransition(HWND window) noexcept
{
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (!(style & WS_VISIBLE) || (style & WS_MINIMIZEBOX)) return false;
    if (!(style & WS_MINIMIZE)) return fullscreen::ObserveWindow(window).has_value();

    // An iconic window has no usable on-screen client rectangle. For a
    // borderless window, its normal placement can still identify fullscreen.
    if (style & (WS_CAPTION | WS_THICKFRAME)) return false;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !GetWindowPlacement(window, &placement))
        return false;

    fullscreen::WindowSample sample;
    sample.style = style;
    sample.extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    sample.nonRude = GetPropW(window, L"NonRudeHWND") != nullptr;
    sample.frame = placement.rcNormalPosition;
    // Top-level non-tool windows report placement in workspace coordinates.
    if (!(sample.extended & WS_EX_TOOLWINDOW))
        OffsetRect(&sample.frame, info.rcWork.left - info.rcMonitor.left,
            info.rcWork.top - info.rcMonitor.top);
    sample.client = sample.frame;
    return fullscreen::IsFullscreen(sample, info.rcMonitor);
}
}
