#include "widget/runtime/widget_engine.h"
#include "widget/preview/widget_preview_context.h"
#include "common/l10n.h"
#include "widget/view/widget_surface_host.h"
#include "widget/runtime/widget_calendar_provider.h"
#include "widget/api/widget_calendar_query_options.h"

#include <algorithm>
#include <utility>

void WidgetEngine::NotifyCalendarChanged(
    const std::string& reason)
{
    const bool selectionChanged = reason == "selection";
    if (selectionChanged)
        ++calendarSelectionRevision_;
    else
        ++calendarEventsRevision_;
    const std::string topic = selectionChanged
        ? "calendar.selectedDate" : "calendar.events";
    const std::uint64_t revision = selectionChanged
        ? calendarSelectionRevision_ : calendarEventsRevision_;

    std::vector<std::wstring> targets;
    for (const auto& widget : widgets_)
    {
        if (!widget.valid || widget.preview) continue;
        const bool subscribed = std::any_of(
            widget.dataSubscriptions.begin(),
            widget.dataSubscriptions.end(),
            [&topic](const auto& entry) {
                return entry.second == topic;
            });
        if (!subscribed) continue;
        targets.push_back(widget.widgetId);
    }
    for (const auto& widgetId : targets)
    {
        const int index = FindWidget(widgetId);
        if (index < 0) continue;
        LuaWidget& widget = widgets_[index];
        (void)InvokeLifecycleEvent(widget, "data.change",
            [&topic, revision](lua_State* eventState) {
                lua_pushlstring(eventState, topic.data(), topic.size());
                lua_setfield(eventState, -2, "topic");
                lua_pushinteger(eventState,
                    static_cast<lua_Integer>(revision));
                lua_setfield(eventState, -2, "revision");
            });
        RuntimeInvalidateHost(widgetId);
    }
}

void WidgetEngine::SetCalendarDisplayPreferences(snowdesktop::calendar::DisplayPreferences preferences)
{
    snowdesktop::calendar::Normalize(preferences);
    if (calendarDisplay_ == preferences) return;
    calendarDisplay_ = std::move(preferences);
    calendarAnnotationCacheKey_.clear();
    for (auto& widget : widgets_)
        if (widget.state && WidgetDeclaresFeature(widget, "calendar.annotations"))
            InvokeLifecycleEvent(widget, "calendar.preferences", [](lua_State*) {});
    RuntimeInvalidateHost();
}

const std::vector<snowdesktop::calendar::DayAnnotation>& WidgetEngine::RuntimeCalendarAnnotations(const std::string& from, const std::string& to)
{
    const std::string language = Locale::Instance().GetEffectiveLanguage();
    const std::string key = from + ":" + to + ":" + language;
    if (key != calendarAnnotationCacheKey_)
    {
        calendarAnnotationCache_ = snowdesktop::calendar::Annotate(from, to, calendarDisplay_, language);
        calendarAnnotationCacheKey_ = key;
    }
    return calendarAnnotationCache_;
}

std::string WidgetEngine::RuntimeCalendarSelectedDate() const
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return "2026-08-02";
    return calendarService_
        ? calendarService_->SelectedDate()
        : std::string();
}

bool WidgetEngine::RuntimeCalendarSetSelectedDate(
    const std::string& date)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return snowdesktop::calendar::CalendarService::GetDateInfo(date).has_value();
    return calendarService_ &&
        calendarService_->SetSelectedDate(date);
}

std::optional<snowdesktop::calendar::DateInfo>
WidgetEngine::RuntimeCalendarDateInfo(
    const std::string& date) const
{
    return snowdesktop::calendar::CalendarService::
        GetDateInfo(date);
}

std::optional<std::string>
WidgetEngine::RuntimeCalendarAddDays(
    const std::string& date, int offset) const
{
    return snowdesktop::calendar::CalendarService::
        AddDays(date, offset);
}

std::vector<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarEvents(
    const std::string& fromDate,
    const std::string& toDate, bool includeSubscriptions) const
{
    if (snowdesktop::widget_runtime::IsDryLoad())
    {
        const std::vector<snowdesktop::calendar::CalendarEvent> samples = {
            { "preview-1", 1,
                _L("app.widget_preview.api.calendar_review"), "2026-08-02",
                false, 600, 660,
                _L("app.widget_preview.api.calendar_review_notes"), 15, {} },
            { "preview-2", 1,
                _L("app.widget_preview.api.calendar_publish"), "2026-08-03",
                true, 0, 0, {}, -1, {} },
        };
        std::vector<snowdesktop::calendar::CalendarEvent> result;
        for (const auto& event : samples)
            if (event.date >= fromDate && event.date <= toDate)
                result.push_back(event);
        return result;
    }
    return calendarService_
        ? calendarService_->Events(fromDate, toDate, includeSubscriptions)
        : std::vector<
            snowdesktop::calendar::CalendarEvent>{};
}

