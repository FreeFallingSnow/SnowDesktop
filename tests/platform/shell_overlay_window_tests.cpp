#include "platform/shell_overlay_window.h"

#include <initializer_list>
#include <iostream>
#include <string>
#include <thread>

namespace
{
int failures = 0;

bool Check(bool value, const char* message)
{
    if (!value)
    {
        ++failures;
        std::cerr << "FAILED: " << message << '\n';
    }
    return value;
}

struct Observation
{
    bool visibleDuringCreate = false;
    unsigned shows = 0;
    unsigned unmarkedShows = 0;
};

LRESULT CALLBACK ObserveWindow(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_NCCREATE)
    {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lp);
        auto* observation = static_cast<Observation*>(creation->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(observation));
        observation->visibleDuringCreate = (creation->style & WS_VISIBLE) != 0;
    }
    if (message == WM_SHOWWINDOW && wp)
    {
        auto* observation = reinterpret_cast<Observation*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        ++observation->shows;
        if (GetPropW(window, L"NonRudeHWND") != reinterpret_cast<HANDLE>(TRUE))
            ++observation->unmarkedShows;
    }
    return DefWindowProcW(window, message, wp, lp);
}

struct Window
{
    HWND handle = nullptr;
    ~Window() { if (handle) DestroyWindow(handle); }
};

void CheckOnPrivateDesktop()
{
    constexpr wchar_t className[] = L"SnowDesktop.ShellOverlay.Test";
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = ObserveWindow;
    windowClass.lpszClassName = className;
    if (!Check(RegisterClassExW(&windowClass) != 0, "register the isolated fixture class"))
        return;

    {
        Observation legacy;
        Window unprotected{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className, L"unprotected", WS_POPUP | WS_VISIBLE,
            0, 0, 1280, 720, nullptr, nullptr, instance, &legacy)};
        if (Check(unprotected.handle != nullptr, "create the pre-change fullscreen-sized fixture"))
        {
            Check(legacy.visibleDuringCreate && legacy.unmarkedShows > 0,
                "the pre-change path reproduces a first show without a Shell exclusion");
        }

        Observation ownerObservation;
        Window owner{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className, L"owner", WS_POPUP, 0, 0, 1280, 720,
            nullptr, nullptr, instance, &ownerObservation)};
        if (Check(owner.handle != nullptr, "create the test-owned parent"))
        {
            ShowWindow(owner.handle, SW_SHOWNOACTIVATE);
            // Covers the independent startup/animation window, the owned
            // floating surface, and the reparented desktop/backdrop child.
            for (const DWORD style : {static_cast<DWORD>(WS_POPUP),
                     static_cast<DWORD>(WS_POPUP | WS_CLIPCHILDREN),
                     static_cast<DWORD>(WS_CHILD)})
            {
                const bool child = (style & WS_CHILD) != 0;
                const HWND parent = style == WS_POPUP ? nullptr : owner.handle;
                constexpr DWORD extendedStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
                Observation observation;
                Window overlay{snowdesktop::CreateShellOverlayWindowEx(
                    extendedStyle, className, L"overlay", style | WS_VISIBLE,
                    11, 17, 1280, 720, parent, nullptr, instance, &observation)};
                if (!Check(overlay.handle != nullptr, "create a protected native overlay"))
                    continue;
                Check(!observation.visibleDuringCreate && observation.shows == 0 &&
                        !IsWindowVisible(overlay.handle),
                    "the caller controls the first show even if it supplies WS_VISIBLE");
                Check(GetPropW(overlay.handle, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
                    "the Shell exclusion exists on the hidden HWND");
                Check((GetWindowLongPtrW(overlay.handle, GWL_STYLE) &
                        (WS_POPUP | WS_CHILD | WS_CLIPCHILDREN)) == style &&
                        GetWindowLongPtrW(overlay.handle, GWL_EXSTYLE) == extendedStyle,
                    "overlay creation preserves window role and activation styles");
                Check((child ? GetParent(overlay.handle) : GetWindow(overlay.handle, GW_OWNER)) == parent,
                    "overlay creation preserves the caller's parent or owner");
                RECT bounds{};
                Check(GetClientRect(overlay.handle, &bounds) &&
                        bounds.right == 1280 && bounds.bottom == 720,
                    "overlay creation preserves fullscreen-sized client geometry");
                Check(SetWindowPos(overlay.handle, nullptr, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE,
                    "the existing no-activation placement path can show the overlay");
                ShowWindow(overlay.handle, SW_HIDE);
                ShowWindow(overlay.handle, SW_SHOWNOACTIVATE);
                Check(observation.shows >= 2 && observation.unmarkedShows == 0,
                    "first show and later hide/show cycles retain the Shell exclusion");
                if (!child && !parent)
                {
                    SetParent(overlay.handle, owner.handle);
                    SetWindowLongPtrW(overlay.handle, GWL_STYLE, WS_CHILD | WS_VISIBLE);
                    Check(GetParent(overlay.handle) == owner.handle &&
                            GetPropW(overlay.handle, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
                        "desktop host reparenting retains the HWND's exclusion");
                }
            }
            Check(GetPropW(owner.handle, L"NonRudeHWND") == nullptr,
                "the overlay helper leaves parent and owner metadata unchanged");
        }

        Observation replacementObservation;
        Window replacement{snowdesktop::CreateShellOverlayWindowEx(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className, L"replacement", WS_POPUP,
            0, 0, 1280, 720, nullptr, nullptr, instance, &replacementObservation)};
        Check(replacement.handle &&
                GetPropW(replacement.handle, L"NonRudeHWND") == reinterpret_cast<HANDLE>(TRUE),
            "a newly recreated overlay receives its own Shell exclusion");
        SetLastError(ERROR_SUCCESS);
        Window invalid{snowdesktop::CreateShellOverlayWindowEx(0,
            L"SnowDesktop.ShellOverlay.Unregistered", L"invalid", WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, instance, nullptr)};
        Check(!invalid.handle && GetLastError() == ERROR_CANNOT_FIND_WND_CLASS,
            "creation failure preserves the Win32 error for the caller");
    }
    Check(UnregisterClassW(className, instance) != FALSE, "release the isolated fixture class");
}
} // namespace

int main()
{
    const std::wstring desktopName = L"SnowDesktop.ShellOverlay." +
        std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr,
        0, GENERIC_ALL, nullptr);
    if (!Check(desktop != nullptr, "create a private desktop for overlay regression"))
        return 1;
    // Never switch the input desktop or interact with SnowDesktop/Explorer.
    std::thread([desktop] {
        if (Check(SetThreadDesktop(desktop) != FALSE, "attach the fixture thread to its private desktop"))
            CheckOnPrivateDesktop();
    }).join();
    Check(CloseDesktop(desktop) != FALSE, "release the private fixture desktop");
    if (failures == 0)
        std::cout << "Shell overlay creation and pre-show metadata regression passed.\n";
    return failures == 0 ? 0 : 1;
}
