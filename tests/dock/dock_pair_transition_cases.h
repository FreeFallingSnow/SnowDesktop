#pragma once

#include "app/popup/popup_window_pair_z_order.h"
#include <imm.h>

#include <array>
#include <string>
#include <thread>

namespace snowdesktop::test::dock_pair_transition
{
namespace pair = snowdesktop::popup_window_pair_z_order;

struct Observation
{
    std::array<std::array<HWND, 2>, 2> pairs{};
    unsigned changes = 0;
    unsigned exposedAboveContent = 0;
    bool requireVisible = false;
};
inline thread_local Observation* observation = nullptr;

inline LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    const LRESULT result = DefWindowProcW(window, message, wp, lp);
    if (observation && message == WM_WINDOWPOSCHANGED)
    {
        for (const auto& [content, backdrop] : observation->pairs)
        {
            if (window != content && window != backdrop) continue;
            ++observation->changes;
            // Final adjacency alone misses a helper that briefly samples its
            // own content while a Dock is lowered behind an application.
            if ((!observation->requireVisible || IsWindowVisible(backdrop)) && pair::IsAbove(backdrop, content))
                ++observation->exposedAboveContent;
        }
    }
    return result;
}

template<class Check>
void Run(Check check, bool nativeVisibility = false)
{
    std::thread([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        const std::wstring name = L"SnowDesktop.DockPairTransition." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentThreadId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!check(desktop && SetThreadDesktop(desktop),
                "Dock pair sequence uses a private desktop that is never switched to"))
        {
            if (desktop) CloseDesktop(desktop);
            return;
        }
        if (nativeVisibility) ImmDisableIME(0);
        constexpr wchar_t className[] = L"SnowDesktop.DockPairTransition";
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        const bool registered = RegisterClassW(&wc) != 0;
        std::array<HWND, 5> windows{};
        bool created = registered;
        for (auto& window : windows)
        {
            // Hidden by default; the fallback probe can show them on this
            // private desktop only, without activation or input. Observe
            // real native placement callbacks; no rasterizer,
            // application Dock state, or Windows API outcome is substituted.
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                className, L"", WS_POPUP | (nativeVisibility ? WS_VISIBLE : 0), 0, 0, 32, 32,
                nullptr, nullptr, wc.hInstance, nullptr);
            created = created && window;
        }
        if (check(created, "create test-owned Dock, popup and ordinary application anchor"))
        {
            const auto [dock, glass, popup, popupGlass, anchor] = windows;
            Observation observed{{{{dock, glass}, {popup, popupGlass}}}};
            observed.requireVisible = nativeVisibility;
            const auto place = [&](HWND content, HWND backdrop, HWND after, bool topmost, const char* step) {
                return check(pair::Apply(content, backdrop, after, topmost, {0, 0}, {32, 32}) &&
                    pair::IsTopmost(content) == topmost &&
                    pair::IsTopmost(backdrop) == topmost && pair::IsPaired(content, backdrop) &&
                    (IsWindowVisible(content) != FALSE) == nativeVisibility &&
                    (IsWindowVisible(backdrop) != FALSE) == nativeVisibility, step);
            };
            const bool prepared = place(dock, glass, HWND_NOTOPMOST, false, "prepare ordinary Dock pair") &&
                place(popup, popupGlass, HWND_NOTOPMOST, false, "prepare ordinary popup pair");
            bool placed = prepared;
            observation = &observed;
            for (unsigned iteration = 0; placed && iteration < 128; ++iteration)
            {
                // A popup can outlive a Dock's dismissal; immediately reveal
                // the Dock again, then exercise the sentinel close as well.
                placed = place(dock, glass, HWND_TOPMOST, true, "summon Dock with paired glass") &&
                    place(popup, popupGlass, HWND_TOPMOST, true, "open independent popup above Dock") &&
                    place(dock, glass, anchor, false, "lower Dock below application while popup remains") && pair::IsAbove(anchor, dock) &&
                    place(popup, popupGlass, HWND_NOTOPMOST, false, "retire popup into ordinary band") &&
                    place(dock, glass, HWND_TOPMOST, true, "summon Dock immediately after popup retirement") &&
                    place(dock, glass, HWND_NOTOPMOST, false, "dismiss Dock to ordinary band without concrete anchor") &&
                    place(dock, glass, anchor, false, "place ordinary Dock behind concrete application anchor") && pair::IsAbove(anchor, dock);
            }
            observation = nullptr;
            check(placed && observed.changes > 0,
                "rapid summon, popup retirement and desktop dismissal keep native bands and final pairs valid");
            check(observed.exposedAboveContent == 0,
                "no native transition callback exposes visible glass above its own content");
        }
        for (const HWND window : windows)
            if (window) DestroyWindow(window);
        if (registered) UnregisterClassW(className, wc.hInstance);
        check(SetThreadDesktop(original) != FALSE, "restore test thread desktop after Dock pair transitions");
        check(CloseDesktop(desktop) != FALSE, "release private Dock pair transition desktop");
    }).join();
}
} // namespace snowdesktop::test::dock_pair_transition
