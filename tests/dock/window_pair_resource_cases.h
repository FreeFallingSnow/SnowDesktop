#pragma once

#include "app/popup/popup_window_pair_z_order.h"
#include "../platform/native_import_patch.h"

#include <cstring>
#include <future>
#include <iostream>
#include <thread>

namespace snowdesktop::test::window_pair_resources
{
namespace pair = snowdesktop::popup_window_pair_z_order;
inline decltype(&DeferWindowPos) nativeDefer = nullptr;
inline decltype(&SetWindowPos) nativeSet = nullptr;
struct Observation
{
    HWND content = nullptr, backdrop = nullptr, anchor = nullptr;
    bool expire = false;
    unsigned expired = 0, exposedGlass = 0;
};
inline thread_local Observation* observation = nullptr;

inline HWND Create(const wchar_t* className = L"STATIC")
{
    return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, className, L"",
        WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}
inline LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_APP + 1) return reinterpret_cast<LRESULT>(Create());
    if (message == WM_CLOSE) { DestroyWindow(window); PostQuitMessage(0); return 0; }
    const auto result = DefWindowProcW(window, message, wp, lp);
    if (observation && message == WM_WINDOWPOSCHANGED &&
        window == observation->backdrop && IsWindowVisible(window) &&
        pair::IsAbove(window, observation->content)) ++observation->exposedGlass;
    return result;
}
inline void ExpireAnchor(HWND after)
{
    if (!observation || !observation->expire || after != observation->anchor) return;
    observation->expire = false;
    const DWORD error = GetLastError();
    DWORD_PTR ignored = 0;
    // Only the fixture's other thread receives this close. Keep User32's real
    // failure after the lifetime change; never fabricate a native API result.
    if (SendMessageTimeoutW(after, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 2000, &ignored) && !IsWindow(after))
        ++observation->expired;
    SetLastError(error);
}
inline HDWP WINAPI ObserveDefer(HDWP batch, HWND window, HWND after,
    int x, int y, int width, int height, UINT flags)
{
    ExpireAnchor(after);
    return nativeDefer(batch, window, after, x, y, width, height, flags);
}
inline BOOL WINAPI ObserveSet(HWND window, HWND after, int x, int y, int width, int height, UINT flags)
{
    ExpireAnchor(after);
    return nativeSet(window, after, x, y, width, height, flags);
}

