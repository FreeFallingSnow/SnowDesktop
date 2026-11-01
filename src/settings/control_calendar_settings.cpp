#include "control_calendar_settings.h"

#include <algorithm>

namespace snowdesktop::control_settings
{
namespace
{
using namespace control;
using calendar::CalendarEvent;
using calendar::CalendarSeries;
using calendar::CalendarSeriesRule;

Value Names(std::initializer_list<std::string_view> names)
{
    auto output = Array();
    for (const auto name : names) output.array.push_back(String(std::string(name)));
    return output;
}
Value Event(const CalendarEvent& event)
{
    return Object({{"id", String(event.id)}, {"hostRevision", Number(event.revision)},
        {"title", String(event.title)}, {"date", String(event.date)}, {"allDay", Boolean(event.allDay)},
        {"startMinutes", Number(event.startMinutes)}, {"endMinutes", Number(event.endMinutes)},
        {"notes", String(event.notes)}, {"reminderMinutes", Number(event.reminderMinutes)},
        {"seriesId", String(event.seriesId)}, {"occurrenceDate", String(event.occurrenceDate)},
        {"occurrenceOverride", Boolean(event.occurrenceOverride)}});
}
Value Rule(const CalendarSeriesRule& rule)
{
    auto dates = Array(), weekdays = Array();
    for (const auto& date : rule.dates) dates.array.push_back(String(date));
    for (const int weekday : rule.weekdays) weekdays.array.push_back(Number(weekday));
    return Object({{"kind", String(rule.kind)}, {"dates", std::move(dates)},
        {"startDate", String(rule.startDate)}, {"endDate", String(rule.endDate)},
        {"interval", Number(rule.interval)}, {"weekdays", std::move(weekdays)}, {"monthDay", Number(rule.monthDay)}});
}
Value Series(const CalendarSeries& series)
{
    auto exceptions = Array();
    for (const auto& [date, exception] : series.exceptions)
        exceptions.array.push_back(Object({{"date", String(date)}, {"canceled", Boolean(exception.canceled)}}));
    return Object({{"id", String(series.id)}, {"hostRevision", Number(series.revision)},
        {"event", Event(series.event)}, {"rule", Rule(series.rule)}, {"exceptions", std::move(exceptions)}});
}
Value EventFields()
{
    auto reminders = Array();
    for (const int minutes : {-1, 0, 5, 15, 30, 60, 1440}) reminders.array.push_back(Number(minutes));
    return Object({{"title", Object({{"type", String("string")}, {"maximumBytes", Number(512)}, {"required", Boolean(true)}})},
        {"date", Object({{"type", String("date")}, {"format", String("YYYY-MM-DD")}})},
        {"allDay", Object({{"type", String("boolean")}})},
        {"startMinutes", Object({{"type", String("integer")}, {"minimum", Number(0)}, {"maximum", Number(1439)}})},
        {"endMinutes", Object({{"type", String("integer")}, {"minimum", Number(0)}, {"maximum", Number(1439)}})},
        {"notes", Object({{"type", String("string")}, {"maximumBytes", Number(8192)}})},
        {"reminderMinutes", Object({{"type", String("integer")}, {"enum", std::move(reminders)}})}});
}

void TextPatch(const Value& values, const char* key, std::string& output, std::size_t maximum)
{
    const auto* field = values.Find(key);
    if (!field) return;
    if (!field->IsString()) throw Error("invalid_params", std::string(key) + " must be a string", -32602);
    auto value = field->string;
    if (value.size() > maximum || value.find('\0') != std::string::npos)
        throw Error("invalid_params", std::string(key) + " is too long or contains NUL", -32602);
    if (!value.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0) <= 0)
        throw Error("invalid_params", std::string(key) + " must be UTF-8", -32602);
    output = std::move(value);
}
void EventPatch(const Value& values, CalendarEvent& event)
{
    Fields(values, {"title", "date", "allDay", "startMinutes", "endMinutes", "notes", "reminderMinutes"});
    TextPatch(values, "title", event.title, 512);
    TextPatch(values, "date", event.date, 10);
    TextPatch(values, "notes", event.notes, 8192);
    if (const auto* allDay = values.Find("allDay"))
    {
        if (!allDay->IsBoolean()) throw Error("invalid_params", "allDay must be boolean", -32602);
        event.allDay = allDay->boolean;
    }
    if (values.Find("startMinutes")) event.startMinutes = Integer(values, "startMinutes", 0, 1439);
    if (values.Find("endMinutes")) event.endMinutes = Integer(values, "endMinutes", 0, 1439);
    if (values.Find("reminderMinutes")) event.reminderMinutes = Integer(values, "reminderMinutes", -1, 1440);
}
void RulePatch(const Value& values, CalendarSeriesRule& rule)
{
    Fields(values, {"kind", "dates", "startDate", "endDate", "interval", "weekdays", "monthDay"});
    TextPatch(values, "kind", rule.kind, 16);
    TextPatch(values, "startDate", rule.startDate, 10);
    TextPatch(values, "endDate", rule.endDate, 10);
    if (values.Find("interval")) rule.interval = Integer(values, "interval", 1, 99);
    if (values.Find("monthDay")) rule.monthDay = Integer(values, "monthDay", 0, 31);
    if (const auto* dates = values.Find("dates"))
    {
        if (!dates->IsArray() || dates->array.size() > 366) throw Error("invalid_params", "dates must contain at most 366 dates", -32602);
        rule.dates.clear();
        for (const auto& date : dates->array)
        {
            if (!date.IsString() || date.string.size() != 10 || date.string.find('\0') != std::string::npos)
                throw Error("invalid_params", "dates must contain ISO dates", -32602);
            rule.dates.push_back(date.string);
        }
    }
    if (const auto* days = values.Find("weekdays"))
    {
        if (!days->IsArray() || days->array.size() > 7) throw Error("invalid_params", "weekdays must contain at most seven days", -32602);
        rule.weekdays.clear();
        for (const auto& day : days->array)
        {
            auto wrapped = Object({{"day", day}});
            rule.weekdays.push_back(Integer(wrapped, "day", 1, 7));
        }
    }
}
void SeriesPatch(const Value& values, CalendarSeries& series)
{
    Fields(values, {"event", "rule"});
    if (const auto* event = values.Find("event")) EventPatch(*event, series.event);
    if (const auto* rule = values.Find("rule")) RulePatch(*rule, series.rule);
}
const Value& InputObject(const Value& params, const char* key)
{
    const auto* values = params.Find(key);
    if (!values || !values->IsObject() || values->object.empty())
        throw Error("invalid_params", std::string(key) + " must be a nonempty named object", -32602);
    return *values;
}
void Confirmation(const Value& params)
{
    const auto* value = params.Find("confirmed");
    if (!value || !value->IsBoolean() || !value->boolean)
        throw Error("confirmation_required", "Calendar removal requires arguments.confirmed:true and a current revision");
}
Value Result(const Target& target, const calendar::MutationResult& result)
{
    if (!result.ok) throw Error(result.error == "conflict" ? "stale_settings" :
        result.error == "not_found" ? "settings_target_not_found" : "settings_failed",
        result.error.empty() ? "Calendar operation failed" : result.error);
    return Object({{"target", TargetJson(target)}, {"status", String("applied")},
        {"id", String(result.id)}, {"hostRevision", Number(result.revision)}});
}
}

