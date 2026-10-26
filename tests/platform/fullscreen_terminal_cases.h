#pragma once
#include "platform/foreground_fullscreen.h"
#include "dock/dock_fullscreen_policy.h"
#include "system/status_bar/status_bar_appbar.h"
#include <array>
#include <string>
#include <thread>

// Reproduce the user's Terminal sample on a private desktop. Only the external
// process exclusion is substituted; window geometry, native sampling, scanning,
// focus guarding and the status-bar hide state use their production paths.
template<class Check>
void CheckTerminalFullscreenObservation(Check&& check)
{
    namespace observation = snowdesktop::fullscreen;
    const auto desktopName = L"SnowTerminalFullscreen_" + std::to_wstring(GetCurrentProcessId()) +
        L"_" + std::to_wstring(GetTickCount64());
    const auto desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
    check(desktop != nullptr, "create private desktop for the original terminal inset sample");
    if (!desktop) return;
    std::thread fixture([&] {
        if (!SetThreadDesktop(desktop)) { check(false, "attach private terminal fixture desktop"); return; }
        if (!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        { check(false, "measure terminal inset in physical pixels"); return; }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW cls{};
        cls.hInstance = instance;
        cls.lpszClassName = L"SnowTerminalFullscreenFixture";
        cls.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_NCCALCSIZE)
            {
                auto& client = wp ? reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0]
                                  : *reinterpret_cast<RECT*>(lp);
                client.bottom -= static_cast<LONG>(GetWindowLongPtrW(window, GWLP_USERDATA));
                return 0;
            }
            return DefWindowProcW(window, message, wp, lp);
        };
        if (!RegisterClassW(&cls)) { check(false, "register private terminal fixture"); return; }
        const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info))
        { check(false, "read terminal fixture monitor"); UnregisterClassW(cls.lpszClassName, instance); return; }
        const auto rect = info.rcMonitor;
        const auto window = CreateWindowExW(0, cls.lpszClassName, L"Private terminal inset", WS_POPUP,
            rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, nullptr);
        check(window != nullptr, "create native terminal fullscreen fixture");
        if (window)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 2);
            SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            const auto sample = observation::SampleWindow(window, 0);
            check(sample && sample->client.bottom == rect.bottom - 2 && EqualRect(&sample->frame, &rect),
                "terminal sample has a complete outer frame and a client two pixels short");
            const auto direct = observation::ObserveWindow(window, 0);
            const std::array monitors{monitor};
            const auto scanned = observation::ObserveMonitors(monitors, 0);
            check(direct && scanned.front() && scanned.front()->window == window &&
                observation::ObserveMonitor(monitor, 0).has_value(),
                "window, monitor and shared multi-monitor detection recognize the original terminal sample");
            snowdesktop::StatusBarFullscreenState bar;
            bar.Observe(scanned.front() ? scanned.front()->window : nullptr, false, false, false);
            check(bar.ShouldHide(false), "shared terminal observation drives independent status-bar hiding");
            int activations = 0;
            check(snowdesktop::dock_fullscreen::BlocksReveal(DockFullscreenPolicy::FullProtection,
                    DockRevealSource::Gesture, direct.has_value(), true) &&
                !observation::GuardFocusRequest(false, [&] { return observation::ObserveWindow(window, 0); },
                    [&] { ++activations; }) && activations == 0,
                "terminal inset blocks protected Dock reveal and automatic focus takeover");
            SetWindowLongPtrW(window, GWLP_USERDATA, 3);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
            check(!observation::ObserveWindow(window, 0) && !observation::ObserveMonitor(monitor, 0),
                "native client insets larger than the terminal sample remain windowed");
            SetWindowLongPtrW(window, GWLP_USERDATA, 2);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
            check(observation::ObserveWindow(window, 0).has_value(), "restoring the terminal client inset restores observation");
            ShowWindow(window, SW_HIDE);
            check(!observation::ObserveWindow(window, 0), "hidden terminal windows cannot hold fullscreen protection");
            DestroyWindow(window);
        }
        UnregisterClassW(cls.lpszClassName, instance);
    });
    fixture.join();
    CloseDesktop(desktop);
}
