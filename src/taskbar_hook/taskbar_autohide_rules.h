#pragma once

#include <windows.h>

namespace snowdesktop::taskbar_hook
{
inline bool IsAutoHiddenTaskbar(const RECT& bar, const RECT& screen) noexcept
{
    if (bar.right <= bar.left || bar.bottom <= bar.top ||
        screen.right <= screen.left || screen.bottom <= screen.top) return false;
    if (bar.right - bar.left >= bar.bottom - bar.top)
        return bar.top >= screen.bottom - 2 || bar.bottom <= screen.top + 2;
    return bar.left >= screen.right - 2 || bar.right <= screen.left + 2;
}

inline bool IsTaskbarRevealEdge(const RECT& bar, const RECT& screen, POINT cursor) noexcept
{
    if (cursor.x < screen.left || cursor.x >= screen.right ||
        cursor.y < screen.top || cursor.y >= screen.bottom) return false;
    if (bar.right - bar.left >= bar.bottom - bar.top)
        return bar.top + (bar.bottom - bar.top) / 2 >= screen.top + (screen.bottom - screen.top) / 2
            ? cursor.y >= screen.bottom - 2 : cursor.y < screen.top + 2;
    return bar.left + (bar.right - bar.left) / 2 >= screen.left + (screen.right - screen.left) / 2
        ? cursor.x >= screen.right - 2 : cursor.x < screen.left + 2;
}

struct MinimizeProtectionInput
{
    LONG sequence = 0;
    ULONGLONG started = 0, now = 0;
    bool enabled = false, cancelled = false, explicitReveal = false;
    bool sourceMinimized = false, nativeHidden = false;
};

// One request covers every taskbar. Each window releases its own reversible
// cloak only after Shell has hidden it, or the user explicitly asks to see it.
// Merely expiring a timer while Shell is expanded would cause a delayed flash.
struct MinimizeProtection
{
    static constexpr ULONGLONG kMinimumHoldMs = 400, kMinimizeTimeoutMs = 1500;
    LONG sequence = 0;
    bool active = false, observedMinimized = false;

    bool Update(const MinimizeProtectionInput& input) noexcept
    {
        const ULONGLONG elapsed = input.now >= input.started ? input.now - input.started : 0;
        if (sequence != input.sequence)
        {
            sequence = input.sequence;
            active = input.sequence != 0 && input.started <= input.now && elapsed < kMinimizeTimeoutMs;
            observedMinimized = false;
        }
        if (!input.enabled || input.cancelled || input.explicitReveal)
            active = false;
        if (!active) return false;
        if (input.sourceMinimized) observedMinimized = true;
        else if (observedMinimized || elapsed >= kMinimizeTimeoutMs) active = false;
        if (observedMinimized && input.nativeHidden && elapsed >= kMinimumHoldMs) active = false;
        return active;
    }
};
}
