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

inline double SettlingMilliseconds(Sample initial, double target, double frequency)
{
    if (AtRest(initial, target)) return 0.0;
    double upper = 1.0 / frequency;
    double lower = 0.0;
    // Test after the velocity extremum too: a reversal may briefly pass the
    // target's rest tolerance before moving away from it.
    const double linear = initial.velocity + frequency * (initial.position - target);
    if (std::abs(linear) > 1e-12)
    {
        lower = std::max(0.0, initial.velocity / (frequency * linear) + 1.0 / frequency);
        upper = std::max(upper, lower);
    }
    for (unsigned count = 0; count < 24 && !AtRest(Evaluate(initial, target, frequency, upper), target); ++count)
        upper *= 2.0;
    if (AtRest(Evaluate(initial, target, frequency, lower), target)) lower = 0.0;
    for (unsigned count = 0; count < 32; ++count)
    {
        const double middle = (lower + upper) * 0.5;
        if (AtRest(Evaluate(initial, target, frequency, middle), target)) upper = middle;
        else lower = middle;
    }
    return std::max(1.0, std::ceil(upper * 1000.0));
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

inline Timeline Build(Sample initial, double target, double frequency, double milliseconds)
{
    Timeline result;
    result.durationMilliseconds = milliseconds;
    result.target = target;
    if (milliseconds <= 0.0) return result;
    const double endSeconds = milliseconds / 1000.0;
    const auto sample = [&](double seconds) {
        return seconds >= endSeconds ? Sample{target, 0.0} : Evaluate(initial, target, frequency, seconds);
    };
    const auto append = [&](auto&& self, double begin, double end, unsigned depth) -> void {
        const auto first = sample(begin), last = sample(end);
        const double span = end - begin, delta = last.position - first.position;
        CubicSegment segment{begin, span, first.position, first.velocity,
            3.0 * delta / (span * span) - (2.0 * first.velocity + last.velocity) / span,
            -2.0 * delta / (span * span * span) + (first.velocity + last.velocity) / (span * span)};
        double error = 0.0;
        for (const double fraction : {0.25, 0.5, 0.75})
            error = std::max(error, std::abs(segment.EvaluateLocal(span * fraction).position - sample(begin + span * fraction).position));
        if (error > 0.00005 && depth < 10)
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
    if (turn > 0.000001 && turn < endSeconds - 0.000001)
    {
        append(append, 0.0, turn, 0);
        append(append, turn, endSeconds, 0);
    }
    else append(append, 0.0, endSeconds, 0);
    return result;
}
}
