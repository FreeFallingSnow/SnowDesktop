#pragma once

#include "dock_attraction_rules.h"
#include "dock_genie_timeline.h"

namespace snowdesktop::dock_attraction
{
inline constexpr std::size_t TimelineSteps = 32;
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
            matrix ? CellOpacity(window, dock, frame.collapsed, cell) * CellVisibility(frame.patch, cell) : 0.0,
            Opacity(frame.collapsed)};
}
inline dock_genie::TimelineCubic TimelineSegment(const Values &first, const Values &last, const Values &firstTangent,
                                                 const Values &lastTangent, std::size_t element,
                                                 double duration) noexcept
{
    const double dt = duration / TimelineSteps, delta = last[element] - first[element];
    const double a = firstTangent[element], b = lastTangent[element];
    return {static_cast<float>(first[element]), static_cast<float>(a / dt),
            static_cast<float>((3 * delta - 2 * a - b) / (dt * dt)),
            static_cast<float>((-2 * delta + a + b) / (dt * dt * dt))};
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
inline std::array<Values, TimelineSteps + 1> CellTangents(const std::array<Values, TimelineSteps + 1> &samples,
                                                          double left, double top, double right, double bottom) noexcept
{
    std::array<Values, TimelineSteps + 1> result{};
    for (std::size_t i = 1; i < TimelineSteps; ++i)
    {
        for (std::size_t e = 0; e < 9; ++e)
            result[i][e] = (samples[i + 1][e] - samples[i - 1][e]) * 0.5;
        // Both neighbouring Bezier handles must remain in front of the
        // perspective plane across the complete cropped clip, including AA.
        double scale = 1;
        for (const double x : {left, right})
            for (const double y : {top, bottom})
            {
                const double w = samples[i][2] * x + samples[i][5] * y + samples[i][8];
                const double d = result[i][2] * x + result[i][5] * y + result[i][8];
                if (std::abs(d) > 1e-12)
                    scale = std::min(scale, std::max(0.0, 3 * (w - 0.00001) / std::abs(d)));
                // Positive perspective alone does not bound a projected fragment.
                // Keep both Bezier handles inside the adjacent corner trajectories
                // so interpolation cannot throw a sliver past its sampled motion.
                for (const int direction : {-1, 1})
                {
                    const auto &neighbour = samples[direction < 0 ? i - 1 : i + 1];
                    const double nw = neighbour[2] * x + neighbour[5] * y + neighbour[8];
                    for (const int axis : {0, 1})
                    {
                        const double n = samples[i][axis] * x + samples[i][axis + 3] * y + samples[i][axis + 6];
                        const double dn = result[i][axis] * x + result[i][axis + 3] * y + result[i][axis + 6];
                        const double next = (neighbour[axis] * x + neighbour[axis + 3] * y + neighbour[axis + 6]) / nw;
                        const double low = std::min(n / w, next) - 0.5, high = std::max(n / w, next) + 0.5;
                        const auto limit = [&](double clearance, double slope) {
                            if (slope < 0)
                                scale = std::min(scale, std::max(0.0, clearance / -slope));
                        };
                        limit(n - low * w, direction * (dn - low * d) / 3);
                        limit(high * w - n, direction * (high * d - dn) / 3);
                    }
                }
            }
        for (std::size_t e = 0; e < 9; ++e)
            result[i][e] *= scale;
        // Monotone alpha tangents prevent flashes or negative opacity near
        // hidden folded cells and the transparent endpoint.
        for (std::size_t e = 9; e < 11; ++e)
        {
            const double before = samples[i][e] - samples[i - 1][e], after = samples[i + 1][e] - samples[i][e];
            if (before * after > 0)
                result[i][e] = 2 * before * after / (before + after);
        }
    }
    return result;
}
} // namespace snowdesktop::dock_attraction
