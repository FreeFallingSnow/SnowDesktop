#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace snowdesktop::ui_animation_scheduler_rules
{

inline constexpr double kMinimumRefreshHz = 30.0;
inline constexpr double kMaximumRefreshHz = 240.0;
inline constexpr double kFallbackRefreshHz = 60.0;
inline constexpr double kRecoveryWindowMilliseconds = 2000.0;

constexpr double ClampTargetRefresh(
    double refreshHz,
    bool softwareOrRemote) noexcept
{
    if (refreshHz < kMinimumRefreshHz)
        refreshHz = kFallbackRefreshHz;
    if (softwareOrRemote)
        refreshHz = std::min(
            refreshHz, kFallbackRefreshHz);
    return std::clamp(
        refreshHz, kMinimumRefreshHz,
        kMaximumRefreshHz);
}

constexpr double AdvanceRepeatingDeadline(
    double deadlineMilliseconds,
    double intervalMilliseconds,
    double nowMilliseconds) noexcept
{
    intervalMilliseconds = std::max(1.0, intervalMilliseconds);
    while (deadlineMilliseconds <= nowMilliseconds)
        deadlineMilliseconds += intervalMilliseconds;
    return deadlineMilliseconds;
}

constexpr double NextFrameDeadline(
    double nowMilliseconds,
    double lastPresentationMilliseconds,
    double intervalMilliseconds) noexcept
{
    intervalMilliseconds = std::max(1.0, intervalMilliseconds);
    if (lastPresentationMilliseconds <= 0.0 ||
        nowMilliseconds >=
            lastPresentationMilliseconds + intervalMilliseconds)
        return nowMilliseconds;
    return lastPresentationMilliseconds + intervalMilliseconds;
}

inline std::uint64_t MissedFrameCount(
    double scheduledMilliseconds,
    double nowMilliseconds,
    double intervalMilliseconds) noexcept
{
    if (scheduledMilliseconds <= 0.0 ||
        nowMilliseconds <= scheduledMilliseconds)
        return 0;
    return static_cast<std::uint64_t>(std::floor(
        (nowMilliseconds - scheduledMilliseconds) /
        std::max(1.0, intervalMilliseconds)));
}

constexpr unsigned ReduceAdaptiveDivisor(
    double targetRefreshHz,
    unsigned currentDivisor) noexcept
{
    currentDivisor = std::max(1U, currentDivisor);
    const double currentRefresh =
        targetRefreshHz /
        static_cast<double>(currentDivisor);
    return currentRefresh > kMinimumRefreshHz + 0.5
        ? currentDivisor * 2
        : currentDivisor;
}

constexpr bool RecoveryWindowSatisfied(
    double elapsedMilliseconds,
    double frameWorkMilliseconds,
    double frameBudgetMilliseconds) noexcept
{
    return elapsedMilliseconds >= kRecoveryWindowMilliseconds &&
        frameWorkMilliseconds < frameBudgetMilliseconds * 0.70;
}

} // namespace snowdesktop::ui_animation_scheduler_rules
