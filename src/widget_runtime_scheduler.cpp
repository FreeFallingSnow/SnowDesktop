#include "widget_runtime_scheduler.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace snowdesktop::widget_runtime
{
bool NamedTimerSchedule::Set(
    std::string name,
    int intervalMs,
    bool repeat,
    TimePoint now,
    ScheduleHiddenPolicy hiddenPolicy)
{
    if (name.empty() || name.size() > MaxNameBytes ||
        (!timers_.contains(name) && timers_.size() >= MaxTimers))
    {
        return false;
    }

    intervalMs = std::clamp(
        intervalMs, MinIntervalMs, MaxIntervalMs);
    const int effectiveInterval = !visible_ &&
            hiddenPolicy == ScheduleHiddenPolicy::Throttle
        ? std::max(intervalMs, HiddenThrottleIntervalMs)
        : intervalMs;
    timers_[std::move(name)] = {
        intervalMs,
        repeat,
        hiddenPolicy,
        now + std::chrono::milliseconds(effectiveInterval),
    };
    return true;
}

bool NamedTimerSchedule::Cancel(std::string_view name)
{
    return timers_.erase(std::string(name)) > 0;
}

bool NamedTimerSchedule::SetVisible(bool visible, TimePoint now)
{
    if (visible_ == visible) return false;
    visible_ = visible;
    for (auto& [_, timer] : timers_)
    {
        if (timer.hiddenPolicy != ScheduleHiddenPolicy::Throttle)
            continue;
        const int effectiveInterval = visible_
            ? timer.intervalMs
            : std::max(timer.intervalMs, HiddenThrottleIntervalMs);
        const auto adjustedDue = now +
            std::chrono::milliseconds(effectiveInterval);
        timer.due = visible_
            ? std::min(timer.due, adjustedDue)
            : std::max(timer.due, adjustedDue);
    }
    return true;
}

bool NamedTimerSchedule::Eligible(const Timer& timer) const noexcept
{
    return visible_ ||
        timer.hiddenPolicy != ScheduleHiddenPolicy::Pause;
}

std::vector<std::string> NamedTimerSchedule::DueNames(
    TimePoint now) const
{
    std::vector<std::string> result;
    for (const auto& [name, timer] : timers_)
    {
        if (Eligible(timer) && now >= timer.due)
            result.push_back(name);
    }
    return result;
}

bool NamedTimerSchedule::ConsumeDue(
    std::string_view name,
    TimePoint now)
{
    return ConsumeDueInfo(name, now).has_value();
}

std::optional<NamedTimerSchedule::Fire>
NamedTimerSchedule::ConsumeDueInfo(
    std::string_view name, TimePoint now)
{
    auto timer = timers_.find(std::string(name));
    if (timer == timers_.end() || !Eligible(timer->second) ||
        now < timer->second.due)
        return std::nullopt;

    Fire result;
    result.name = timer->first;

    if (!timer->second.repeat)
    {
        timers_.erase(timer);
        return result;
    }

    const int effectiveInterval = !visible_ &&
            timer->second.hiddenPolicy == ScheduleHiddenPolicy::Throttle
        ? std::max(timer->second.intervalMs, HiddenThrottleIntervalMs)
        : timer->second.intervalMs;
    auto nextDue = timer->second.due +
        std::chrono::milliseconds(effectiveInterval);
    while (nextDue <= now)
    {
        ++result.missed;
        nextDue +=
            std::chrono::milliseconds(effectiveInterval);
    }
    timer->second.due = nextDue;
    result.coalesced = result.missed > 0;
    return result;
}

std::optional<std::chrono::milliseconds>
NamedTimerSchedule::NextDelay(TimePoint now) const
{
    if (timers_.empty())
        return std::nullopt;

    std::optional<TimePoint> nextDue;
    for (const auto& [_, timer] : timers_)
    {
        if (!Eligible(timer)) continue;
        if (!nextDue || timer.due < *nextDue)
            nextDue = timer.due;
    }
    if (!nextDue) return std::nullopt;

    const auto remaining = *nextDue > now
        ? *nextDue - now : Clock::duration::zero();
    std::int64_t delayMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            remaining).count();
    if (remaining > std::chrono::milliseconds(delayMs))
        ++delayMs;
    delayMs = std::clamp<std::int64_t>(
        delayMs, MinIntervalMs, MaxIntervalMs);
    return std::chrono::milliseconds(delayMs);
}

std::size_t NamedTimerSchedule::Size() const noexcept
{
    return timers_.size();
}
}
