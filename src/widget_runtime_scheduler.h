#pragma once

#include "widget_interaction_region.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    using WallClock = std::chrono::system_clock;
    using WallTimePoint = WallClock::time_point;

    static constexpr std::size_t MaxTimers = 32;
    static constexpr std::size_t MaxNameBytes = 128;
    static constexpr int MinIntervalMs = 100;
    static constexpr int MaxIntervalMs = 86400000;
    static constexpr std::int64_t MaxAbsoluteDelayMs =
        366LL * 24LL * 60LL * 60LL * 1000LL;
    static constexpr int HiddenThrottleIntervalMs = 5000;
    static constexpr std::size_t MaxTimelineEntries = 64;

    struct TimelineEntry
    {
        std::int64_t epochMilliseconds = 0;
        InteractionValue value;
    };

    bool Set(std::string name, int intervalMs, bool repeat, TimePoint now,
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue);
    bool SetAt(std::string name, std::int64_t epochMilliseconds,
        TimePoint now, WallTimePoint wallNow,
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue);
    bool SetTimeline(std::string name,
        std::vector<TimelineEntry> entries,
        TimePoint now, WallTimePoint wallNow,
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue,
        bool reloadAtEnd = false);
    bool Cancel(std::string_view name);
    bool SetVisible(bool visible, TimePoint now);
    struct Fire
    {
        std::string name;
        std::size_t missed = 0;
        bool coalesced = false;
        bool timeline = false;
        std::size_t timelineIndex = 0;
        std::size_t timelineCount = 0;
        bool timelineEnded = false;
        bool reload = false;
        InteractionValue value;
    };
    std::vector<std::string> DueNames(TimePoint now) const;
    std::vector<std::string> DueNames(
        TimePoint now, WallTimePoint wallNow) const;
    std::optional<Fire> ConsumeDueInfo(
        std::string_view name, TimePoint now);
    std::optional<Fire> ConsumeDueInfo(
        std::string_view name, TimePoint now, WallTimePoint wallNow);
    bool ConsumeDue(std::string_view name, TimePoint now);
    std::optional<std::chrono::milliseconds> NextDelay(
        TimePoint now) const;
    std::optional<std::chrono::milliseconds> NextDelay(
        TimePoint now, WallTimePoint wallNow) const;
    std::size_t Size() const noexcept;

private:
    struct Timer
    {
        int intervalMs = 1000;
        bool repeat = true;
        ScheduleHiddenPolicy hiddenPolicy =
            ScheduleHiddenPolicy::Continue;
        TimePoint due;
        std::int64_t absoluteEpochMilliseconds = -1;
        std::vector<TimelineEntry> timeline;
        std::size_t timelineIndex = 0;
        bool reloadAtEnd = false;
    };

    bool Eligible(const Timer& timer) const noexcept;
    static TimePoint EffectiveDue(const Timer& timer, TimePoint now,
        WallTimePoint wallNow) noexcept;
    std::unordered_map<std::string, Timer> timers_;
    bool visible_ = true;
};

class AnimationFrameRequests
{
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static constexpr std::size_t MaxRequests = 16;
    static constexpr std::size_t MaxNameBytes = 128;
    static constexpr std::int64_t MaximumDeltaMilliseconds = 1000;

    struct Frame
    {
        std::string name;
        std::int64_t deltaMilliseconds = 0;
    };

    bool Request(std::string name, bool reducedMotion = false);
    bool Cancel(std::string_view name);
    bool SetVisible(bool visible, bool preservePending = false);
    std::vector<Frame> Consume(TimePoint now);
    void Clear();
    bool HasPending() const noexcept;
    std::size_t Size() const noexcept;

private:
    std::vector<std::string> pending_;
    std::unordered_map<std::string, TimePoint> previousFrames_;
    bool visible_ = true;
};
}
