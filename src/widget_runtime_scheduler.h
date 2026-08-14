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
class NamedTimerSchedule
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::size_t MaxTimers = 32;
    static constexpr int MinIntervalMs = 100;
    static constexpr int MaxIntervalMs = 86400000;

    bool Set(std::string name, int intervalMs, bool repeat, TimePoint now);
    bool Cancel(std::string_view name);
    std::vector<std::string> DueNames(TimePoint now) const;
    bool ConsumeDue(std::string_view name, TimePoint now);
    std::optional<std::chrono::milliseconds> NextDelay(
        TimePoint now) const;
    std::size_t Size() const noexcept;

private:
    struct Timer
    {
        int intervalMs = 1000;
        bool repeat = true;
        TimePoint due;
    };

    std::unordered_map<std::string, Timer> timers_;
};
}
