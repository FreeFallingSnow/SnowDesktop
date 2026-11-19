#include "widget_schedule_lua.h"

#include "widget/preview/widget_preview_context.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget_lua_binding_context.h"
#include "widget_engine.h"

#include <cmath>
#include <unordered_set>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
static bool IsValidAnimationIdUtf8(const std::string& value)
{
    return !value.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

static int LuaScheduleSet(lua_State* state, bool repeat,
    const char* functionName)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "%s: id must contain 1 to 128 bytes", functionName);
    }
    const lua_Integer delay = luaL_checkinteger(state, 2);
    if (delay <= 0 || delay >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxIntervalMs)
    {
        return luaL_error(state,
            "%s: milliseconds must be between 1 and 86400000",
            functionName);
    }
    auto hiddenPolicy =
        snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 3))
    {
        luaL_checktype(state, 3, LUA_TTABLE);
        lua_getfield(state, 3, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "%s: whenHidden must be 'pause', 'throttle', or 'continue'",
                    functionName);
        }
        lua_pop(state, 1);
    }
    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeSetTimer(BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            std::string(name, nameLength), static_cast<int>(delay),
            repeat, hiddenPolicy));
    return 1;
}

int lua_ScheduleAt(lua_State* state)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "schedule.at: id must contain 1 to 128 bytes");
    }
    const lua_Integer epochMilliseconds = luaL_checkinteger(state, 2);
    const auto wallNowMilliseconds = snowdesktop::widget_runtime::
        CurrentWallClockMilliseconds();
    if (epochMilliseconds < 0 ||
        epochMilliseconds > wallNowMilliseconds +
            snowdesktop::widget_runtime::NamedTimerSchedule::
                MaxAbsoluteDelayMs)
    {
        return luaL_error(state,
            "schedule.at: epochMilliseconds must be a non-negative UTC timestamp no more than 366 days in the future");
    }
    auto hiddenPolicy =
        snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
    if (!lua_isnoneornil(state, 3))
    {
        luaL_checktype(state, 3, LUA_TTABLE);
        lua_getfield(state, 3, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::ScheduleHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "schedule.at: whenHidden must be 'pause', 'throttle', or 'continue'");
        }
        lua_pop(state, 1);
    }
    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeSetTimerAt(BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            std::string(name, nameLength),
            static_cast<std::int64_t>(epochMilliseconds), hiddenPolicy));
    return 1;
}

int lua_ScheduleTimeline(lua_State* state)
{
    using Schedule = snowdesktop::widget_runtime::NamedTimerSchedule;
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength > Schedule::MaxNameBytes)
        return luaL_error(state,
            "schedule.timeline: id must contain 1 to 128 bytes");
    luaL_checktype(state, 2, LUA_TTABLE);
    if (lua_getmetatable(state, 2) != 0)
    {
        lua_pop(state, 1);
        return luaL_error(state,
            "schedule.timeline: entries must be a plain array");
    }
    const std::size_t entryCount = lua_rawlen(state, 2);
    if (entryCount == 0 || entryCount > Schedule::MaxTimelineEntries)
        return luaL_error(state,
            "schedule.timeline: entries must contain 1 to 64 items");
    std::size_t enumeratedEntries = 0;
    lua_pushnil(state);
    while (lua_next(state, 2) != 0)
    {
        ++enumeratedEntries;
        const bool validIndex = lua_isinteger(state, -2) &&
            lua_tointeger(state, -2) >= 1 &&
            static_cast<std::size_t>(lua_tointeger(state, -2)) <=
                entryCount;
        lua_pop(state, 1);
        if (!validIndex)
            return luaL_error(state,
                "schedule.timeline: entries must be a contiguous array");
    }
    if (enumeratedEntries != entryCount)
        return luaL_error(state,
            "schedule.timeline: entries must be a contiguous array");

    const std::int64_t wallNowMilliseconds =
        snowdesktop::widget_runtime::CurrentWallClockMilliseconds();
    std::vector<Schedule::TimelineEntry> entries;
    entries.reserve(entryCount);
    std::int64_t previousAt = -1;
    std::size_t valueNodes = 0;
    std::size_t valueStringBytes = 0;
    for (std::size_t index = 1; index <= entryCount; ++index)
    {
        lua_rawgeti(state, 2, static_cast<lua_Integer>(index));
        if (lua_type(state, -1) != LUA_TTABLE ||
            lua_getmetatable(state, -1) != 0)
        {
            if (lua_istable(state, -1)) lua_pop(state, 1);
            return luaL_error(state,
                "schedule.timeline: each entry must be a plain table");
        }
        const int entryTable = lua_absindex(state, -1);
        lua_pushnil(state);
        while (lua_next(state, entryTable) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
                return luaL_error(state,
                    "schedule.timeline: entry keys must be strings");
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            const std::string_view field(key ? key : "", keyLength);
            if (field != "at" && field != "value")
                return luaL_error(state,
                    "schedule.timeline: entry received an unknown field");
            lua_pop(state, 1);
        }

        lua_getfield(state, entryTable, "at");
        if (!lua_isinteger(state, -1))
            return luaL_error(state,
                "schedule.timeline: entry at must be a UTC epoch millisecond integer");
        const auto at = static_cast<std::int64_t>(
            lua_tointeger(state, -1));
        lua_pop(state, 1);
        if (at < 0 || at <= previousAt ||
            at > wallNowMilliseconds + Schedule::MaxAbsoluteDelayMs)
        {
            return luaL_error(state,
                "schedule.timeline: entry times must be strictly increasing non-negative UTC timestamps no more than 366 days in the future");
        }
        previousAt = at;

        lua_getfield(state, entryTable, "value");
        snowdesktop::widget_runtime::InteractionValue value;
        std::unordered_set<const void*> ancestors;
        std::string valueError;
        if (!ReadInteractionValue(state, -1, value, 0,
                valueNodes, valueStringBytes, ancestors, valueError))
        {
            return luaL_error(state,
                "schedule.timeline: value must be bounded JSON-like data: %s",
                valueError.c_str());
        }
        lua_pop(state, 2);
        entries.push_back({ at, std::move(value) });
    }

    auto hiddenPolicy =
        snowdesktop::widget_runtime::ScheduleHiddenPolicy::Throttle;
    bool reloadAtEnd = false;
    if (!lua_isnoneornil(state, 3))
    {
        luaL_checktype(state, 3, LUA_TTABLE);
        if (lua_getmetatable(state, 3) != 0)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "schedule.timeline: options must be a plain table");
        }
        lua_pushnil(state);
        while (lua_next(state, 3) != 0)
        {
            if (lua_type(state, -2) != LUA_TSTRING)
                return luaL_error(state,
                    "schedule.timeline: option keys must be strings");
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            const std::string_view field(key ? key : "", keyLength);
            if (field != "whenHidden" && field != "reload")
                return luaL_error(state,
                    "schedule.timeline: options received an unknown field");
            lua_pop(state, 1);
        }
        lua_getfield(state, 3, "whenHidden");
        if (!lua_isnil(state, -1))
        {
            std::size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "pause")
                hiddenPolicy = snowdesktop::widget_runtime::
                    ScheduleHiddenPolicy::Pause;
            else if (value == "throttle")
                hiddenPolicy = snowdesktop::widget_runtime::
                    ScheduleHiddenPolicy::Throttle;
            else if (value == "continue")
                hiddenPolicy = snowdesktop::widget_runtime::
                    ScheduleHiddenPolicy::Continue;
            else
                return luaL_error(state,
                    "schedule.timeline: whenHidden must be 'pause', 'throttle', or 'continue'");
        }
        lua_pop(state, 1);
        lua_getfield(state, 3, "reload");
        if (!lua_isnil(state, -1))
        {
            std::size_t valueLength = 0;
            const char* raw = luaL_checklstring(
                state, -1, &valueLength);
            const std::string_view value(raw, valueLength);
            if (value == "atEnd") reloadAtEnd = true;
            else if (value != "none")
                return luaL_error(state,
                    "schedule.timeline: reload must be 'none' or 'atEnd'");
        }
        lua_pop(state, 1);
    }

    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeSetTimeline(BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            std::string(name, nameLength), std::move(entries),
            hiddenPolicy, reloadAtEnd));
    return 1;
}

