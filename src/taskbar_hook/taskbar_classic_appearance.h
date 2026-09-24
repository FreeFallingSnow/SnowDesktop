#pragma once

#include "taskbar_hook_protocol.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop::taskbar_hook::native
{
struct AccentPolicy
{
    int state = 0;
    DWORD flags = 0, color = 0, animation = 0;
};

inline AccentPolicy MakeClassicAccentPolicy(const TargetAppearance& style,
    bool allowAcrylic = true)
{
    AccentPolicy policy;
    policy.state = (style.style & kStyleGlassBackdrop) ?
        ((allowAcrylic && (style.style & kStyleAcrylicBackdrop)) ? 4 : 3) : 2;
    policy.flags = policy.state == 4 ? 0 : 2;
    // Classic taskbars accept straight-alpha ABGR in ACCENT_POLICY. Keep the
    // solid tint in the native material, as TranslucentTB does on Win10. The
    // optional composition surface only supplies gradients and borders.
    if (!DecodeGradient(style.gradient).enabled)
    {
        const auto channel = [](float value) -> DWORD {
            return std::isfinite(value) ?
                static_cast<DWORD>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f) : 0;
        };
        policy.color = (channel(style.alpha) << 24) | (channel(style.blue) << 16) |
            (channel(style.green) << 8) | channel(style.red);
    }
    // Acrylic requires a nonzero alpha even for an otherwise clear material.
    if (policy.state == 4 && !(policy.color & 0xff000000)) policy.color |= 0x01000000;
    return policy;
}
}
