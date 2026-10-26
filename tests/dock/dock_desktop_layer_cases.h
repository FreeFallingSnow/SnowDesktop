#pragma once

#include "dock/floating_dock_rules.h"
#include "app/popup/popup_window_pair_z_order.h"

#include <array>
#include <string>
#include <thread>

namespace snowdesktop::test
{
template <typename Check>
void CheckDockDesktopLayerRestoration(Check&& check)
{
    // Keep real User32 order and band transitions on a private, never-activated
    // desktop. Only application-vs-Shell classification is supplied by fixtures.
    std::thread([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        const std::wstring name = L"SnowDesktop.DockLayer." + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!check(desktop && SetThreadDesktop(desktop), "create isolated Dock desktop-layer fixture"))
        {
            if (desktop) CloseDesktop(desktop);
            return;
        }
        {
            struct Fixture
            {
                std::array<HWND, 6> windows{};
                Fixture()
                {
                    for (auto& window : windows)
                        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"STATIC", L"dock-layer-fixture", WS_POPUP,
                            -32000, -32000, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                }
                ~Fixture() { for (HWND window : windows) if (IsWindow(window)) DestroyWindow(window); }
            } fixture;
            const auto [content, backdrop, shell, lowerApplication, upperApplication, shellTool] = fixture.windows;
            namespace rules = snowdesktop::floating_dock_rules;
            namespace pair = snowdesktop::popup_window_pair_z_order;
            constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            if (check(content && backdrop && shell && lowerApplication && upperApplication && shellTool,
                    "create only test-owned Dock, glass, desktop and application windows"))
            {
                const auto application = [&](HWND window) {
                    return window == lowerApplication || window == upperApplication;
                };
                const auto owned = [&](HWND window) { return window == content || window == backdrop; };
                // Independent expected order is read backwards from each lower
                // fixture, rather than reusing the production success predicate.
                const auto above = [](HWND upper, HWND lower) {
                    for (HWND window = GetWindow(lower, GW_HWNDPREV); window;
                         window = GetWindow(window, GW_HWNDPREV))
                        if (window == upper) return true;
                    return false;
                };
                for (const bool glass : {true, false})
                {
                    const HWND helper = glass ? backdrop : nullptr;
                    const auto prepare = [&] {
                        return SetWindowPos(shellTool, HWND_BOTTOM, 0, 0, 0, 0, flags) &&
                            SetWindowPos(shell, HWND_BOTTOM, 0, 0, 0, 0, flags) &&
                            SetWindowPos(lowerApplication, HWND_NOTOPMOST, 0, 0, 0, 0, flags) &&
                            SetWindowPos(lowerApplication, HWND_TOP, 0, 0, 0, 0, flags) &&
                            SetWindowPos(upperApplication, HWND_NOTOPMOST, 0, 0, 0, 0, flags) &&
                            SetWindowPos(upperApplication, HWND_TOP, 0, 0, 0, 0, flags) &&
                            pair::Apply(content, helper, HWND_TOPMOST, true, {-32000, -32000}, {32, 32});
                    };
                    const auto place = [&](HWND anchor) {
                        return pair::Apply(content, helper, anchor, false, {-32000, -32000}, {32, 32});
                    };
                    const auto expected = [&] {
                        return !pair::IsTopmost(content) && above(lowerApplication, content) &&
                            above(upperApplication, content) && above(content, shell) &&
                            (!glass || (!pair::IsTopmost(backdrop) && pair::IsPaired(content, backdrop)));
                    };
                    const bool resolverPrepared = prepare();
                    const HWND expectedAnchor = above(lowerApplication, upperApplication) ? upperApplication : lowerApplication;
                    check(resolverPrepared && rules::DesktopLayerInsertAfter(content, shell, application, owned) == expectedAnchor,
                        "a still-topmost Dock uses the desktop reference to resolve the lowest ordinary application before demotion");
                    check(prepare() && rules::RestoreDesktopLayer(content, shell, application, owned, place) && expected(),
                        "dismissed Dock stays below ordinary applications and above desktop, with or without glass");
                    unsigned calls = 0;
                    bool changedBand = false;
                    check(prepare() && rules::RestoreDesktopLayer(content, shell, application, owned,
                            [&](HWND anchor) {
                                if (++calls == 1)
                                    changedBand = SetWindowPos(anchor, HWND_TOPMOST, 0, 0, 0, 0, flags) != FALSE;
                                return place(anchor);
                            }) && changedBand && calls == 2 && expected(),
                        "a band-only success after anchor promotion must trigger fresh placement below the remaining ordinary app");
                    calls = 0;
                    check(prepare() && rules::RestoreDesktopLayer(content, shell, application, owned,
                            [&](HWND anchor) { return ++calls == 1 ? false : place(anchor); }) && expected(),
                        "failed native placement is retried against live windows");
                    calls = 0;
                    check(prepare() && !rules::RestoreDesktopLayer(content, shell, application, owned,
                            [&](HWND) { ++calls; return false; }) && calls == 2,
                        "persistent placement failure remains pending after a bounded attempt");
                    calls = 0;
                    check(prepare() && !rules::RestoreDesktopLayer(content, shell, application, owned,
                            [&](HWND anchor) {
                                ++calls;
                                return anchor == upperApplication && place(anchor);
                            }, upperApplication) && calls == 3 &&
                            !pair::IsTopmost(content) && above(upperApplication, content) &&
                            above(content, shell) && !above(lowerApplication, content) &&
                            (!glass || (!pair::IsTopmost(backdrop) && pair::IsPaired(content, backdrop))),
                        "a rejected lowest anchor still lowers the dismissed Dock below the active app while full restoration remains pending");
                    check(prepare() && !rules::RestoreDesktopLayer(content, nullptr, application, owned, place) &&
                            above(lowerApplication, content) && !pair::IsTopmost(content) &&
                            rules::RestoreDesktopLayer(content, shell, application, owned, place) && expected(),
                        "missing Explorer desktop retains recovery while safely demoting below apps, then restores on reattachment");
                    Fixture expiryFixture;
                    const auto [expiredContent, expiredBackdrop, expiredDesktop, firstApp, secondApp, expiredTool] = expiryFixture.windows;
                    const HWND expiredHelper = glass ? expiredBackdrop : nullptr;
                    calls = 0;
                    HWND removedApplication = nullptr;
                    bool removed = false;
                    const bool expiredAnchorRestored = expiredContent && expiredBackdrop && expiredDesktop && firstApp && secondApp && expiredTool &&
                        SetWindowPos(expiredDesktop, HWND_BOTTOM, 0, 0, 0, 0, flags) &&
                        SetWindowPos(firstApp, HWND_TOP, 0, 0, 0, 0, flags) &&
                        SetWindowPos(secondApp, HWND_TOP, 0, 0, 0, 0, flags) &&
                        pair::Apply(expiredContent, expiredHelper, HWND_TOPMOST, true, {-32000, -32000}, {32, 32}) &&
                        rules::RestoreDesktopLayer(expiredContent, expiredDesktop,
                            [&](HWND window) { return window == firstApp || window == secondApp; },
                            [&](HWND window) { return window == expiredContent || window == expiredBackdrop; },
                            [&](HWND anchor) {
                                if (++calls == 1 && (anchor == firstApp || anchor == secondApp))
                                {
                                    removedApplication = anchor;
                                    removed = DestroyWindow(anchor) != FALSE;
                                }
                                return pair::Apply(expiredContent, expiredHelper, anchor, false, {-32000, -32000}, {32, 32});
                            });
                    // DestroyWindow can itself restack siblings. Either attempt
                    // count is valid; the independent final order is mandatory.
                    const HWND remainingApplication = removedApplication == firstApp ? secondApp : firstApp;
                    check(expiredAnchorRestored && removed && !IsWindow(removedApplication) && IsWindow(remainingApplication) &&
                            calls > 0 && calls <= 2 && above(remainingApplication, expiredContent) && above(expiredContent, expiredDesktop) &&
                            !pair::IsTopmost(expiredContent) && (!glass ||
                                (!pair::IsTopmost(expiredBackdrop) && pair::IsPaired(expiredContent, expiredBackdrop))),
                        "an application anchor destroyed during dismissal must be replaced before desktop recovery completes");
                }
            }
        }
        check(SetThreadDesktop(original) != FALSE, "restore original thread desktop after Dock layer fixture");
        check(CloseDesktop(desktop) != FALSE, "release isolated Dock desktop-layer fixture");
    }).join();
}
} // namespace snowdesktop::test
