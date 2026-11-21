#include "widget_time_lua.h"

#include "common/l10n.h"
#include "widget/preview/widget_preview_context.h"
#include "widget_time.h"

#include <cstdint>
#include <string>
#include <string_view>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
int lua_TimeNow(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(
        snowdesktop::widget_runtime::CurrentWallClockMilliseconds()));
    return 1;
}

int lua_TimeMonotonic(lua_State* L)
{
    lua_pushinteger(L, static_cast<lua_Integer>(
        snowdesktop::widget_runtime::CurrentMonotonicMilliseconds()));
    return 1;
}

static int LuaTimeOptions(lua_State* L, int index)
{
    if (lua_isnoneornil(L, index)) return 0;
    luaL_checktype(L, index, LUA_TTABLE);
    return lua_absindex(L, index);
}

static std::string LuaTimeStringOption(lua_State* L, int options,
    const char* name, const char* defaultValue)
{
    if (!options) return defaultValue;
    lua_getfield(L, options, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return defaultValue;
    }
    size_t length = 0;
    const char* value = luaL_checklstring(L, -1, &length);
    if (length == 0 || length > 128)
        luaL_error(L, "time: option '%s' has an invalid length", name);
    std::string result(value, length);
    lua_pop(L, 1);
    return result;
}

static std::int64_t LuaTimeDeltaField(lua_State* L, int delta,
    const char* name)
{
    lua_getfield(L, delta, name);
    if (lua_isnil(L, -1))
    {
        lua_pop(L, 1);
        return 0;
    }
    int isNumber = 0;
    const lua_Integer value = lua_tointegerx(L, -1, &isNumber);
    if (!isNumber || value < -1000000 || value > 1000000)
    {
        luaL_error(L,
            "time.add: '%s' must be an integer from -1000000 to 1000000",
            name);
    }
    lua_pop(L, 1);
    return static_cast<std::int64_t>(value);
}

int lua_TimeParts(lua_State* L)
{
    const auto now = snowdesktop::widget_runtime::
        CurrentWallClockMilliseconds();
    const auto milliseconds = lua_isnoneornil(L, 1)
        ? now : static_cast<long long>(luaL_checkinteger(L, 1));
    const char* zone = luaL_optstring(L, 2, "local");
    snowdesktop::widget_time::DateTimeParts parts;
    const auto error = snowdesktop::widget_time::Parts(
        milliseconds, zone ? zone : "local", parts);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.parts: %s",
            snowdesktop::widget_time::DescribeError(error));
    }

    lua_createtable(L, 0, 9);
    lua_pushinteger(L, parts.year); lua_setfield(L, -2, "year");
    lua_pushinteger(L, parts.month); lua_setfield(L, -2, "month");
    lua_pushinteger(L, parts.day); lua_setfield(L, -2, "day");
    lua_pushinteger(L, parts.weekday); lua_setfield(L, -2, "wday");
    lua_pushinteger(L, parts.hour); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, parts.minute); lua_setfield(L, -2, "min");
    lua_pushinteger(L, parts.second); lua_setfield(L, -2, "sec");
    lua_pushinteger(L, parts.millisecond);
    lua_setfield(L, -2, "millisecond");
    lua_pushstring(L, zone ? zone : "local");
    lua_setfield(L, -2, "timeZone");
    return 1;
}

int lua_TimeFormat(lua_State* L)
{
    const auto now = snowdesktop::widget_runtime::
        CurrentWallClockMilliseconds();
    const auto milliseconds = lua_isnoneornil(L, 1)
        ? now : static_cast<std::int64_t>(luaL_checkinteger(L, 1));
    const int options = LuaTimeOptions(L, 2);
    const std::string timeZone = LuaTimeStringOption(
        L, options, "timeZone", "local");
    const std::string locale = LuaTimeStringOption(L, options, "locale",
        Locale::Instance().GetEffectiveLanguage().c_str());
    const std::string dateStyleName = LuaTimeStringOption(
        L, options, "dateStyle", "short");
    const std::string timeStyleName = LuaTimeStringOption(
        L, options, "timeStyle", "short");
    snowdesktop::widget_time::DateStyle dateStyle;
    if (dateStyleName == "none")
        dateStyle = snowdesktop::widget_time::DateStyle::None;
    else if (dateStyleName == "short")
        dateStyle = snowdesktop::widget_time::DateStyle::Short;
    else if (dateStyleName == "long")
        dateStyle = snowdesktop::widget_time::DateStyle::Long;
    else
        return luaL_error(L,
            "time.format: dateStyle must be 'none', 'short', or 'long'");
    snowdesktop::widget_time::TimeStyle timeStyle;
    if (timeStyleName == "none")
        timeStyle = snowdesktop::widget_time::TimeStyle::None;
    else if (timeStyleName == "short")
        timeStyle = snowdesktop::widget_time::TimeStyle::Short;
    else if (timeStyleName == "long")
        timeStyle = snowdesktop::widget_time::TimeStyle::Long;
    else
        return luaL_error(L,
            "time.format: timeStyle must be 'none', 'short', or 'long'");

    std::string result;
    const auto error = snowdesktop::widget_time::Format(milliseconds,
        locale, timeZone, dateStyle, timeStyle, result);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.format: %s",
            snowdesktop::widget_time::DescribeError(error));
    }
    lua_pushlstring(L, result.data(), result.size());
    return 1;
}

int lua_TimeAdd(lua_State* L)
{
    const auto milliseconds = static_cast<std::int64_t>(
        luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    const int deltaTable = lua_absindex(L, 2);
    snowdesktop::widget_time::AddDelta delta;
    delta.years = LuaTimeDeltaField(L, deltaTable, "years");
    delta.months = LuaTimeDeltaField(L, deltaTable, "months");
    delta.days = LuaTimeDeltaField(L, deltaTable, "days");
    delta.hours = LuaTimeDeltaField(L, deltaTable, "hours");
    delta.minutes = LuaTimeDeltaField(L, deltaTable, "minutes");
    delta.seconds = LuaTimeDeltaField(L, deltaTable, "seconds");
    delta.milliseconds = LuaTimeDeltaField(
        L, deltaTable, "milliseconds");
    const int options = LuaTimeOptions(L, 3);
    const std::string timeZone = LuaTimeStringOption(
        L, options, "timeZone", "local");
    std::int64_t result = 0;
    const auto error = snowdesktop::widget_time::Add(
        milliseconds, delta, timeZone, result);
    if (error != snowdesktop::widget_time::TimeError::None)
    {
        return luaL_error(L, "time.add: %s",
            snowdesktop::widget_time::DescribeError(error));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(result));
    return 1;
}

int lua_TimeCompare(lua_State* L)
{
    const lua_Integer left = luaL_checkinteger(L, 1);
    const lua_Integer right = luaL_checkinteger(L, 2);
    lua_pushinteger(L, left < right ? -1 : (left > right ? 1 : 0));
    return 1;
}
}
