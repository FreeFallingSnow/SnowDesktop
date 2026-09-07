#pragma once
#include "large_icon_config.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop
{
// Shared by the desktop and settings preview; no window, timer or entitlement
// state belongs here. The owner requests frames only while Advance returns true.
struct LargeIconMotion
{
    float hover = 0, from = 0, target = 0;
    double transitionStart = 0, launchStart = 0;

    bool Advance(double now, bool hovered, bool visible, bool enabled,
        double durationScale, const LargeIconConfig& config)
    {
        if (!visible)
        { hover = from = target = 0; transitionStart = now; launchStart = 0; return false; }
        const float next = hovered ? 1.f : 0.f;
        if (!enabled)
        { hover = from = target = next; transitionStart = now; launchStart = 0; return false; }
        if (next != target)
        {
            from = hover; target = next;
            transitionStart = now + (hovered && hover == 0 ? config.delayMs : 0);
        }
        const double duration = (target ? config.enterMs : config.exitMs) * std::max(0., durationScale);
        const double progress = now < transitionStart ? 0 : duration <= 0 ? 1 : std::clamp((now - transitionStart) / duration, 0., 1.);
        hover = static_cast<float>(from + (target - from) * progress * progress * (3 - 2 * progress));
        if (config.launch == 0 || now >= launchStart + 450 * std::max(0., durationScale)) launchStart = 0;
        return hover != target || now < transitionStart || launchStart > 0;
    }

    void Launch(double now, bool enabled, const LargeIconConfig& config)
    { launchStart = enabled && config.launch != 0 ? std::max(.0001, now) : 0; }

    double LaunchWave(double now, double durationScale) const
    {
        const double duration = 450 * std::max(0., durationScale);
        if (launchStart <= 0 || duration <= 0 || now < launchStart || now >= launchStart + duration) return 0;
        return std::sin((now - launchStart) / duration * 3.14159265358979323846);
    }
};
}
