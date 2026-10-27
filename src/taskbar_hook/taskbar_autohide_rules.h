#pragma once

#include <windows.h>
#include <cstdint>
#include <unordered_map>

namespace snowdesktop::taskbar_hook
{
inline bool IsRecentTaskbarInput(DWORD recorded, DWORD lastInput, DWORD now) noexcept
{
    // A pointer merely resting over the taskbar is not an input-origin signal.
    // A later click/key/movement elsewhere invalidates the recorded operation.
    return recorded != 0 && recorded == lastInput && now - recorded <= 500;
}

class TaskbarMinimizeExemptions
{
public:
    bool Exempts(std::uintptr_t source, bool observed, bool taskbarInput, ULONGLONG now)
    {
        std::erase_if(sources_, [now](const auto& entry) { return now - entry.second > 1500; });
        if (!source) return false;
        if (taskbarInput)
        {
            sources_[source] = now;
            return true;
        }
        // An asynchronous WinEvent belongs to the operation already excluded
        // by the synchronous pre-minimize hook, even if the pointer moved.
        if (observed && sources_.contains(source)) return true;
        // A new application/Dock command with another input must be protected.
        sources_.erase(source);
        return false;
    }
private:
    std::unordered_map<std::uintptr_t, ULONGLONG> sources_;
};

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

// Suppress the opening activation, while preserving a later explicit reveal
// for automatic hiding. Once released, renewal cannot hide the bar again in
// this invocation; a new begin command rearms the next Dock click.
struct StartMenuProtection
{
    bool seen = false, released = false, suppress = false, pointerKnown = false;
    ULONGLONG started = 0;
    POINT previous{};

    void Reset() noexcept { *this = {}; }
    void Reveal(ULONGLONG now) noexcept
    {
        // The Win key used to open Start generates a native reveal message.
        // Ignore that opening activation, not later taskbar keyboard access.
        if (seen && now >= started && now - started >= 250) released = true;
    }
    bool Update(bool lease, bool permanent, bool geometry, bool edge, POINT cursor, ULONGLONG now) noexcept
    {
        if (!lease) { Reset(); return false; }
        if (!seen)
        {
            seen = true; started = now;
            previous = cursor; pointerKnown = geometry;
        }
        const bool moved = geometry && pointerKnown &&
            (cursor.x != previous.x || cursor.y != previous.y);
        if (!permanent && edge && moved) released = true;
        previous = cursor; pointerKnown = geometry;
        suppress = permanent || !released;
        return suppress;
    }
};

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