std::optional<std::vector<snowdesktop::calendar::SubscriptionInfo>> WidgetEngine::RuntimeCalendarSubscriptions() const
{
    return calendarService_ && calendarServiceLoaded_ ? std::optional{calendarService_->Subscriptions()} : std::nullopt;
}

std::optional<snowdesktop::calendar::SubscriptionDetails> WidgetEngine::RuntimeCalendarSubscriptionDetails(
    const std::string& id) const
{
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->SubscriptionDetailsById(id) : std::nullopt;
}

snowdesktop::calendar::SubscriptionResult WidgetEngine::RuntimeCalendarSubscription(
    snowdesktop::calendar::SubscriptionCommand command)
{
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->ApplySubscription(std::move(command))
        : snowdesktop::calendar::SubscriptionResult::Unavailable;
}

std::vector<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarSingleEvents() const
{
    return calendarService_ ? calendarService_->SingleEvents() :
        std::vector<snowdesktop::calendar::CalendarEvent>{};
}

std::optional<snowdesktop::calendar::CalendarEvent>
WidgetEngine::RuntimeCalendarEventById(const std::string& id) const
{
    return calendarService_ ? calendarService_->EventById(id) : std::nullopt;
}

std::optional<snowdesktop::calendar::CalendarSeries>
WidgetEngine::RuntimeCalendarSeriesById(const std::string& id) const
{
    return calendarService_ ? calendarService_->SeriesById(id) : std::nullopt;
}

std::vector<snowdesktop::calendar::CalendarSeries>
WidgetEngine::RuntimeCalendarSeries() const
{
    return calendarService_ ? calendarService_->Series() :
        std::vector<snowdesktop::calendar::CalendarSeries>{};
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesCreate(
    snowdesktop::calendar::CalendarSeries series)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, {}, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->CreateSeries(std::move(series)) :
        snowdesktop::calendar::MutationResult{ false, {}, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesUpdate(
    const std::string& id, int expectedRevision,
    snowdesktop::calendar::CalendarSeries series)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->UpdateSeries(id, expectedRevision, std::move(series)) :
        snowdesktop::calendar::MutationResult{ false, id, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult WidgetEngine::RuntimeCalendarSeriesRemove(
    const std::string& id, int expectedRevision)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0, "previewReadOnly" };
    return calendarService_ && calendarServiceLoaded_ ? calendarService_->RemoveSeries(id, expectedRevision) :
        snowdesktop::calendar::MutationResult{ false, id, 0, "unavailable" };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarCreate(
    snowdesktop::calendar::CalendarEvent event)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, {}, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_ && calendarServiceLoaded_
        ? calendarService_->Create(std::move(event))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarUpdate(
    const std::string& id,
    int expectedRevision,
    snowdesktop::calendar::CalendarEvent event)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_ && calendarServiceLoaded_
        ? calendarService_->Update(
            id, expectedRevision, std::move(event))
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

snowdesktop::calendar::MutationResult
WidgetEngine::RuntimeCalendarRemove(
    const std::string& id, int expectedRevision)
{
    if (snowdesktop::widget_runtime::IsDryLoad())
        return { false, id, 0,
            _L("app.widget_preview.api.read_only") };
    return calendarService_ && calendarServiceLoaded_
        ? calendarService_->Remove(id, expectedRevision)
        : snowdesktop::calendar::MutationResult{
            false, id, 0, "unavailable"
        };
}

void WidgetEngine::TickWidgetCalendar()
{
    if (calendarService_ && calendarServiceLoaded_)
        calendarService_->Tick();
    const bool eventsChanged =
        std::exchange(
            pendingCalendarEventsChange_, false);
    const bool selectionChanged =
        std::exchange(
            pendingCalendarSelectionChange_, false);
    if (eventsChanged)
        NotifyCalendarChanged("events");
    if (selectionChanged)
    {
        NotifyCalendarChanged("selection");
    }
}
