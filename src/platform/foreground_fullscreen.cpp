#include "foreground_fullscreen.h"

#include <dwmapi.h>
#include <iterator>
#include <cwchar>

namespace snowdesktop::fullscreen
{
bool ClientCoversMonitor(const RECT& client, const RECT& monitor, LONG edgeInset) noexcept
{
    return monitor.right > monitor.left && monitor.bottom > monitor.top &&
        client.right > client.left && client.bottom > client.top &&
        static_cast<LONGLONG>(client.left) <= static_cast<LONGLONG>(monitor.left) + edgeInset &&
        static_cast<LONGLONG>(client.top) <= static_cast<LONGLONG>(monitor.top) + edgeInset &&
        static_cast<LONGLONG>(client.right) >= static_cast<LONGLONG>(monitor.right) - edgeInset &&
        static_cast<LONGLONG>(client.bottom) >= static_cast<LONGLONG>(monitor.bottom) - edgeInset;
}

bool IsWindowedMaximization(LONG_PTR style, bool maximized) noexcept
{
    // Chromium retains WS_MAXIMIZE after removing its caption and resize frame.
    return maximized && (style & (WS_CAPTION | WS_THICKFRAME)) != 0;
}

bool IsCandidate(LONG_PTR style, LONG_PTR extended, bool maximized, bool shell) noexcept
{
    return !shell && !(extended & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) &&
        !IsWindowedMaximization(style, maximized);
}

bool IsFullscreen(const WindowSample& window, const RECT& monitor) noexcept
{
    if (window.cloaked || window.nonRude ||
        !IsCandidate(window.style, window.extended, window.maximized, window.shell)) return false;
    if (ClientCoversMonitor(window.client, monitor)) return true;
    // Windows Terminal can inset its fullscreen client by two physical pixels
    // for an auto-hidden taskbar. Steam Big Picture (SDL) can also stop its outer
    // frame one pixel short. Bound both against the monitor so their allowances
    // do not accumulate; ordinary work-area windows and framed windows stay out.
    constexpr LONG clientEdgeInset = 2, frameEdgeInset = 1;
    return !(window.style & (WS_CAPTION | WS_THICKFRAME)) &&
        ClientCoversMonitor(window.frame, monitor, frameEdgeInset) &&
        ClientCoversMonitor(window.client, monitor, clientEdgeInset);
}

bool IsShellWindow(HWND window) noexcept
{
    wchar_t name[128]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    return _wcsicmp(name, L"Progman") == 0 || _wcsicmp(name, L"WorkerW") == 0 ||
        _wcsicmp(name, L"Shell_TrayWnd") == 0 || _wcsicmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

std::optional<WindowSample> SampleWindow(
    HWND window, DWORD ignoredProcess) noexcept
{
    if (!window || !IsWindowVisible(window) || IsIconic(window)) return std::nullopt;
    WindowSample sample;
    sample.window = window;
    GetWindowThreadProcessId(window, &sample.process);
    if (!sample.process || sample.process == ignoredProcess) return std::nullopt;
    sample.style = GetWindowLongPtrW(window, GWL_STYLE);
    sample.extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    sample.maximized = IsZoomed(window) != FALSE;
    sample.nonRude = GetPropW(window, L"NonRudeHWND") != nullptr;
    sample.shell = IsShellWindow(window);
    if (sample.shell) return std::nullopt;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        sample.cloaked = cloaked != 0;
    if (sample.cloaked || !GetClientRect(window, &sample.client)) return std::nullopt;
    POINT origin{sample.client.left, sample.client.top}, end{sample.client.right, sample.client.bottom};
    if (!ClientToScreen(window, &origin) || !ClientToScreen(window, &end)) return std::nullopt;
    sample.client = {origin.x, origin.y, end.x, end.y};
    // A failed outer-frame query still permits strict client coverage.
    if (!GetWindowRect(window, &sample.frame)) sample.frame = {};
    return sample;
}

std::optional<WindowSnapshot> ObserveWindow(
    HWND window, DWORD ignoredProcess) noexcept
{
    const auto sample = SampleWindow(window, ignoredProcess);
    if (!sample) return std::nullopt;
    const auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !IsFullscreen(*sample, info.rcMonitor))
        return std::nullopt;
    return WindowSnapshot{window, monitor, sample->process, sample->client, info.rcMonitor};
}

std::optional<WindowSnapshot> ObserveForeground() noexcept
{
    return ObserveWindow(GetForegroundWindow());
}

} // namespace snowdesktop::fullscreen
