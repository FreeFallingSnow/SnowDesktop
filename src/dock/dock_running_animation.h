#pragma once

#include <cstddef>

namespace snowdesktop::dock_running_animation
{
// Presence controls both the icon and its share of the Dock's axis length.
// Reversing an unfinished entrance/exit starts at the current presentation.
class Presence
{
public:
    static constexpr double kDurationMilliseconds = 220.0;

    void SetVisible(bool visible, double now, bool animate, double durationScale = 1.0);

    void Advance(double now);

    float Amount() const;
    bool Visible() const;
    bool IsAnimating() const;
    bool IsHidden() const;
    bool Interactive() const;

private:
    float amount_ = 0.0f, from_ = 0.0f, to_ = 0.0f;
    double started_ = 0.0, duration_ = kDurationMilliseconds;
    bool visible_ = false, animating_ = false;
};

int AxisLength(float units, int pitch);

float GroupAmount(float units);

int ScrollableExtent(std::size_t fixed, std::size_t frequent, std::size_t folders,
    float runningUnits, int pitch, int separatorGap);
} // namespace snowdesktop::dock_running_animation
