#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace snowdesktop::dock_running_animation
{
// Presence controls both the icon and its share of the Dock's axis length.
// Reversing an unfinished entrance/exit starts at the current presentation.
class Presence
{
public:
    static constexpr double kDurationMilliseconds = 220.0;

    void SetVisible(bool visible, double now, bool animate, double durationScale = 1.0)
    {
        Advance(now);
        const float target = visible ? 1.0f : 0.0f;
        visible_ = visible;
        if (!animate)
        {
            amount_ = from_ = to_ = target;
            animating_ = false;
            return;
        }
        if (to_ == target) return;
        from_ = amount_;
        to_ = target;
        started_ = now;
        duration_ = kDurationMilliseconds * std::abs(to_ - from_) *
            (std::isfinite(durationScale) ? std::max(0.01, durationScale) : 1.0);
        animating_ = from_ != to_;
    }

    void Advance(double now)
    {
        if (!animating_) return;
        const float progress = static_cast<float>(
            std::clamp((now - started_) / duration_, 0.0, 1.0));
        // Entrance has immediate visible velocity; a slow ease-in looks like
        // an extra discovery delay before the first few pixels appear.
        const float remaining = 1.0f - progress;
        const float eased = to_ > from_
            ? 1.0f - remaining * remaining * remaining
            : progress * progress * (3.0f - 2.0f * progress);
        amount_ = from_ + (to_ - from_) * eased;
        if (progress >= 1.0f)
        {
            amount_ = to_;
            animating_ = false;
        }
    }

    float Amount() const { return amount_; }
    bool Visible() const { return visible_; }
    bool IsAnimating() const { return animating_; }
    bool IsHidden() const { return !visible_ && !animating_; }
    bool Interactive() const { return visible_ && amount_ >= 0.15f; }

private:
    float amount_ = 0.0f, from_ = 0.0f, to_ = 0.0f;
    double started_ = 0.0, duration_ = kDurationMilliseconds;
    bool visible_ = false, animating_ = false;
};

inline int AxisLength(float units, int pitch)
{
    return static_cast<int>(std::lround(std::max(0.0f, units) * pitch));
}

inline float GroupAmount(float units)
{
    return std::clamp(units, 0.0f, 1.0f);
}

inline int ScrollableExtent(std::size_t fixed, std::size_t frequent, std::size_t folders,
    float runningUnits, int pitch, int separatorGap)
{
    const float groups = static_cast<float>(fixed > 0) +
        static_cast<float>(frequent > 0) + static_cast<float>(folders > 0) +
        GroupAmount(runningUnits);
    return AxisLength(static_cast<float>(fixed + frequent + folders) + runningUnits, pitch) +
        AxisLength(std::max(0.0f, groups - 1.0f), separatorGap);
}
} // namespace snowdesktop::dock_running_animation
