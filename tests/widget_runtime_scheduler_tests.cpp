#include "widget_runtime_scheduler.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Schedule = snowdesktop::widget_runtime::NamedTimerSchedule;
using HiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy;
using FrameRequests =
    snowdesktop::widget_runtime::AnimationFrameRequests;

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

void TestVisibilityPolicies()
{
    const Schedule::TimePoint start{};
    Schedule schedule;
    Check(schedule.SetVisible(false, start),
        "the schedule must enter its hidden state");
    Check(schedule.Set("paused", 100, true, start,
              HiddenPolicy::Pause) &&
            schedule.Set("throttled", 100, true, start,
                HiddenPolicy::Throttle) &&
            schedule.Set("continuous", 100, true, start,
                HiddenPolicy::Continue),
        "all hidden policies must be accepted");
    Check(schedule.NextDelay(start) == std::chrono::milliseconds(100),
        "a hidden continue timer must keep its requested interval");
    const auto hiddenDue = schedule.DueNames(
        start + std::chrono::milliseconds(150));
    Check(hiddenDue.size() == 1 && hiddenDue[0] == "continuous",
        "hidden pause and throttle timers must not fire at foreground rate");
    Check(schedule.ConsumeDue("continuous",
              start + std::chrono::milliseconds(150)) &&
            schedule.Cancel("continuous"),
        "the hidden continue timer must remain consumable");
    Check(schedule.NextDelay(start + std::chrono::milliseconds(150)) ==
            std::chrono::milliseconds(4850),
        "a hidden throttle timer must use the shared five-second floor");

    const auto resume = start + std::chrono::milliseconds(1000);
    Check(schedule.SetVisible(true, resume),
        "the schedule must resume its visible state");
    const auto resumedDue = schedule.DueNames(resume);
    Check(resumedDue.size() == 1 && resumedDue[0] == "paused",
        "an overdue paused timer must coalesce when visibility resumes");
    const auto paused = schedule.ConsumeDueInfo("paused", resume);
    Check(paused && paused->missed == 9 && paused->coalesced,
        "resumed pause timers must report missed deadlines");
    Check(schedule.NextDelay(resume) == std::chrono::milliseconds(100),
        "resuming must restore the foreground throttle interval");

    Schedule repeatedThrottle;
    (void)repeatedThrottle.SetVisible(false, start);
    Check(repeatedThrottle.Set("repeat", 100, true, start,
              HiddenPolicy::Throttle) &&
            repeatedThrottle.ConsumeDue("repeat",
                start + std::chrono::milliseconds(5000)) &&
            repeatedThrottle.NextDelay(
                start + std::chrono::milliseconds(5000)) ==
                std::chrono::milliseconds(5000),
        "hidden throttle must preserve its floor after every firing");
}

void TestAbsoluteDeadlines()
{
    const Schedule::TimePoint steadyStart{};
    const Schedule::WallTimePoint wallStart{
        std::chrono::milliseconds(1'000'000) };
    Schedule schedule;
    Check(schedule.SetAt("absolute", 1'000'500,
              steadyStart, wallStart, HiddenPolicy::Continue),
        "an absolute deadline within the bounded horizon must be accepted");
    Check(schedule.NextDelay(steadyStart, wallStart) ==
            std::chrono::milliseconds(500),
        "an absolute deadline must project onto the steady host clock");
    Check(schedule.DueNames(
            steadyStart + std::chrono::milliseconds(499),
            wallStart + std::chrono::milliseconds(499)).empty(),
        "an absolute deadline must not fire early");
    const auto due = schedule.DueNames(
        steadyStart + std::chrono::milliseconds(500),
        wallStart + std::chrono::milliseconds(500));
    Check(due.size() == 1 && due[0] == "absolute" &&
            schedule.ConsumeDueInfo("absolute",
                steadyStart + std::chrono::milliseconds(500),
                wallStart + std::chrono::milliseconds(500)).has_value() &&
            schedule.Size() == 0,
        "an absolute deadline must fire once and retire itself");

    Check(schedule.SetAt("past", 900'000,
              steadyStart, wallStart) &&
            schedule.NextDelay(steadyStart, wallStart) ==
                std::chrono::milliseconds(Schedule::MinIntervalMs),
        "a past absolute deadline must coalesce into the next host wakeup");
    Check(!schedule.SetAt("too-far",
              1'000'000 + Schedule::MaxAbsoluteDelayMs + 1,
              steadyStart, wallStart),
        "absolute deadlines beyond 366 days must be rejected");

    Schedule paused;
    Check(paused.SetVisible(false, steadyStart) &&
            paused.SetAt("paused-at", 1'000'100,
                steadyStart, wallStart, HiddenPolicy::Pause) &&
            paused.DueNames(
                steadyStart + std::chrono::milliseconds(200),
                wallStart + std::chrono::milliseconds(200)).empty() &&
            paused.SetVisible(true,
                steadyStart + std::chrono::milliseconds(200)) &&
            paused.DueNames(
                steadyStart + std::chrono::milliseconds(200),
                wallStart + std::chrono::milliseconds(200)).size() == 1,
        "a hidden paused absolute deadline must coalesce on resume");
}

