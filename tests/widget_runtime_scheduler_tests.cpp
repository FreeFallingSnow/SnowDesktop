#include "widget_runtime_scheduler.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
using Schedule = snowdesktop::widget_runtime::NamedTimerSchedule;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void TestLimitsAndReplacement()
{
    Schedule schedule;
    const Schedule::TimePoint now{};
    Check(!schedule.Set("", 1000, true, now),
        "empty timer names must be rejected");
    Check(!schedule.Set(std::string(Schedule::MaxNameBytes + 1, 'x'),
            1000, true, now),
        "oversized timer names must be rejected");
    for (std::size_t index = 0; index < Schedule::MaxTimers; ++index)
    {
        Check(schedule.Set(
                "timer-" + std::to_string(index), 1000, true, now),
            "timers below the per-instance limit must be accepted");
    }
    Check(!schedule.Set("overflow", 1000, true, now),
        "new timers above the per-instance limit must be rejected");
    Check(schedule.Set("timer-0", 250, false, now),
        "an existing timer must remain replaceable at the limit");
    Check(schedule.Size() == Schedule::MaxTimers,
        "replacing a timer must not change the schedule size");
}

void TestDueConsumption()
{
    Schedule schedule;
    const Schedule::TimePoint start{};
    Check(schedule.Set("once", 200, false, start) &&
            schedule.Set("repeat", 100, true, start),
        "test timers must be accepted");
    Check(schedule.DueNames(start).empty(),
        "new timers must not be due immediately");

    const auto due = schedule.DueNames(
        start + std::chrono::milliseconds(250));
    Check(due.size() == 2,
        "all elapsed timers must appear in the due snapshot");
    const auto once = schedule.ConsumeDueInfo(
        "once", start + std::chrono::milliseconds(250));
    Check(once && once->missed == 0 && !once->coalesced,
        "one-shot timer must be consumable");
    const auto repeat = schedule.ConsumeDueInfo(
        "repeat", start + std::chrono::milliseconds(250));
    Check(repeat && repeat->missed == 1 && repeat->coalesced,
        "repeating timer must report coalesced elapsed deadlines");
    Check(schedule.Size() == 1 &&
            !schedule.Cancel("once") && schedule.Cancel("repeat"),
        "one-shot timers must be removed and repeating timers retained");
}

void TestDelayClampingAndRounding()
{
    const Schedule::TimePoint start{};
    Schedule minimum;
    Check(minimum.Set("minimum", 1, false, start),
        "minimum timer must be accepted");
    Check(minimum.NextDelay(start) ==
            std::chrono::milliseconds(Schedule::MinIntervalMs),
        "timer intervals must clamp to the minimum");

    Schedule maximum;
    Check(maximum.Set("maximum", Schedule::MaxIntervalMs + 1,
            false, start),
        "maximum timer must be accepted");
    Check(maximum.NextDelay(start) ==
            std::chrono::milliseconds(Schedule::MaxIntervalMs),
        "timer intervals must clamp to the maximum");

    Schedule rounded;
    Check(rounded.Set("rounded", 101, false, start),
        "rounding timer must be accepted");
    const auto fractionalNow = start +
        std::chrono::microseconds(500);
    Check(rounded.NextDelay(fractionalNow) ==
            std::chrono::milliseconds(101),
        "remaining delay must round up to avoid early wakeup");
    Check(rounded.NextDelay(
            start + std::chrono::milliseconds(500)) ==
            std::chrono::milliseconds(Schedule::MinIntervalMs),
        "overdue timers must use the minimum host delay");
}
}

int main()
{
    TestLimitsAndReplacement();
    TestDueConsumption();
    TestDelayClampingAndRounding();
    std::cout << "widget runtime scheduler tests passed\n";
    return 0;
}
