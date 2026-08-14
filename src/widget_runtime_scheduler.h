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
    static constexpr std::size_t MaxNameBytes = 128;
    static constexpr int MinIntervalMs = 100;
    static constexpr int MaxIntervalMs = 86400000;

    bool Set(std::string name, int intervalMs, bool repeat, TimePoint now);
    bool Cancel(std::string_view name);
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
        TimePoint due;
    };

    std::unordered_map<std::string, Timer> timers_;
};
}
