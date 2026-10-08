#pragma once

#include <windows.h>

namespace snowdesktop
{

// Desktop UI surfaces can cover a monitor without representing a fullscreen
// application. The Shell requires NonRudeHWND before the first show; setting it
// afterwards leaves a fullscreen-detection race. Always create hidden, including
// when a caller supplies WS_VISIBLE, and let the caller perform the first show.
// This Shell hint alone does not prevent Windows automatic do-not-disturb from
// classifying a fullscreen-sized animation surface; also constrain its geometry.
// https://learn.microsoft.com/windows/win32/api/shobjidl_core/nf-shobjidl_core-itaskbarlist2-markfullscreenwindow
inline HWND CreateShellOverlayWindowEx(
    DWORD extendedStyle, LPCWSTR className, LPCWSTR windowName, DWORD style,
    int x, int y, int width, int height, HWND parent, HMENU menu,
    HINSTANCE instance, LPVOID parameter) noexcept
{
    HWND window = CreateWindowExW(extendedStyle, className, windowName,
        style & ~static_cast<DWORD>(WS_VISIBLE),
        x, y, width, height, parent, menu, instance, parameter);
    if (!window)
        return nullptr;

    if (!SetPropW(window, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE)))
    {
        const DWORD error = GetLastError();
        DestroyWindow(window);
        SetLastError(error ? error : ERROR_GEN_FAILURE);
        return nullptr;
    }
    return window;
}

} // namespace snowdesktop