template<class Check>
void Run(Check check)
{
    // USER-count assertions run before this suite initializes WinRT rendering.
    // All windows belong to a private desktop that is never switched to.
    std::thread([&] {
        const HDESK original = GetThreadDesktop(GetCurrentThreadId());
        const std::wstring name = L"SnowDesktop.WindowPairResources." + std::to_wstring(GetCurrentProcessId());
        const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!check(desktop && SetThreadDesktop(desktop), "create private desktop for HDWP resource regression"))
        { if (desktop) CloseDesktop(desktop); return; }
        constexpr wchar_t className[] = L"SnowDesktop.WindowPairResources";
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WindowProc; wc.lpszClassName = className;
        const bool registered = RegisterClassW(&wc) != 0;
        const HWND content = registered ? Create(className) : nullptr;
        const HWND backdrop = registered ? Create(className) : nullptr;
        const HWND separator = registered ? Create() : nullptr;
        std::promise<HWND> ready;
        auto factoryResult = ready.get_future();
        std::thread anchorThread([&] {
            if (!SetThreadDesktop(desktop)) { ready.set_value(nullptr); return; }
            const HWND factory = registered ? Create(className) : nullptr;
            ready.set_value(factory);
            if (!factory) return;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
        });
        const HWND factory = factoryResult.get();
        if (check(content && backdrop && separator && factory, "create owned pair and separate-thread anchor factory"))
        {
            const auto deferAddress = GetProcAddress(GetModuleHandleW(L"user32.dll"), "DeferWindowPos");
            const auto setAddress = GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowPos");
            static_assert(sizeof(nativeDefer) == sizeof(deferAddress) && sizeof(nativeSet) == sizeof(setAddress));
            std::memcpy(&nativeDefer, &deferAddress, sizeof(nativeDefer));
            std::memcpy(&nativeSet, &setAddress, sizeof(nativeSet));
            NativeImportPatch deferPatch("DeferWindowPos", reinterpret_cast<ULONG_PTR>(&ObserveDefer));
            NativeImportPatch setPatch("SetWindowPos", reinterpret_cast<ULONG_PTR>(&ObserveSet));
            Observation observed{content, backdrop};
            observation = &observed;
            constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
            constexpr unsigned iterations = 64;
            for (const bool visible : {false, true})
            {
                ShowWindow(content, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
                ShowWindow(backdrop, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
                for (const bool topmost : {false, true})
                {
                    const HWND band = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
                    bool stable = true, rejected = true, restored = true;
                    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
                    const unsigned expiredBefore = observed.expired;
                    for (unsigned i = 0; i < iterations; ++i)
                    {
                        const HWND anchor = reinterpret_cast<HWND>(SendMessageW(factory, WM_APP + 1, 0, 0));
                        if (!anchor) { stable = false; break; }
                        nativeSet(anchor, topmost ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0, flags);
                        nativeSet(content, band, 0, 0, 0, 0, flags);
                        nativeSet(content, HWND_TOP, 0, 0, 0, 0, flags);
                        nativeSet(backdrop, content, 0, 0, 32, 32, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                        nativeSet(separator, band, 0, 0, 0, 0, flags);
                        nativeSet(separator, content, 0, 0, 0, 0, flags);
                        nativeSet(backdrop, separator, 0, 0, 0, 0, flags);
                        // A live concrete anchor must still work without any
                        // resource accumulation before exercising its lifetime.
                        stable = pair::Apply(content, backdrop, anchor, topmost, {}, {32, 32}) && stable;
                        nativeSet(content, band, 0, 0, 0, 0, flags);
                        nativeSet(content, HWND_TOP, 0, 0, 0, 0, flags);
                        nativeSet(backdrop, content, 0, 0, 32, 32, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                        nativeSet(separator, band, 0, 0, 0, 0, flags);
                        nativeSet(separator, content, 0, 0, 0, 0, flags);
                        nativeSet(backdrop, separator, 0, 0, 0, 0, flags);
                        observed.anchor = anchor; observed.expire = true;
                        rejected = !pair::Apply(content, backdrop, anchor, topmost, {}, {32, 32}) && rejected;
                        observed.expire = false;
                        if (IsWindow(anchor)) SendMessageW(anchor, WM_CLOSE, 0, 0);
                        restored = pair::IsTopmost(content) == topmost && pair::IsTopmost(backdrop) == topmost &&
                            pair::IsPaired(content, backdrop) &&
                            (IsWindowVisible(content) != FALSE) == visible &&
                            (IsWindowVisible(backdrop) != FALSE) == visible && restored;
                        // Recovery must leave both later sentinel placement and
                        // another concrete placement usable on the same pair.
                        stable = pair::Apply(content, backdrop, band, topmost, {}, {32, 32}) && stable;
                    }
                    const DWORD after = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
                    if (after != before)
                        std::cerr << "HDWP regression visible=" << visible << " topmost=" << topmost
                            << " USER before=" << before << " after=" << after << '\n';
                    check(after == before, "expired native placement anchors must not accumulate USER objects");
                    check(observed.expired - expiredBefore == iterations && rejected,
                        "each anchor expires at its first native reference and incomplete placement remains rejected");
                    check(restored && stable, "failed anchors retain pair visibility/bands and allow later recovery");
                }
            }
            check(observed.exposedGlass == 0, "native fallback callbacks never expose glass above content");
            observation = nullptr;
        }
        if (factory) SendMessageW(factory, WM_CLOSE, 0, 0);
        anchorThread.join();
        if (separator) DestroyWindow(separator);
        if (backdrop) DestroyWindow(backdrop);
        if (content) DestroyWindow(content);
        if (registered) UnregisterClassW(className, wc.hInstance);
        check(SetThreadDesktop(original) != FALSE, "restore desktop after HDWP resource regression");
        check(CloseDesktop(desktop) != FALSE, "close private HDWP regression desktop");
    }).join();
}
} // namespace snowdesktop::test::window_pair_resources
