#pragma once

#include "dock_attraction_rules.h"
#include "dock_genie_timeline.h"

namespace snowdesktop::dock_attraction
{
inline constexpr std::size_t TimelineSteps = 128;
// Nine projective coefficients, local gradient opacity, parent opacity.
using Values = std::array<double, 11>;
struct Frame
{
    Patch patch;
    double collapsed;
};
inline Frame TimelineFrame(Rect window, Rect dock, double from, double to, double progress) noexcept
{
    const double collapsed = dock_genie::Mix(from, to, dock_genie::Smooth(progress));
    return {ControlPoints(window, dock, collapsed), collapsed};
}
inline Values SampleCell(const Frame &frame, Rect window, Rect dock, Cell cell, double width, double height,
                         double cropX, double cropY, double hostX, double hostY) noexcept
{
    const auto matrix = Projection(frame.patch, cell, width, height, cropX, cropY, hostX, hostY);
    const Matrix m = matrix.value_or(Matrix{});
    return {m.m11,
            m.m12,
            m.m14,
            m.m21,
            m.m22,
            m.m24,
            m.dx,
            m.dy,
            m.w,
            matrix ? CellOpacity(window, dock, frame.collapsed, cell) : 0.0,
            Opacity(frame.collapsed)};
}
inline dock_genie::TimelineCubic TimelineSegment(const Values &first, const Values &last, std::size_t element,
                                                 double duration) noexcept
{
    // Linear interpolation of all homogeneous coordinates preserves positive
    // denominators throughout a segment, even near the patch's local folds.
    // The spatial and timing Beziers are sampled before compositor playback.
    return {static_cast<float>(first[element]),
            static_cast<float>((last[element] - first[element]) * TimelineSteps / duration), 0, 0};
}
inline void StabilizeInactiveGeometry(std::array<Values, TimelineSteps + 1> &samples) noexcept
{
    // A folded cell is invisible. Borrow the nearest finite matrix so its
    // opacity transition cannot briefly reveal an identity-projected tile.
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        if (samples[i][9] > 0)
            continue;
        for (std::size_t distance = 1; distance < samples.size(); ++distance)
        {
            const std::size_t before = i >= distance ? i - distance : i;
            const std::size_t after = std::min(samples.size() - 1, i + distance);
            const std::size_t donor = samples[before][9] > 0 ? before : after;
            if (samples[donor][9] <= 0)
                continue;
            std::copy_n(samples[donor].begin(), 9, samples[i].begin());
            break;
        }
    }
}
} // namespace snowdesktop::dock_attraction
