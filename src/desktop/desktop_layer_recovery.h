#pragma once

#include <windows.h>

namespace snowdesktop::desktop_layer_recovery
{
enum class Status { Unchanged, Deferred, Restored, Failed };

struct Conditions
{
    bool customDesktopVisible = false;
    bool presentationReady = false;
    bool interactionActive = false;
};

struct Result
{
    Status status = Status::Unchanged;
    HWND previous = nullptr;
    HWND insertAfter = nullptr;
    DWORD error = ERROR_SUCCESS;
};

inline bool IsAbove(HWND window, HWND sibling) noexcept
{
    // Bound the walk because Explorer owns the sibling list and can change it.
    HWND current = GetWindow(window, GW_HWNDNEXT);
    for (unsigned remaining = 256; current && remaining; --remaining)
    {
        if (current == sibling) return true;
        current = GetWindow(current, GW_HWNDNEXT);
    }
    return false;
}

// Only repair the application's child relative to the native desktop view.
// No foreground activation, parent promotion, wallpaper host creation or
// changes to Explorer-owned windows are needed for this same-host drift.
inline Result TryRestore(HWND render, HWND host, HWND desktopView,
    Conditions conditions) noexcept
{
    Result result;
    if (!conditions.customDesktopVisible || !conditions.presentationReady ||
        conditions.interactionActive || GetCapture() != nullptr)
    {
        result.status = Status::Deferred;
        return result;
    }
    if (!render || !host || !desktopView || render == desktopView ||
        !IsWindow(render) || !IsWindow(host) || !IsWindow(desktopView) ||
        GetWindowThreadProcessId(render, nullptr) != GetCurrentThreadId() ||
        GetParent(render) != host || GetParent(desktopView) != host ||
        !IsWindowVisible(render) || !IsWindowVisible(desktopView) ||
        !IsAbove(desktopView, render))
        return result;

    RECT renderRect{}, viewRect{}, overlap{};
    if (!GetWindowRect(render, &renderRect) ||
        !GetWindowRect(desktopView, &viewRect) ||
        !IntersectRect(&overlap, &renderRect, &viewRect))
        return result;

    result.previous = GetWindow(render, GW_HWNDPREV);
    // Insert immediately before DefView, preserving any other sibling above it.
    result.insertAfter = GetWindow(desktopView, GW_HWNDPREV);
    SetLastError(ERROR_SUCCESS);
    const bool moved = SetWindowPos(render,
        result.insertAfter ? result.insertAfter : HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
    const DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    if (moved && GetParent(render) == host &&
        GetParent(desktopView) == host && IsAbove(render, desktopView))
    {
        result.status = Status::Restored;
    }
    else
    {
        result.status = Status::Failed;
        result.error = error ? error : ERROR_RETRY;
    }
    return result;
}
}
