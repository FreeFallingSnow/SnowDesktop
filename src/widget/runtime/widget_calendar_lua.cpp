#include "widget_calendar_lua.h"

#include "common/l10n.h"
#include "common/utils.h"
#include "widget/api/widget_calendar_query_options.h"
#include "widget_lua_binding_context.h"
#include "widget_engine.h"

#include <algorithm>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
static void PushCalendarEvent(
    lua_State* L,
    const snowdesktop::calendar::CalendarEvent& event)
{
    lua_createtable(L, 0, 13);
    lua_pushlstring(
        L, event.id.data(), event.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, event.revision);
    lua_setfield(L, -2, "revision");
    lua_pushlstring(
        L, event.title.data(), event.title.size());
    lua_setfield(L, -2, "title");
    lua_pushlstring(
        L, event.date.data(), event.date.size());
    lua_setfield(L, -2, "date");
    lua_pushboolean(L, event.allDay);
    lua_setfield(L, -2, "allDay");
    lua_pushinteger(L, event.startMinutes);
    lua_setfield(L, -2, "startMinutes");
    lua_pushinteger(L, event.endMinutes);
    lua_setfield(L, -2, "endMinutes");
    lua_pushlstring(
        L, event.notes.data(), event.notes.size());
    lua_setfield(L, -2, "notes");
    lua_pushinteger(L, event.reminderMinutes);
    lua_setfield(L, -2, "reminderMinutes");
    if (!event.seriesId.empty())
    {
        lua_pushlstring(L, event.seriesId.data(), event.seriesId.size());
        lua_setfield(L, -2, "seriesId");
        lua_pushlstring(L, event.occurrenceDate.data(), event.occurrenceDate.size());
        lua_setfield(L, -2, "occurrenceDate");
        lua_pushboolean(L, event.occurrenceOverride);
        lua_setfield(L, -2, "occurrenceOverride");
    }
}

static void PushCalendarSeries(lua_State* L,
    const snowdesktop::calendar::CalendarSeries& series)
{
    lua_createtable(L, 0, 5);
    lua_pushlstring(L, series.id.data(), series.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, series.revision);
    lua_setfield(L, -2, "revision");
    PushCalendarEvent(L, series.event);
    lua_setfield(L, -2, "event");
    lua_createtable(L, 0, 7);
    const auto& rule = series.rule;
    lua_pushlstring(L, rule.kind.data(), rule.kind.size());
    lua_setfield(L, -2, "kind");
    lua_pushlstring(L, rule.startDate.data(), rule.startDate.size());
    lua_setfield(L, -2, "startDate");
    lua_pushlstring(L, rule.endDate.data(), rule.endDate.size());
    lua_setfield(L, -2, "endDate");
    lua_pushinteger(L, rule.interval);
    lua_setfield(L, -2, "interval");
    lua_pushinteger(L, rule.monthDay);
    lua_setfield(L, -2, "monthDay");
    lua_createtable(L, static_cast<int>(rule.dates.size()), 0);
    for (std::size_t i = 0; i < rule.dates.size(); ++i)
    {
        lua_pushlstring(L, rule.dates[i].data(), rule.dates[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "dates");
    lua_createtable(L, static_cast<int>(rule.weekdays.size()), 0);
    for (std::size_t i = 0; i < rule.weekdays.size(); ++i)
    {
        lua_pushinteger(L, rule.weekdays[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(L, -2, "weekdays");
    lua_setfield(L, -2, "rule");
    lua_createtable(L, static_cast<int>(series.exceptions.size()), 0);
    int index = 1;
    for (const auto& [date, exception] : series.exceptions)
    {
        lua_createtable(L, 0, 3);
        lua_pushlstring(L, date.data(), date.size());
        lua_setfield(L, -2, "occurrenceDate");
        lua_pushboolean(L, exception.canceled);
        lua_setfield(L, -2, "canceled");
        if (!exception.canceled)
        {
            PushCalendarEvent(L, exception.event);
            lua_setfield(L, -2, "event");
        }
        lua_rawseti(L, -2, index++);
    }
    lua_setfield(L, -2, "exceptions");
}

static snowdesktop::calendar::CalendarEvent
ReadCalendarEvent(lua_State* L, int index)
{
    const int table = lua_absindex(L, index);
    luaL_checktype(L, table, LUA_TTABLE);
    snowdesktop::calendar::CalendarEvent event;
    auto readString = [&](const char* field) {
        lua_getfield(L, table, field);
        std::string result;
        if (lua_isstring(L, -1))
        {
            size_t length = 0;
            const char* value =
                lua_tolstring(L, -1, &length);
            if (value)
                result.assign(value, length);
        }
        lua_pop(L, 1);
        return result;
    };
    auto readInteger = [&](const char* field, int fallback) {
        lua_getfield(L, table, field);
        const int result = lua_isinteger(L, -1)
            ? static_cast<int>(lua_tointeger(L, -1))
            : fallback;
        lua_pop(L, 1);
        return result;
    };
    event.title = readString("title");
    event.date = readString("date");
    lua_getfield(L, table, "allDay");
    event.allDay =
        lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    event.startMinutes =
        readInteger("startMinutes", 0);
    event.endMinutes =
        readInteger("endMinutes", 0);
    event.notes = readString("notes");
    event.reminderMinutes =
        readInteger("reminderMinutes", -1);
    return event;
}

static void PushCalendarMutation(
    lua_State* L,
    const snowdesktop::calendar::MutationResult& result)
{
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, result.ok);
    lua_setfield(L, -2, "ok");
    lua_pushlstring(
        L, result.id.data(), result.id.size());
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, result.revision);
    lua_setfield(L, -2, "revision");
    lua_pushlstring(
        L, result.error.data(), result.error.size());
    lua_setfield(L, -2, "error");
}

int lua_CalendarSelectedDate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    auto* host = BoundWidgetEngine(L);
    const std::string date =
        host
        ? host->RuntimeCalendarSelectedDate()
        : std::string();
    lua_pushlstring(L, date.data(), date.size());
    return 1;
}

int lua_CalendarSetSelectedDate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    size_t length = 0;
    const char* value =
        luaL_checklstring(L, 1, &length);
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(
        L, host &&
            host->RuntimeCalendarSetSelectedDate(
                std::string(value, length)));
    return 1;
}

int lua_CalendarSelectDate(lua_State* L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "calendar.selectDate: expected one date");
    size_t length = 0;
    const char* value = luaL_checklstring(L, 1, &length);
    const std::string date(value ? value : "", length);
    auto* host = BoundWidgetEngine(L);
    lua_pushboolean(L, host && length == 10 &&
        date.find('\0') == std::string::npos &&
        host->RuntimeCalendarSetSelectedDate(date));
    return 1;
}

