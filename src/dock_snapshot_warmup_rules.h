#pragma once

#include "dock_window_rules.h"

#include <cstdint>
#include <windows.h>

namespace snowdesktop::dock_snapshot_warmup_rules
{
constexpr bool ShouldStart(bool enabled, bool active, bool eligible, bool pending,
    std::uint64_t now, std::uint64_t lastAttempt, std::uint32_t foregroundAge) noexcept
{
    return enabled && !active && eligible && !pending && foregroundAge >= 250 &&
        (lastAttempt == 0 || (now >= lastAttempt && now - lastAttempt >= 2000));
}

constexpr bool ShouldAccept(std::uint64_t capturedTick, std::uint64_t now,
    bool hasExisting, std::uint64_t existingTick) noexcept
{
    return now >= capturedTick && now - capturedTick <= 3000 &&
        (!hasExisting || capturedTick > existingTick);
}

constexpr bool CanEvict(bool minimized, bool background) noexcept
{
    return !background || !minimized;
}

constexpr bool HasSameRestorePlacement(const WINDOWPLACEMENT& captured,
    const WINDOWPLACEMENT& current) noexcept
{
    return captured.length == sizeof(WINDOWPLACEMENT) &&
        current.length == sizeof(WINDOWPLACEMENT) &&
        captured.rcNormalPosition.left == current.rcNormalPosition.left &&
        captured.rcNormalPosition.top == current.rcNormalPosition.top &&
        captured.rcNormalPosition.right == current.rcNormalPosition.right &&
        captured.rcNormalPosition.bottom == current.rcNormalPosition.bottom &&
        dock_window_rules::ShouldRestoreDockWindowMaximized(captured.flags, captured.showCmd) ==
            dock_window_rules::ShouldRestoreDockWindowMaximized(current.flags, current.showCmd);
}
}
