#pragma once

#include "dock_genie_rules.h"

#include <array>

namespace snowdesktop::dock_genie
{
inline constexpr std::size_t TimelineSteps = 96;
// Seven nonconstant matrix elements followed by the shared parent opacity.
using TimelineValues = std::array<double, 8>;
struct TimelineSample
{
    TimelineValues value{}, derivative{};
};
struct TimelineCubic
{
    float constant = 0, linear = 0, quadratic = 0, cubic = 0;
    double Evaluate(double seconds) const noexcept
    {
        return constant + seconds * (linear + seconds * (quadratic + seconds * cubic));
    }
};

inline TimelineValues TimelineValue(const Rect& window, const Rect& dock, Edge edge,
    double from, double to, double progress, double sourceWidth, double sourceHeight,
    double begin, double end, double hostLeft, double hostTop) noexcept
{
    const double collapsed = Mix(from, to, Smooth(progress));
    const auto matrix = StripProjectiveMatrix(window, dock, edge, collapsed,
        sourceWidth, sourceHeight, begin, end, hostLeft, hostTop);
    // A common boundary must have the same homogeneous numerator and denominator
    // in both strips, including BETWEEN timeline samples. Normalizing by the
    // first section width provides this invariant; raw matrix interpolation does not.
    const double scale = 1.0 / SectionAt(window, dock, edge, collapsed, begin).width;
    const bool vertical = Vertical(edge);
    return {matrix.m11 * scale, (vertical ? matrix.m21 : matrix.m12) * scale,
        matrix.m22 * scale, (vertical ? matrix.m24 : matrix.m14) * scale,
        matrix.dx * scale, matrix.dy * scale, matrix.w * scale, Opacity(collapsed)};
}

inline TimelineSample SampleTimeline(const Rect& window, const Rect& dock, Edge edge,
    double from, double to, double progress, double sourceWidth, double sourceHeight,
    double begin, double end, double hostLeft, double hostTop) noexcept
{
    const auto sample = [&](double time) {
        return TimelineValue(window, dock, edge, from, to, time, sourceWidth,
            sourceHeight, begin, end, hostLeft, hostTop);
    };
    TimelineSample result;
    result.value = sample(progress);
    // Smooth clamps at the endpoints, where the exact derivative is zero.
    if (progress > 0.0 && progress < 1.0)
    {
        constexpr double delta = 0.00001;
        const auto before = sample(progress - delta), after = sample(progress + delta);
        for (std::size_t i = 0; i < result.value.size(); ++i)
            result.derivative[i] = (after[i] - before[i]) / (2.0 * delta);
    }
    return result;
}

inline TimelineCubic TimelineSegment(const TimelineSample& from, const TimelineSample& to,
    std::size_t element, double durationSeconds) noexcept
{
    const double seconds = durationSeconds / TimelineSteps;
    const double delta = to.value[element] - from.value[element];
    const double first = from.derivative[element] / durationSeconds;
    const double last = to.derivative[element] / durationSeconds;
    return {static_cast<float>(from.value[element]), static_cast<float>(first),
        static_cast<float>(3.0 * delta / (seconds * seconds) - (2.0 * first + last) / seconds),
        static_cast<float>(-2.0 * delta / (seconds * seconds * seconds) +
            (first + last) / (seconds * seconds))};
}
}
