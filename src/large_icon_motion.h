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
    float tiltX = 0, tiltY = 0, tiltFromX = 0, tiltFromY = 0, tiltTargetX = 0, tiltTargetY = 0;
    double tiltStart = 0, tiltDuration = 0;

    bool AdvanceTilt(double now, float x, float y, bool hovered, bool visible,
        bool enabled, double durationScale, const LargeIconConfig& config)
    {
        if (!visible || !enabled || config.effect != 1 || config.amplitude <= 0)
        { tiltX = tiltY = tiltFromX = tiltFromY = tiltTargetX = tiltTargetY = 0; tiltStart = now; return false; }
        const double elapsed = tiltDuration > 0 ? std::clamp((now - tiltStart) / tiltDuration, 0., 1.) : 1;
        const float t = static_cast<float>(elapsed * elapsed * (3 - 2 * elapsed));
        tiltX = tiltFromX + (tiltTargetX - tiltFromX) * t;
        tiltY = tiltFromY + (tiltTargetY - tiltFromY) * t;
        x = hovered && std::isfinite(x) ? std::clamp(x, -1.f, 1.f) : 0;
        y = hovered && std::isfinite(y) ? std::clamp(y, -1.f, 1.f) : 0;
        if (x != tiltTargetX || y != tiltTargetY)
        {
            tiltFromX = tiltX; tiltFromY = tiltY; tiltTargetX = x; tiltTargetY = y;
            tiltStart = now;
            tiltDuration = (hovered ? config.enterMs : config.exitMs) * std::max(0., durationScale);
            if (tiltDuration <= 0) { tiltX = x; tiltY = y; }
        }
        return tiltX != tiltTargetX || tiltY != tiltTargetY;
    }

    bool Advance(double now, bool hovered, bool visible, bool enabled,
        double durationScale, const LargeIconConfig& config)
    {
        if (!visible || config.effect == 0 || (config.effect == 1 && !enabled))
        { hover = from = target = 0; transitionStart = now; return false; }
        const float next = hovered ? 1.f : 0.f;
        if (!enabled)
        { hover = from = target = next; transitionStart = now; return false; }
        const double speed = config.effect == 2 ? .6 : 1.;
        // Sample at this event's time before reversing; pointer events need
        // not coincide with the last animation frame.
        const double oldDuration = (target ? config.enterMs : config.exitMs) * std::max(0., durationScale) * speed;
        const double oldProgress = now < transitionStart ? 0 : oldDuration <= 0 ? 1 : std::clamp((now - transitionStart) / oldDuration, 0., 1.);
        hover = static_cast<float>(from + (target - from) * oldProgress * oldProgress * (3 - 2 * oldProgress));
        if (next != target)
        {
            from = hover; target = next;
            transitionStart = now + (hovered && hover == 0 ? config.delayMs * speed : 0);
        }
        const double duration = (target ? config.enterMs : config.exitMs) * std::max(0., durationScale) * speed;
        const double progress = now < transitionStart ? 0 : duration <= 0 ? 1 : std::clamp((now - transitionStart) / duration, 0., 1.);
        hover = static_cast<float>(from + (target - from) * progress * progress * (3 - 2 * progress));

        return hover != target || now < transitionStart;
    }

};
}
