#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget_runtime
{
enum class ScheduleHiddenPolicy
{
    Pause,
    Throttle,
    Continue,
};

class NamedTimerSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::size_t MaxTimers = 32;
    static constexpr std::size_t MaxNameBytes = 128;
    static constexpr int MinIntervalMs = 100;
    static constexpr int MaxIntervalMs = 86400000;
    static constexpr int HiddenThrottleIntervalMs = 5000;

    bool Set(std::string name, int intervalMs, bool repeat, TimePoint now,
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue);
    bool Cancel(std::string_view name);
    bool SetVisible(bool visible, TimePoint now);
    struct Fire
    {
        std::string name;
        std::size_t missed = 0;
        bool coalesced = false;
    };
    std::vector<std::string> DueNames(TimePoint now) const;
    std::optional<Fire> ConsumeDueInfo(
        std::string_view name, TimePoint now);
    bool ConsumeDue(std::string_view name, TimePoint now);
    std::optional<std::chrono::milliseconds> NextDelay(
        TimePoint now) const;
    std::size_t Size() const noexcept;

private:
    struct Timer
    {
        int intervalMs = 1000;
        bool repeat = true;
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue;
        TimePoint due;
    };

    bool Eligible(const Timer& timer) const noexcept;
    std::unordered_map<std::string, Timer> timers_;
    bool visible_ = true;
};
}
