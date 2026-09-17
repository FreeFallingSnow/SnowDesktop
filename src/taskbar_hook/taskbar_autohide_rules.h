#pragma once

#include <windows.h>
#include <cstdint>
#include <utility>

namespace snowdesktop::taskbar_hook
{
struct ActivationRevealContext
{
    bool protectedTaskbar = false;
    bool geometryValid = false;
    bool explicitFocus = false;
    bool secondary = false;
    LONG activation = WA_INACTIVE;
    std::uint32_t callerRva = 0;
    RECT taskbar{}, monitor{};
    POINT cursor{};
};

inline bool ShouldSuppressActivationReveal(const ActivationRevealContext& context,
    int flags, int request) noexcept
{
    // RVAs belong ONLY to the independently verified Taskbar.dll adapter.
    // Keyboard focus can use the exact same activation request and call site.
    if (!context.protectedTaskbar || !context.geometryValid || context.explicitFocus ||
        context.activation != WA_ACTIVE || flags != 0 || request != 8 ||
        context.callerRva != (context.secondary ? 0x22550u : 0x92af3u))
        return false;
    const auto& bar = context.taskbar;
    const auto& screen = context.monitor;
    if (screen.right <= screen.left || screen.bottom <= screen.top ||
        bar.right <= bar.left || bar.bottom <= bar.top ||
        bar.top < screen.bottom - 2 || bar.top > screen.bottom || bar.bottom <= screen.bottom)
        return false; // Includes visible, transitioning, and non-bottom taskbars.
    const bool intentionalEdge = context.cursor.x >= screen.left && context.cursor.x < screen.right &&
        context.cursor.y >= screen.bottom - 2 && context.cursor.y < screen.bottom;
    return !intentionalEdge;
}

// Shared production dispatch boundary: forwarding preserves arguments exactly.
template<typename Original>
bool DispatchActivationReveal(const ActivationRevealContext& context,
    int flags, int request, Original&& original)
{
    if (ShouldSuppressActivationReveal(context, flags, request)) return true;
    std::forward<Original>(original)(flags, request);
    return false;
}
}
