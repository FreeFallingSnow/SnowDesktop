#pragma once

#include "dock_window_peek_cases.h"
#include "../platform/native_import_patch.h"
#include "platform/window_peek_policy.h"

namespace dock_window_transition_suppression_cases
{
using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
inline SetAttribute original = nullptr;
inline HWND source = nullptr;
inline bool failRelease = false;
inline std::vector<std::string> events;

inline HRESULT WINAPI Set(HWND window, DWORD attribute, LPCVOID data, DWORD size)
{
    if (window == source && attribute == DWMWA_TRANSITIONS_FORCEDISABLED &&
        data && size == sizeof(BOOL))
    {
        const bool disabled = *static_cast<const BOOL*>(data) != FALSE;
        events.push_back(disabled ? "suppress" : "release");
        if (!disabled && failRelease) return E_FAIL;
    }
    return original(window, attribute, data, size);
}
inline HRESULT WINAPI Peek(BOOL active, HWND, HWND, UINT, const RECT*)
{
    events.push_back(active ? "peek" : "stop");
    return S_OK;
}

template<class Check>
void Run(Check report)
{
    using snowdesktop::test::NativeImportPatch;
    using dock_preview_thumbnail_cases::Source;
    try
    {
        const auto check = [&](bool value, const char* message) {
            report(value, message);
            if (!value) throw std::runtime_error(message);
        };
        Source target;
        source = target.window;
        check(source != nullptr, "system-transition regression owns its source window");
        ShowWindow(source, SW_SHOWMINNOACTIVE);
        const HWND foreground = GetForegroundWindow();
        NativeImportPatch attribute("DwmSetWindowAttribute", reinterpret_cast<ULONG_PTR>(&Set));
        original = reinterpret_cast<SetAttribute>(attribute.original);
        {
            snowdesktop::ScopedWindowTransitionSuppression suppression;
            events.clear();
            check(suppression.Suppress(source) && IsIconic(source),
                "real DWM accepts a target-only override without restoring the minimized window");
            failRelease = true;
            check(!suppression.Release(), "rejected release remains owned for cleanup retry");
            failRelease = false;
            check(suppression.Release() && events == std::vector<std::string>{"suppress", "release", "release"},
                "cleanup retries the window-level release without changing global animation settings");
        }
        {
            events.clear();
            snowdesktop::dock_peek::Session session(snowdesktop::dock_peek::Api{&Peek});
            check(session.Begin(source, nullptr) && events == std::vector<std::string>{"peek"},
                "hovering does not change source transition policy");
            session.PrepareActivation();
            events.push_back("restore");
            ShowWindow(source, SW_SHOWNOACTIVATE);
            check(session.Stop() && events == std::vector<std::string>{"peek", "suppress", "restore", "stop", "release"},
                "native source transitions are suppressed before restore and released after Peek ends");
            check(!IsIconic(source) && GetForegroundWindow() != source,
                "owned nonactivating restore never gives its source foreground");
        }
        {
            events.clear();
            snowdesktop::ScopedWindowTransitionSuppression suppression;
            check(!suppression.Suppress(source) && events.empty(),
                "ordinary nonminimized activation does not override transition policy");
            ShowWindow(source, SW_SHOWMINNOACTIVE);
            check(suppression.Suppress(source), "destroyed-source override initializes");
            const size_t beforeDestroy = events.size();
            DestroyWindow(source);
            target.window = nullptr;
            check(suppression.Release() && events.size() == beforeDestroy,
                "destroyed or reused source identities never receive a release intended for an old HWND");
        }

        // Production popup setter wraps the actual click callback. The source
        // and DWM attribute setter remain real; only Peek and input delivery
        // are substituted, so this does not exercise a desktop-host window.
        Source clickTarget;
        source = clickTarget.window;
        check(source != nullptr, "popup system-transition fixture owns its source window");
        ShowWindow(source, SW_SHOWMINNOACTIVE);
        dock_window_peek_cases::foreground = foreground;
        dock_window_peek_cases::escape = false;
        NativeImportPatch pointer("TrackMouseEvent", reinterpret_cast<ULONG_PTR>(&dock_window_peek_cases::TrackPointer));
        NativeImportPatch keys("GetAsyncKeyState", reinterpret_cast<ULONG_PTR>(&dock_window_peek_cases::KeyState));
        NativeImportPatch focus("GetForegroundWindow", reinterpret_cast<ULONG_PTR>(&dock_window_peek_cases::Foreground));
        events.clear();
        {
            DockWindowPreview preview(snowdesktop::dock_peek::Api{&Peek});
            unsigned ordinary = 0, committed = 0;
            check(preview.Initialize(GetModuleHandleW(nullptr), [&](HWND) { ++ordinary; }, [](HWND) {}),
                "system-transition popup fixture initializes");
            preview.SetPeekActivateCallback([&](HWND window) {
                const bool valid = window == source && !events.empty() && events.back() == "suppress";
                report(valid,
                    "production handoff disables the system transition before the host activation callback");
                if (!valid) return;
                ++committed;
                events.push_back("restore");
                ShowWindow(window, SW_SHOWNOACTIVATE);
                dock_window_peek_cases::foreground = window;
            });
            preview.Show({{source, L"system transition"}}, {0, 0, 30, 30}, DockPosition::Bottom, false);
            SetWindowPos(preview.GetWindow(), nullptr, -16000, -16000, 0, 0,
                SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(preview.GetWindow(), WM_MOUSEMOVE, 0, dock_window_peek_cases::ContentPoint(preview.GetWindow()));
            check(dock_window_peek_cases::Wait([] { return !events.empty(); }) &&
                events == std::vector<std::string>{"peek"}, "the popup's dwell changes no transition setting");
            SendMessageW(preview.GetWindow(), WM_LBUTTONDOWN, 0, dock_window_peek_cases::ContentPoint(preview.GetWindow()));
            SendMessageW(preview.GetWindow(), WM_LBUTTONUP, 0, dock_window_peek_cases::ContentPoint(preview.GetWindow()));
            check(dock_window_peek_cases::Wait([&] { return preview.IsCleared(); }) && ordinary == 0 && committed == 1 &&
                events == std::vector<std::string>{"peek", "suppress", "restore", "stop", "release"},
                "the real popup click commits once with scoped native-transition suppression and cleanup");
        }
        source = nullptr;
        std::cout << "window-scoped system transition handoff cases completed\n";
    }
    catch (const std::exception& error)
    {
        failRelease = false;
        source = nullptr;
        std::cerr << "system transition fixture: " << error.what() << '\n';
        report(false, "system transition production-path fixture must complete");
    }
}
}
