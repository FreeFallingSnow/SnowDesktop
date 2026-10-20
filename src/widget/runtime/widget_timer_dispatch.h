#pragma once

#include "widget_runtime_scheduler.h"
#include "widget/preview/widget_preview_context.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetTimerMutation
{
    enum class Kind { Set, At, Timeline, Cancel };
    Kind kind = Kind::Cancel;
    std::string name;
    int intervalMs = 0;
    bool repeat = false;
    ScheduleHiddenPolicy hiddenPolicy = ScheduleHiddenPolicy::Continue;
    std::int64_t epochMilliseconds = 0;
    std::vector<NamedTimerSchedule::TimelineEntry> entries;
    bool reloadAtEnd = false;
};

// Resolve the current VM once and execute the actual schedule mutation there.
// A retiring VM or a module not yet bound to a generation cannot address the
// replacement simply because both VMs share a persistent widget ID.
template<class Owners, class Reschedule>
bool DispatchWidgetTimerMutation(Owners& owners, std::wstring_view widgetId,
    std::uint64_t sourceToken, WidgetTimerMutation mutation,
    NamedTimerSchedule::TimePoint steadyNow,
    NamedTimerSchedule::WallTimePoint wallNow, Reschedule reschedule)
{
    const auto owner = std::find_if(owners.rbegin(), owners.rend(),
        [widgetId](const auto& candidate) { return candidate.widgetId == widgetId; });
    if (owner == owners.rend() || sourceToken == 0 ||
        owner->runtimeToken != sourceToken || mutation.name.empty())
        return false;
    if (owner->preview)
    {
        steadyNow = NamedTimerSchedule::TimePoint{
            std::chrono::milliseconds(PreviewMonotonicMilliseconds)};
        wallNow = NamedTimerSchedule::WallTimePoint{
            std::chrono::milliseconds(PreviewWallClockMilliseconds)};
    }
    bool changed = false;
    switch (mutation.kind)
    {
    case WidgetTimerMutation::Kind::Set:
        changed = owner->namedTimers.Set(std::move(mutation.name),
            mutation.intervalMs, mutation.repeat, steadyNow, mutation.hiddenPolicy);
        break;
    case WidgetTimerMutation::Kind::At:
        changed = owner->namedTimers.SetAt(std::move(mutation.name),
            mutation.epochMilliseconds, steadyNow, wallNow, mutation.hiddenPolicy);
        break;
    case WidgetTimerMutation::Kind::Timeline:
        changed = owner->namedTimers.SetTimeline(std::move(mutation.name),
            std::move(mutation.entries), steadyNow, wallNow,
            mutation.hiddenPolicy, mutation.reloadAtEnd);
        break;
    case WidgetTimerMutation::Kind::Cancel:
        changed = owner->namedTimers.Cancel(mutation.name);
        break;
    }
    if (changed && !owner->preview) reschedule(*owner);
    return changed;
}
}
