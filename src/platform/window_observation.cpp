#include "platform/window_observation.h"
#include "platform/fullscreen_rules.h"
#include "diagnostics/performance_trace.h"

#include <array>
#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace snowdesktop::window_observation
{
namespace
{
bool IsAppWindow(HWND window, LONG_PTR extended) noexcept
{
    return GetAncestor(window, GA_ROOT) == window &&
        !(extended & WS_EX_TOOLWINDOW) &&
        (!GetWindow(window, GW_OWNER) || (extended & WS_EX_APPWINDOW));
}

WindowPlacement Placement(HMONITOR monitor, bool maximized, const RECT& frame,
    const MONITORINFO* geometry) noexcept
{
    return {monitor, maximized, geometry &&
        fullscreen::ClientCoversMonitor(frame, geometry->rcMonitor, 2)};
}

RECT DesktopCoverage(const fullscreen::WindowSample& window, const MONITORINFO& monitor)
{
    if (window.nonRude ||
        (window.extended & (WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)))
        return {};
    if (!fullscreen::IsFullscreen(window, monitor.rcMonitor) &&
        !(window.maximized && fullscreen::ClientCoversMonitor(window.frame, monitor.rcWork)))
        return {};

    // Only the visible frame can cover desktop pixels. A custom region
    // may expose pixels inside its bounding rectangle.
    RECT visible{}, region{}, covered{};
    if (GetWindowRgnBox(window.window, &region) != ERROR ||
        FAILED(DwmGetWindowAttribute(window.window, DWMWA_EXTENDED_FRAME_BOUNDS,
            &visible, sizeof(visible))) ||
        !fullscreen::ClientCoversMonitor(visible, monitor.rcWork, 2))
        return {};
    IntersectRect(&covered, &visible, &monitor.rcMonitor);
    return covered;
}
}

std::optional<WindowPlacement> ObservePlacement(HWND window) noexcept
{
    if (!window || !IsWindowVisible(window) || IsIconic(window) ||
        !IsAppWindow(window, GetWindowLongPtrW(window, GWL_EXSTYLE)))
        return std::nullopt;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (!process || process == GetCurrentProcessId() || fullscreen::IsShellWindow(window))
        return std::nullopt;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    RECT frame{};
    const bool geometryKnown = monitor && GetWindowRect(window, &frame) &&
        GetMonitorInfoW(monitor, &info);
    return Placement(monitor, IsZoomed(window) != FALSE, frame, geometryKnown ? &info : nullptr);
}

Snapshot Observe(std::span<const RECT> pages, HWND desktopRoot)
{
    performance::Scope trace("shared.windows", "observe");
    std::unordered_map<HMONITOR, MONITORINFO> geometry;
    std::vector<HMONITOR> monitors;
    std::vector<std::optional<RECT>> monitorBounds;
    bool monitorsKnown = !pages.empty();
    for (const auto& page : pages)
    {
        const HMONITOR monitor = MonitorFromRect(&page, MONITOR_DEFAULTTONULL);
        MONITORINFO info{sizeof(info)};
        if (IsRectEmpty(&page) || !monitor || !GetMonitorInfoW(monitor, &info) ||
            !EqualRect(&page, &info.rcMonitor))
        {
            monitorsKnown = false;
            continue;
        }
        if (geometry.try_emplace(monitor, info).second)
        {
            monitors.push_back(monitor);
            monitorBounds.emplace_back(info.rcMonitor);
        }
    }
    const int monitorCount = GetSystemMetrics(SM_CMONITORS);
    monitorsKnown = monitorsKnown && monitorCount > 0 &&
        monitors.size() == static_cast<std::size_t>(monitorCount);

    Microsoft::WRL::ComPtr<IVirtualDesktopManager> virtualDesktop;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&virtualDesktop));
    Snapshot result;
    struct Scan
    {
        const std::unordered_map<HMONITOR, MONITORINFO>& geometry;
        Snapshot& result;
        IVirtualDesktopManager* virtualDesktop;
        HWND desktopRoot;
        bool reachedDesktop = false;
        std::vector<fullscreen::WindowSample> samples;
    } scanContext{geometry, result, virtualDesktop.Get(), desktopRoot, false, {}};
    if (!EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& scan = *reinterpret_cast<Scan*>(value);
        if (window == scan.desktopRoot) scan.reachedDesktop = true;
        const auto sample = fullscreen::SampleWindow(window);
        if (!sample) return TRUE;
        BOOL currentDesktop = TRUE;
        if (scan.virtualDesktop && SUCCEEDED(scan.virtualDesktop->IsWindowOnCurrentVirtualDesktop(
                window, &currentDesktop)) && !currentDesktop)
            return TRUE;
        scan.samples.push_back(*sample);
        if (!IsAppWindow(window, sample->extended)) return TRUE;
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        if (!monitor) return TRUE;
        const auto found = scan.geometry.find(monitor);
        const auto* monitorGeometry = found == scan.geometry.end() ? nullptr : &found->second;
        scan.result.placements.emplace(window, Placement(
            monitor, sample->maximized, sample->frame, monitorGeometry));
        auto& state = scan.result.monitors[monitor];
        state.visible = true;
        state.maximized = state.maximized || sample->maximized;
        if (!scan.reachedDesktop)
            for (const auto& [display, bounds] : scan.geometry)
            {
                const RECT covered = DesktopCoverage(*sample, bounds);
                if (IsRectEmpty(&covered)) continue;
                auto& displayState = scan.result.monitors[display];
                if (IsRectEmpty(&displayState.desktopCoverage))
                    displayState.desktopCoverage = covered;
            }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scanContext))) return {};

    // Fullscreen keeps its own Z-order policy while using the same sample of
    // top-level windows as maximization and desktop coverage.
    const auto sources = fullscreen::ScanMonitors(monitorBounds, [&](const auto& visit) {
        for (const auto& sample : scanContext.samples)
            if (!visit(sample)) break;
        return true;
    });
    for (std::size_t i = 0; i < monitors.size(); ++i)
    {
        if (!monitorsKnown && !result.monitors.contains(monitors[i]) &&
            (!sources[i] || !sources[i]->window))
            continue;
        auto& state = result.monitors[monitors[i]];
        if (sources[i]) state.fullscreenSource = sources[i]->window;
        if (!scanContext.reachedDesktop) state.desktopCoverage = {};
        if (!IsRectEmpty(&state.desktopCoverage))
            result.desktopCoverage.push_back(state.desktopCoverage);
    }
    return result;
}

std::optional<fullscreen::WindowSnapshot> RecheckFullscreenSource(
    HMONITOR monitor, HWND knownSource)
{
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return std::nullopt;
    const HWND foreground = GetForegroundWindow();
    const std::array bounds{std::optional{info.rcMonitor}};
    const auto sources = fullscreen::ScanMonitors(bounds, [&](const auto& visit) {
        if (foreground && MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL) == monitor)
            if (const auto sample = fullscreen::SampleWindow(foreground); sample && !visit(*sample))
                return true;
        if (knownSource && knownSource != foreground)
            if (const auto sample = fullscreen::SampleWindow(knownSource))
                visit(*sample);
        return true;
    });
    const auto& source = sources.front();
    if (!source || !source->window) return std::nullopt;
    return fullscreen::WindowSnapshot{source->window, monitor, source->process,
        source->client, info.rcMonitor};
}
}