bool IsCalendarSettingsTarget(const Target& target) noexcept
{
    return target.kind == "calendar" || target.kind == "calendar-event" || target.kind == "calendar-series";
}

Value HandleCalendarSettings(std::string_view method, const Value& params,
    const Target& target, const CalendarSettingsContext& context)
{
    const auto snapshot = context.controller.Snapshot();
    if (!snapshot || !context.calendar.events || !context.calendar.series)
        throw Error("settings_unavailable", "Calendar service is unavailable");
    const auto events = context.calendar.events(snapshot->generation);
    const auto series = context.calendar.series(snapshot->generation);
    if (!events || !series) throw Error("settings_unavailable", "Calendar service is not available in the current host context");
    auto state = Object({{"generation", String(std::to_string(snapshot->generation))}, {"events", Array()}, {"series", Array()}});
    for (const auto& event : *events) state.object.at("events").array.push_back(Event(event));
    for (const auto& item : *series) state.object.at("series").array.push_back(Series(item));
    const auto revision = Revision(state);
    std::optional<CalendarEvent> event;
    std::optional<CalendarSeries> selectedSeries;
    if (target.kind == "calendar-event")
    {
        if (target.id.empty()) throw Error("invalid_params", "calendar-event requires an id", -32602);
        if (context.calendar.occurrence) event = context.calendar.occurrence(snapshot->generation, target.id);
        else for (const auto& item : *events) if (item.id == target.id) event = item;
        if (!event) throw Error("settings_target_not_found", "Calendar event no longer exists");
        // Recurring occurrence edits must also observe the original series revision.
        state.object.emplace("selectedOccurrence", Event(*event));
    }
    else if (target.kind == "calendar-series")
    {
        if (target.id.empty()) throw Error("invalid_params", "calendar-series requires an id", -32602);
        for (const auto& item : *series) if (item.id == target.id) selectedSeries = item;
        if (!selectedSeries) throw Error("settings_target_not_found", "Calendar series no longer exists");
    }
    else if (target.kind != "calendar" || !target.id.empty()) throw Error("settings_target_not_found", "Unknown calendar settings target");
    const auto targetRevision = event ? Revision(state) : revision;
    if (method == "settings.get" || method == "settings.catalog")
    {
        Fields(params, {"target", "offset", "limit"});
        Value values;
        if (event) values = Event(*event);
        else if (selectedSeries) values = Series(*selectedSeries);
        else
        {
            const int offset = params.Find("offset") ? Integer(params, "offset", 0, 2000) : 0;
            const int limit = params.Find("limit") ? Integer(params, "limit", 1, 100) : 100;
            auto eventRows = Array(), seriesRows = Array();
            for (std::size_t index = static_cast<std::size_t>(offset); index < events->size() && index < static_cast<std::size_t>(offset + limit); ++index)
            {
                const auto& row = events->at(index);
                eventRows.array.push_back(Object({{"id", String(row.id)}, {"title", String(row.title)}, {"date", String(row.date)}, {"hostRevision", Number(row.revision)}}));
            }
            for (std::size_t index = static_cast<std::size_t>(offset); index < series->size() && index < static_cast<std::size_t>(offset + limit); ++index)
            {
                const auto& row = series->at(index);
                seriesRows.array.push_back(Object({{"id", String(row.id)}, {"title", String(row.event.title)}, {"kind", String(row.rule.kind)}, {"hostRevision", Number(row.revision)}}));
            }
            values = Object({{"events", std::move(eventRows)}, {"series", std::move(seriesRows)},
                {"eventCount", Number(static_cast<double>(events->size()))}, {"seriesCount", Number(static_cast<double>(series->size()))},
                {"offset", Number(offset)}, {"limit", Number(limit)}});
        }
        auto output = Object({{"target", TargetJson(target)}, {"revision", String(targetRevision)}, {"values", std::move(values)}});
        if (method == "settings.catalog")
        {
            output.object.emplace("fields", event ? EventFields() : selectedSeries ? Object({{"event", EventFields()},
                {"rule", Object({{"kind", Names({"dates", "weekly", "monthly"})}, {"interval", Object({{"minimum", Number(1)}, {"maximum", Number(99)}})},
                    {"weekdays", Object({{"minimum", Number(1)}, {"maximum", Number(7)}, {"maximumItems", Number(7)}})},
                    {"monthDay", Object({{"minimum", Number(0)}, {"maximum", Number(31)}})}, {"dates", Object({{"maximumItems", Number(366)}})}})}}) : Object());
            output.object.emplace("actions", event || selectedSeries ? Names({"remove"}) : Names({"create-event", "create-series"}));
            output.object.emplace("readOnly", Names({"id", "hostRevision", "seriesId", "occurrenceDate", "occurrenceOverride", "exceptions"}));
            output.object.emplace("occurrenceTargetFormat", String("calendar-event id is seriesId/YYYY-MM-DD; edit/remove uses the existing occurrence service"));
        }
        return output;
    }
    if (snapshot->externalReplacementPending) throw Error("settings_unavailable", "Data replacement is pending");
    RequireRevision(params, targetRevision);
    if (method == "settings.reset") throw Error("unsupported_setting", "Calendar data has no settings default; edit or explicitly remove it");
    if (method == "settings.update")
    {
        Fields(params, {"target", "ifRevision", "changes"});
        const auto& changes = InputObject(params, "changes");
        if (event)
        {
            if (!context.calendar.mutate) throw Error("settings_unavailable", "Calendar event editing is unavailable");
            EventPatch(changes, *event);
            return Result(target, context.calendar.mutate(snapshot->generation, *event, false));
        }
        if (selectedSeries)
        {
            if (!context.calendar.mutateSeries) throw Error("settings_unavailable", "Calendar series editing is unavailable");
            SeriesPatch(changes, *selectedSeries);
            return Result(target, context.calendar.mutateSeries(snapshot->generation, *selectedSeries, false));
        }
        throw Error("unsupported_setting", "Edit a calendar event or series target");
    }
    if (method != "settings.action") throw Error("method_not_found", "Unknown calendar settings method", -32601);
    Fields(params, {"target", "ifRevision", "action", "arguments"});
    const auto action = Text(params, "action");
    if (action == "remove" && (event || selectedSeries))
    {
        const auto* arguments = params.Find("arguments");
        if (!arguments) throw Error("confirmation_required", "Calendar removal requires arguments.confirmed:true");
        Fields(*arguments, {"confirmed"});
        Confirmation(*arguments);
        if (event && context.calendar.mutate) return Result(target, context.calendar.mutate(snapshot->generation, *event, true));
        if (selectedSeries && context.calendar.mutateSeries) return Result(target, context.calendar.mutateSeries(snapshot->generation, *selectedSeries, true));
        throw Error("settings_unavailable", "Calendar removal is unavailable");
    }
    if (!event && !selectedSeries && action == "create-event")
    {
        CalendarEvent created;
        EventPatch(InputObject(params, "arguments"), created);
        if (!context.calendar.mutate) throw Error("settings_unavailable", "Calendar event creation is unavailable");
        return Result(target, context.calendar.mutate(snapshot->generation, std::move(created), false));
    }
    if (!event && !selectedSeries && action == "create-series")
    {
        CalendarSeries created;
        SeriesPatch(InputObject(params, "arguments"), created);
        if (!context.calendar.mutateSeries) throw Error("settings_unavailable", "Calendar series creation is unavailable");
        return Result(target, context.calendar.mutateSeries(snapshot->generation, std::move(created), false));
    }
    throw Error("unsupported_action", "Unknown calendar action for this target");
}
}
