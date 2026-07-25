#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace snowdesktop::dock_launch_animation
{

constexpr UINT kFrameIntervalMs = 16;
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

inline double NormalizedOffset(ULONGLONG elapsedMs) noexcept
{
    if (elapsedMs >= kMaximumDurationMs)
        return 0.0;

    const ULONGLONG cycle = elapsedMs / kBouncePeriodMs;
    const double phase = static_cast<double>(
        elapsedMs % kBouncePeriodMs) /
        static_cast<double>(kBouncePeriodMs);
    constexpr double pi = 3.14159265358979323846;
    return std::sin(pi * phase) *
        std::pow(kCycleDamping, static_cast<double>(cycle));
}

inline int OffsetPixels(
    ULONGLONG elapsedMs, int iconSize) noexcept
{
    const double amplitude =
        std::max(6.0, static_cast<double>(iconSize) * 0.38);
    return std::max(0, static_cast<int>(std::lround(
        amplitude * NormalizedOffset(elapsedMs))));
}

inline bool IsRestingPoint(ULONGLONG elapsedMs) noexcept
{
    if (elapsedMs < kMinimumDurationMs)
        return false;
    return NormalizedOffset(elapsedMs) <= 0.035;
}

} // namespace snowdesktop::dock_launch_animation
