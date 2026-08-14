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
    TimePoint now)
{
    if (name.empty() || name.size() > MaxNameBytes ||
        (!timers_.contains(name) && timers_.size() >= MaxTimers))
    {
        return false;
    }

    intervalMs = std::clamp(
        intervalMs, MinIntervalMs, MaxIntervalMs);
    timers_[std::move(name)] = {
        intervalMs,
        repeat,
        now + std::chrono::milliseconds(intervalMs),
    };
    return true;
}

bool NamedTimerSchedule::Cancel(std::string_view name)
{
    return timers_.erase(std::string(name)) > 0;
}

std::vector<std::string> NamedTimerSchedule::DueNames(
    TimePoint now) const
{
    std::vector<std::string> result;
    for (const auto& [name, timer] : timers_)
    {
        if (now >= timer.due)
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
    if (timer == timers_.end() || now < timer->second.due)
        return std::nullopt;

    Fire result;
    result.name = timer->first;

    if (!timer->second.repeat)
    {
        timers_.erase(timer);
        return result;
    }

    auto nextDue = timer->second.due +
        std::chrono::milliseconds(timer->second.intervalMs);
    while (nextDue <= now)
    {
        ++result.missed;
        nextDue +=
            std::chrono::milliseconds(timer->second.intervalMs);
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

    auto nextDue = timers_.begin()->second.due;
    for (const auto& [_, timer] : timers_)
        nextDue = std::min(nextDue, timer.due);

    const auto remaining = nextDue > now
        ? nextDue - now : Clock::duration::zero();
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