int lua_ScheduleEvery(lua_State* state)
{
    return LuaScheduleSet(state, true, "schedule.every");
}

int lua_ScheduleAfter(lua_State* state)
{
    return LuaScheduleSet(state, false, "schedule.after");
}

int lua_ScheduleCancel(lua_State* state)
{
    size_t nameLength = 0;
    const char* name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength == 0 || nameLength >
        snowdesktop::widget_runtime::NamedTimerSchedule::MaxNameBytes)
    {
        return luaL_error(state,
            "schedule.cancel: id must contain 1 to 128 bytes");
    }
    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeCancelTimer(BoundWidgetId(state), BoundWidgetRuntimeToken(state),
            std::string(name, nameLength)));
    return 1;
}

int lua_AnimationRequestFrame(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state,
            "animation.requestFrame: expected one id");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state,
            "animation.requestFrame: id must be a string");
    std::size_t nameLength = 0;
    const char* name = lua_tolstring(state, 1, &nameLength);
    const std::string frameId(name ? name : "", nameLength);
    if (frameId.empty() ||
        frameId.size() > snowdesktop::widget_runtime::
            AnimationFrameRequests::MaxNameBytes ||
        frameId.find('\0') != std::string::npos ||
        !IsValidAnimationIdUtf8(frameId))
    {
        return luaL_error(state,
            "animation.requestFrame: id must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* host = BoundWidgetEngine(state);
    const std::string error = host
        ? host->RuntimeRequestAnimationFrame(
            BoundWidgetId(state), frameId)
        : "hostUnavailable";
    lua_pushboolean(state, error.empty() ? 1 : 0);
    if (error.empty())
        lua_pushnil(state);
    else
        lua_pushlstring(state, error.data(), error.size());
    return 2;
}

int lua_AnimationCancelFrame(lua_State* state)
{
    if (lua_gettop(state) != 1)
        return luaL_error(state,
            "animation.cancelFrame: expected one id");
    if (lua_type(state, 1) != LUA_TSTRING)
        return luaL_error(state,
            "animation.cancelFrame: id must be a string");
    std::size_t nameLength = 0;
    const char* name = lua_tolstring(state, 1, &nameLength);
    const std::string frameId(name ? name : "", nameLength);
    if (frameId.empty() ||
        frameId.size() > snowdesktop::widget_runtime::
            AnimationFrameRequests::MaxNameBytes ||
        frameId.find('\0') != std::string::npos ||
        !IsValidAnimationIdUtf8(frameId))
    {
        return luaL_error(state,
            "animation.cancelFrame: id must contain 1 to 128 bytes of valid UTF-8");
    }
    auto* host = BoundWidgetEngine(state);
    lua_pushboolean(state, host &&
        host->RuntimeCancelAnimationFrame(
            BoundWidgetId(state), frameId));
    return 1;
}
}
