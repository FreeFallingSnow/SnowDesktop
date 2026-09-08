#pragma once
#include "large_icon_config.h"
#include <algorithm>
#include <cmath>

namespace snowdesktop
{
// Desktop transition state; no window, timer or entitlement
// state belongs here. The owner requests frames only while Advance returns true.
struct LargeIconMotion
{
    float hover = 0, from = 0, target = 0;
    double transitionStart = 0;

    bool Advance(double now, bool hovered, bool visible, bool enabled,
        double durationScale, const LargeIconConfig& config)
    {
        if (!visible || config.effect == 0 || (config.effect == 1 && !enabled))
        { hover = from = target = 0; transitionStart = now; return false; }
        const float next = hovered ? 1.f : 0.f;
        if (!enabled)
        { hover = from = target = next; transitionStart = now; return false; }
        if (next != target)
        {
            from = hover; target = next;
            transitionStart = now + (hovered && hover == 0 ? config.delayMs : 0);
        }
        const double duration = (target ? config.enterMs : config.exitMs) * std::max(0., durationScale);
        const double progress = now < transitionStart ? 0 : duration <= 0 ? 1 : std::clamp((now - transitionStart) / duration, 0., 1.);
        hover = static_cast<float>(from + (target - from) * progress * progress * (3 - 2 * progress));

        return hover != target || now < transitionStart;
    }

};
}
