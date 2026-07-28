#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_launch_animation
{

// Request frames faster than a 60 Hz refresh cycle. The animation is driven by
// the performance counter, so coalesced/skipped frames advance to the correct
// position instead of slowing the bounce down.
constexpr UINT kFrameIntervalMs = 8;
constexpr ULONGLONG kBouncePeriodMs = 340;
constexpr ULONGLONG kMinimumDurationMs = kBouncePeriodMs * 2;
constexpr ULONGLONG kMaximumDurationMs = kBouncePeriodMs * 5;
constexpr double kCycleDamping = 0.68;

inline bool SystemAnimationsEnabled() noexcept
{
    ANIMATIONINFO animationInfo{ sizeof(animationInfo) };
    if (SystemParametersInfoW(
            SPI_GETANIMATION, sizeof(animationInfo),
            &animationInfo, 0) &&
        animationInfo.iMinAnimate == 0)
        return false;

    BOOL clientAreaAnimation = TRUE;
    if (SystemParametersInfoW(
            SPI_GETCLIENTAREAANIMATION, 0,
            &clientAreaAnimation, 0) &&
        !clientAreaAnimation)
        return false;
    return true;
}

inline double MonotonicTimeMilliseconds() noexcept
{
    LARGE_INTEGER counter{};
    static const double ticksPerMillisecond = [] {
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) ||
            frequency.QuadPart <= 0)
            return 0.0;
        return static_cast<double>(
            frequency.QuadPart) / 1000.0;
    }();
    if (!QueryPerformanceCounter(&counter) ||
        ticksPerMillisecond <= 0.0)
    {
        return static_cast<double>(GetTickCount64());
    }
    return static_cast<double>(
        counter.QuadPart) / ticksPerMillisecond;
}

inline double NormalizedOffset(double elapsedMs) noexcept
{
    if (elapsedMs < 0.0 ||
        elapsedMs >= static_cast<double>(kMaximumDurationMs))
        return 0.0;

    const double period =
        static_cast<double>(kBouncePeriodMs);
    const double cycle = std::floor(elapsedMs / period);
    const double phase =
        (elapsedMs - cycle * period) / period;
    constexpr double pi = 3.14159265358979323846;
    const double arc = std::sin(pi * phase);
    // Squaring the arc gives every take-off and landing zero velocity. This
    // removes the visible direction snap at cycle boundaries while retaining
    // the familiar damped bounce silhouette.
    return arc * arc *
        std::pow(kCycleDamping, cycle);
}

inline float OffsetPixels(
    double elapsedMs, int iconSize) noexcept
{
    const double amplitude =
        std::max(6.0, static_cast<double>(iconSize) * 0.38);
    return static_cast<float>(std::max(
        0.0, amplitude * NormalizedOffset(elapsedMs)));
}

inline bool IsRestingPoint(double elapsedMs) noexcept
{
    if (elapsedMs <
        static_cast<double>(kMinimumDurationMs))
        return false;
    return NormalizedOffset(elapsedMs) <= 0.035;
}

} // namespace snowdesktop::dock_launch_animation
