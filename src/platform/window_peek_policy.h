#pragma once

#include <windows.h>
#include <dwmapi.h>

namespace snowdesktop
{
inline bool ExcludeDesktopWindowFromPeek(HWND window) noexcept
{
    const BOOL excluded = TRUE;
    return window && SUCCEEDED(DwmSetWindowAttribute(window,
        DWMWA_EXCLUDED_FROM_PEEK, &excluded, sizeof(excluded)));
}

// A short-lived override for committing a minimized window's visible Peek.
// DWMWA_TRANSITIONS_FORCEDISABLED is set-only: both documented DWM queries
// and GetWindowCompositionAttribute reject reads on supported Windows builds.
// Release removes our force-disable override (FALSE), returning to normal DWM
// transition policy; it does not claim to recover an unreadable previous value.
// This never changes SPI_SETANIMATION or another window's global preferences.
class ScopedWindowTransitionSuppression
{
public:
    ScopedWindowTransitionSuppression() = default;
    ~ScopedWindowTransitionSuppression() { Release(); }
    ScopedWindowTransitionSuppression(const ScopedWindowTransitionSuppression&) = delete;
    ScopedWindowTransitionSuppression& operator=(const ScopedWindowTransitionSuppression&) = delete;

    bool Suppress(HWND window) noexcept
    {
        if (window_ == window && IsCurrent()) return true;
        if (!Release() || !window || !IsIconic(window)) return false;
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!process || !thread) return false;
        const BOOL disabled = TRUE;
        if (FAILED(DwmSetWindowAttribute(window, DWMWA_TRANSITIONS_FORCEDISABLED,
                &disabled, sizeof(disabled)))) return false;
        window_ = window;
        process_ = process;
        thread_ = thread;
        return true;
    }

    bool Release() noexcept
    {
        if (!window_) return true;
        if (IsCurrent())
        {
            const BOOL disabled = FALSE;
            if (FAILED(DwmSetWindowAttribute(window_, DWMWA_TRANSITIONS_FORCEDISABLED,
                    &disabled, sizeof(disabled)))) return false;
        }
        window_ = nullptr;
        process_ = thread_ = 0;
        return true;
    }

private:
    bool IsCurrent() const noexcept
    {
        DWORD process = 0;
        const DWORD thread = GetWindowThreadProcessId(window_, &process);
        return process && process == process_ && thread == thread_;
    }

    HWND window_ = nullptr;
    DWORD process_ = 0;
    DWORD thread_ = 0;
};
}
