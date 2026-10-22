#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace snowdesktop::popup_motion_curve
{
struct Sample
{
    double position = 0.0;
    double velocity = 0.0; // units per second
};

inline Sample Evaluate(Sample initial, double target, double frequency, double seconds)
{
    if (seconds <= 0.0) return initial;
    const double offset = initial.position - target;
    const double linear = initial.velocity + frequency * offset;
    const double decay = std::exp(-frequency * std::max(0.0, seconds));
    return {target + (offset + linear * seconds) * decay,
        (linear - frequency * (offset + linear * seconds)) * decay};
}

inline bool AtRest(Sample sample, double target)
{
    return std::abs(sample.position - target) <= 0.0001 &&
        std::abs(sample.velocity) <= 0.01;
}

// At a finite track's soft endpoint the remaining distance can be tiny while
// velocity is still nonzero. A new critical spring must be fast enough to
// absorb that target-directed momentum without crossing its target.
inline double TargetBoundedFrequency(Sample initial, double target, double frequency)
{
    const double distance = target - initial.position;
    if (distance * initial.velocity > 0.0)
        frequency = std::max(frequency, initial.velocity / distance);
    return frequency;
}

struct CubicSegment
{
    double startSeconds = 0.0, durationSeconds = 0.0;
    double constant = 0.0, linear = 0.0, quadratic = 0.0, cubic = 0.0;
    Sample EvaluateLocal(double seconds) const
    {
        return {constant + seconds * (linear + seconds * (quadratic + seconds * cubic)),
            linear + seconds * (2.0 * quadratic + seconds * 3.0 * cubic)};
    }
    // X control points are 1/3 and 2/3, so Bezier time is exactly linear.
    double StartControl() const
    {
        const double delta = EvaluateLocal(durationSeconds).position - constant;
        return std::abs(delta) < 1e-12 ? 0.0 : linear * durationSeconds / (3.0 * delta);
    }
    double EndControl() const
    {
        const auto end = EvaluateLocal(durationSeconds);
        const double delta = end.position - constant;
        return std::abs(delta) < 1e-12 ? 1.0 : 1.0 - end.velocity * durationSeconds / (3.0 * delta);
    }
};

struct Timeline
{
    std::vector<CubicSegment> segments;
    double durationMilliseconds = 0.0;
    double target = 0.0;
};

inline CubicSegment Interpolate(double begin, double end, Sample first, Sample last)
{
    const double span = end - begin, delta = last.position - first.position;
    return {begin, span, first.position, first.velocity,
        3.0 * delta / (span * span) - (2.0 * first.velocity + last.velocity) / span,
        -2.0 * delta / (span * span * span) + (first.velocity + last.velocity) / (span * span)};
}

inline Timeline Build(Sample initial, double target, double frequency, double milliseconds)
{
    Timeline result;
    result.durationMilliseconds = milliseconds;
    result.target = target;
    if (milliseconds <= 0.0) return result;
    const double endSeconds = milliseconds / 1000.0;
    const double tailStart = endSeconds * 0.75;
    const auto tail = Interpolate(tailStart, endSeconds,
        Evaluate(initial, target, frequency, tailStart), {target, 0.0});
    const auto sample = [&](double seconds) {
        if (seconds >= endSeconds) return Sample{target, 0.0};
        if (seconds >= tailStart) return tail.EvaluateLocal(seconds - tailStart);
        return Evaluate(initial, target, frequency, seconds);
    };
    const auto append = [&](auto&& self, double begin, double end, unsigned depth) -> void {
        const auto first = sample(begin), last = sample(end);
        const double span = end - begin;
        const auto segment = Interpolate(begin, end, first, last);
        double error = 0.0;
        for (const double fraction : {0.25, 0.5, 0.75})
            error = std::max(error, std::abs(segment.EvaluateLocal(span * fraction).position - sample(begin + span * fraction).position));
        const double delta = last.position - first.position;
        // A small absolute fitting error can still cross a nearby endpoint.
        // Preserve velocity by subdividing monotone segments whose Bezier
        // controls would leave their endpoint range, rather than clamping them.
        const bool unboundedControls = std::abs(delta) > 1e-12 &&
            first.velocity * delta >= 0.0 && last.velocity * delta >= 0.0 &&
            (first.velocity * span / delta > 3.0 || last.velocity * span / delta > 3.0);
        if ((error > 0.00005 || unboundedControls) && depth < 18)
        {
            const double middle = (begin + end) * 0.5;
            self(self, begin, middle, depth + 1);
            self(self, middle, end, depth + 1);
        }
        else result.segments.push_back(segment);
    };
    result.segments.reserve(24);
    const double linear = initial.velocity + frequency * (initial.position - target);
    const double turn = std::abs(linear) > 1e-12 ? initial.velocity / (frequency * linear) : 0.0;
    // A zero-net-displacement keyframe cannot represent a turn. Both native
    // backends therefore share an explicit boundary at the velocity's zero.
    std::vector<double> boundaries{0.0, tailStart, endSeconds};
    if (turn > 0.000001 && turn < tailStart - 0.000001)
        boundaries.push_back(turn);
    if (std::abs(tail.cubic) > 1e-12)
    {
        const double discriminant = 4.0 * tail.quadratic * tail.quadratic - 12.0 * tail.cubic * tail.linear;
        if (discriminant > 0)
            for (const double sign : {-1.0, 1.0})
            {
                const double root = (-2.0 * tail.quadratic + sign * std::sqrt(discriminant)) / (6.0 * tail.cubic);
                if (root > 0.000001 && root < tail.durationSeconds - 0.000001)
                    boundaries.push_back(tailStart + root);
            }
    }
    std::sort(boundaries.begin(), boundaries.end());
    for (std::size_t index = 1; index < boundaries.size(); ++index)
        append(append, boundaries[index - 1], boundaries[index], 0);
    return result;
}

inline Sample Evaluate(const Timeline& timeline, double seconds)
{
    for (const auto& segment : timeline.segments)
        if (seconds < segment.startSeconds + segment.durationSeconds)
            return segment.EvaluateLocal(std::max(0.0, seconds - segment.startSeconds));
    return {timeline.target, 0.0};
}

inline Timeline Remainder(const Timeline& timeline, double elapsedMilliseconds)
{
    Timeline result;
    result.target = timeline.target;
    result.durationMilliseconds = std::max(0.0, timeline.durationMilliseconds - elapsedMilliseconds);
    const double elapsed = elapsedMilliseconds / 1000.0;
    for (const auto& segment : timeline.segments)
    {
        const double end = segment.startSeconds + segment.durationSeconds;
        if (end <= elapsed) continue;
        const double begin = std::max(elapsed, segment.startSeconds);
        const double local = begin - segment.startSeconds;
        const auto first = segment.EvaluateLocal(local);
        result.segments.push_back({begin - elapsed, end - begin, first.position, first.velocity,
            segment.quadratic + 3.0 * segment.cubic * local, segment.cubic});
    }
    return result;
}
}