void TestTimelineCoalescingAndReload()
{
    const Schedule::TimePoint steadyStart{};
    const Schedule::WallTimePoint wallStart{
        std::chrono::milliseconds(2'000'000) };
    Schedule schedule;
    snowdesktop::widget_runtime::InteractionValue firstValue;
    firstValue.type = snowdesktop::widget_runtime::InteractionValue::Type::String;
    firstValue.string = "first";
    snowdesktop::widget_runtime::InteractionValue secondValue = firstValue;
    secondValue.string = "second";
    snowdesktop::widget_runtime::InteractionValue finalValue = firstValue;
    finalValue.string = "final";
    std::vector<Schedule::TimelineEntry> entries = {
        { 2'000'100, firstValue },
        { 2'000'200, secondValue },
        { 2'000'500, finalValue },
    };
    Check(schedule.SetTimeline("agenda", std::move(entries),
            steadyStart, wallStart, HiddenPolicy::Continue, true) &&
            schedule.NextDelay(steadyStart, wallStart) ==
                std::chrono::milliseconds(100),
        "a bounded increasing timeline must schedule its first entry");

    const auto coalesced = schedule.ConsumeDueInfo("agenda",
        steadyStart + std::chrono::milliseconds(250),
        wallStart + std::chrono::milliseconds(250));
    Check(coalesced && coalesced->timeline &&
            coalesced->timelineIndex == 2 &&
            coalesced->timelineCount == 3 &&
            coalesced->missed == 1 && coalesced->coalesced &&
            coalesced->value.string == "second" &&
            !coalesced->timelineEnded && !coalesced->reload,
        "elapsed timeline entries must coalesce to the newest due value");
    Check(schedule.NextDelay(
            steadyStart + std::chrono::milliseconds(250),
            wallStart + std::chrono::milliseconds(250)) ==
                std::chrono::milliseconds(250),
        "a timeline must advance to its next absolute entry");

    const auto final = schedule.ConsumeDueInfo("agenda",
        steadyStart + std::chrono::milliseconds(500),
        wallStart + std::chrono::milliseconds(500));
    Check(final && final->timeline && final->timelineIndex == 3 &&
            final->value.string == "final" && final->timelineEnded &&
            final->reload && schedule.Size() == 0,
        "the final timeline entry must retire the schedule and expose reload-at-end");
}

