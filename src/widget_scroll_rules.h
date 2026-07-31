#pragma once

#include <algorithm>

namespace snowdesktop::widget_scroll_rules
{

struct WheelResult
{
    int offset = 0;
    bool moved = false;
};

inline WheelResult ApplyWheelDelta(
    int offset, int maximum, int delta, int step = 48)
{
    constexpr int kWheelDelta = 120;
    maximum = std::max(0, maximum);
    int normalizedStep =
        delta == 0
            ? 0
            : (delta * step) / kWheelDelta;
    if (delta != 0 && normalizedStep == 0)
        normalizedStep = delta > 0 ? 1 : -1;
    const int next =
        std::clamp(offset - normalizedStep, 0, maximum);
    return { next, next != offset };
}

} // namespace snowdesktop::widget_scroll_rules