int lua_CalendarPreferences(lua_State* L)
{
    if (lua_gettop(L) != 0) return luaL_error(L, "calendar.preferences: expected no arguments");
    auto* host = BoundWidgetEngine(L);
    const auto p = host ? host->CalendarDisplayPreferences()
        : snowdesktop::calendar::DisplayPreferences{};
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, p.enabled); lua_setfield(L, -2, "enabled");
    lua_pushstring(L, p.calendar.c_str()); lua_setfield(L, -2, "calendar");
    lua_pushboolean(L, p.holidaysEnabled); lua_setfield(L, -2, "holidaysEnabled");
    lua_pushstring(L, p.region.c_str()); lua_setfield(L, -2, "region");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "holidayFirstYear");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "holidayLastYear");
    return 1;
}
int lua_CalendarDisplayOptions(lua_State* L)
{
    if (lua_gettop(L) != 0) return luaL_error(L, "calendar.displayOptions: expected no arguments");
    lua_createtable(L, 0, 2);
    const auto push = [&](const auto& options, const char* field) {
        lua_createtable(L, static_cast<int>(options.size()), 0);
        int index = 1;
        for (const auto& option : options)
        {
            lua_createtable(L, 0, 2);
            lua_pushstring(L, option.id.c_str()); lua_setfield(L, -2, "id");
            const auto label = WideToUtf8(option.label);
            lua_pushlstring(L, label.data(), label.size()); lua_setfield(L, -2, "label");
            lua_rawseti(L, -2, index++);
        }
        lua_setfield(L, -2, field);
    };
    push(snowdesktop::calendar::CalendarOptions(Locale::Instance().GetEffectiveLanguage()), "calendars");
    push(std::vector<snowdesktop::calendar::DisplayOption>{}, "regions");
    return 1;
}
int lua_CalendarAnnotations(lua_State* L)
{
    if (lua_gettop(L) != 2) return luaL_error(L, "calendar.annotations: expected fromDate and toDate");
    size_t fromSize = 0, toSize = 0;
    const char* from = luaL_checklstring(L, 1, &fromSize);
    const char* to = luaL_checklstring(L, 2, &toSize);
    auto* host = BoundWidgetEngine(L);
    if (!host || fromSize != 10 || toSize != 10)
    { lua_pushnil(L); return 1; }
    const auto& days = host->RuntimeCalendarAnnotations(std::string(from, fromSize), std::string(to, toSize));
    if (days.empty()) { lua_pushnil(L); return 1; }
    lua_createtable(L, static_cast<int>(days.size()), 0);
    int index = 1;
    for (const auto& item : days)
    {
        lua_createtable(L, 0, 12);
        const auto text = [&](const char* key, const std::string& value) { lua_pushlstring(L, value.data(), value.size()); lua_setfield(L, -2, key); };
        const auto number = [&](const char* key, int value) { lua_pushinteger(L, value); lua_setfield(L, -2, key); };
        const auto flag = [&](const char* key, bool value) { lua_pushboolean(L, value); lua_setfield(L, -2, key); };
        text("date", item.date); text("secondary", item.secondary); text("fullDate", item.fullDate);
        number("year", item.year); number("month", item.month); number("day", item.day); number("era", item.era);
        flag("leapMonth", item.leapMonth); flag("calendarAvailable", item.calendarAvailable); flag("holidaysAvailable", item.holidaysAvailable);
        lua_createtable(L, static_cast<int>(item.holidays.size()), 0);
        int holidayIndex = 1;
        for (const auto& name : item.holidays) { lua_pushlstring(L, name.data(), name.size()); lua_rawseti(L, -2, holidayIndex++); }
        lua_setfield(L, -2, "holidays");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int lua_CalendarDateInfo(lua_State* L)
{
    if (lua_gettop(L) != 1)
        return luaL_error(L, "calendar.dateInfo: expected one date");
    const char* value = luaL_checkstring(L, 1);
    auto* host = BoundWidgetEngine(L);
    const auto info = host
        ? host->RuntimeCalendarDateInfo(value)
        : std::nullopt;
    if (!info)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, info->year);
    lua_setfield(L, -2, "year");
    lua_pushinteger(L, info->month);
    lua_setfield(L, -2, "month");
    lua_pushinteger(L, info->day);
    lua_setfield(L, -2, "day");
    lua_pushinteger(L, info->weekday);
    lua_setfield(L, -2, "weekday");
    lua_pushinteger(L, info->daysInMonth);
    lua_setfield(L, -2, "daysInMonth");
    return 1;
}

int lua_CalendarAddDays(lua_State* L)
{
    if (lua_gettop(L) != 2)
        return luaL_error(L,
            "calendar.addDays: expected a date and offset");
    const char* value = luaL_checkstring(L, 1);
    const lua_Integer requestedOffset = luaL_checkinteger(L, 2);
    if (requestedOffset < -366000 || requestedOffset > 366000)
    {
        return luaL_error(L,
            "calendar.addDays: offset must be between -366000 and 366000");
    }
    const int offset = static_cast<int>(requestedOffset);
    auto* host = BoundWidgetEngine(L);
    const auto result = host
        ? host->RuntimeCalendarAddDays(
            value, offset)
        : std::nullopt;
    if (!result)
    {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(
        L, result->data(), result->size());
    return 1;
}

int lua_CalendarEvents(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    const char* fromDate =
        luaL_checkstring(L, 1);
    const char* toDate =
        luaL_checkstring(L, 2);
    auto* host = BoundWidgetEngine(L);
    const auto events = host
        ? host->RuntimeCalendarEvents(
            fromDate, toDate)
        : std::vector<
            snowdesktop::calendar::CalendarEvent>{};
    lua_createtable(
        L, static_cast<int>(events.size()), 0);
    int index = 1;
    for (const auto& event : events)
    {
        PushCalendarEvent(L, event);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

int lua_CalendarSeriesById(lua_State* L)
{
    if (!RequirePermission(L, "calendar.read"))
        return 0;
    const char* id = luaL_checkstring(L, 1);
    auto* host = BoundWidgetEngine(L);
    const auto series = host
        ? host->RuntimeCalendarSeriesById(id)
        : std::nullopt;
    if (!series) lua_pushnil(L);
    else PushCalendarSeries(L, *series);
    return 1;
}

int lua_CalendarCreate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    auto* host = BoundWidgetEngine(L);
    const auto result = host
        ? host->RuntimeCalendarCreate(
            ReadCalendarEvent(L, 1))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}

int lua_CalendarUpdate(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    const char* id = luaL_checkstring(L, 1);
    const int revision = static_cast<int>(
        luaL_checkinteger(L, 2));
    auto* host = BoundWidgetEngine(L);
    const auto result = host
        ? host->RuntimeCalendarUpdate(
            id, revision,
            ReadCalendarEvent(L, 3))
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}

int lua_CalendarRemove(lua_State* L)
{
    if (!RequirePermission(L, "calendar.write"))
        return 0;
    const char* id = luaL_checkstring(L, 1);
    auto* host = BoundWidgetEngine(L);
    const auto result = host
        ? host->RuntimeCalendarRemove(id)
        : snowdesktop::calendar::MutationResult{
            false, {}, 0, "unavailable"
        };
    PushCalendarMutation(L, result);
    return 1;
}
}