void TestTimelineValidationAndHiddenPause()
{
    const Schedule::TimePoint steadyStart{};
    const Schedule::WallTimePoint wallStart{
        std::chrono::milliseconds(3'000'000) };
    Schedule schedule;
    Check(!schedule.SetTimeline("empty", {}, steadyStart, wallStart),
        "empty timelines must be rejected");
    Check(!schedule.SetTimeline("unordered",
            { { 3'000'200, {} }, { 3'000'100, {} } },
            steadyStart, wallStart),
        "timeline deadlines must be strictly increasing");
    Check(!schedule.SetTimeline("too-far",
            { { 3'000'000 + Schedule::MaxAbsoluteDelayMs + 1, {} } },
            steadyStart, wallStart),
        "timeline entries beyond the absolute horizon must be rejected");

    Check(schedule.SetVisible(false, steadyStart) &&
            schedule.SetTimeline("paused",
                { { 3'000'100, {} }, { 3'000'200, {} } },
                steadyStart, wallStart, HiddenPolicy::Pause) &&
            schedule.DueNames(
                steadyStart + std::chrono::milliseconds(250),
                wallStart + std::chrono::milliseconds(250)).empty(),
        "a hidden paused timeline must not wake the host");
    Check(schedule.SetVisible(true,
                steadyStart + std::chrono::milliseconds(250)),
        "a paused timeline must resume with its owner");
    const auto resumed = schedule.ConsumeDueInfo("paused",
        steadyStart + std::chrono::milliseconds(250),
        wallStart + std::chrono::milliseconds(250));
    Check(resumed && resumed->timeline && resumed->timelineIndex == 2 &&
            resumed->missed == 1 && resumed->timelineEnded,
        "resuming a paused timeline must coalesce elapsed entries once");
}

void TestAnimationFrameRequests()
{
    FrameRequests requests;
    const FrameRequests::TimePoint start{};
    Check(!requests.Request("") &&
            !requests.Request(std::string(
                FrameRequests::MaxNameBytes + 1, 'x')) &&
            !requests.Request("reduced", true),
        "frame requests must reject invalid IDs and reduced-motion work");
    for (std::size_t index = 0; index < FrameRequests::MaxRequests; ++index)
    {
        Check(requests.Request("frame-" + std::to_string(index)),
            "frame requests below the per-instance limit must be accepted");
    }
    Check(requests.Request("frame-0") &&
            requests.Size() == FrameRequests::MaxRequests &&
            !requests.Request("overflow"),
        "duplicate frames must coalesce without bypassing the limit");

    const auto first = requests.Consume(
        start + std::chrono::milliseconds(16));
    Check(first.size() == FrameRequests::MaxRequests &&
            first.front().name == "frame-0" &&
            first.front().deltaMilliseconds == 0 &&
            !requests.HasPending(),
        "the first host frame must consume every pending ID once");
    Check(requests.Request("frame-0"),
        "an event may explicitly request its next frame");
    const auto next = requests.Consume(
        start + std::chrono::milliseconds(33));
    Check(next.size() == 1 && next[0].name == "frame-0" &&
            next[0].deltaMilliseconds == 17,
        "continued frame loops must receive a monotonic delta");
    Check(requests.Request("frame-0"),
        "a continued frame loop may request a later host frame");
    const auto delayed = requests.Consume(
        start + std::chrono::milliseconds(5000));
    Check(delayed.size() == 1 &&
            delayed[0].deltaMilliseconds ==
                FrameRequests::MaximumDeltaMilliseconds,
        "long frame gaps must clamp animation catch-up work");

    Check(requests.Request("frame-0") && requests.SetVisible(false) &&
            !requests.HasPending() && !requests.Request("hidden"),
        "hiding a component must clear and reject frame requests");
    Check(requests.SetVisible(true) && requests.Request("frame-0"),
        "a visible component may start a fresh frame loop");
    const auto resumed = requests.Consume(
        start + std::chrono::milliseconds(5000));
    Check(resumed.size() == 1 &&
            resumed[0].deltaMilliseconds == 0,
        "resuming must not report a catch-up animation delta");
    Check(requests.Request("frame-0") && requests.Cancel("frame-0") &&
            !requests.HasPending() && !requests.Cancel("frame-0"),
        "canceling a named next frame must be idempotent");
}
}

int main()
{
    TestLimitsAndReplacement();
    TestDueConsumption();
    TestDelayClampingAndRounding();
    TestVisibilityPolicies();
    TestAbsoluteDeadlines();
    TestTimelineCoalescingAndReload();
    TestTimelineValidationAndHiddenPause();
    TestAnimationFrameRequests();
    std::cout << "widget runtime scheduler tests passed\n";
    return 0;
}
