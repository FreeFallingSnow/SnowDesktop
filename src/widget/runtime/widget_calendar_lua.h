#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_CalendarSelectedDate(lua_State* state);
int lua_CalendarSetSelectedDate(lua_State* state);
int lua_CalendarSelectDate(lua_State* state);
int lua_CalendarPreferences(lua_State* state);
int lua_CalendarDisplayOptions(lua_State* state);
int lua_CalendarAnnotations(lua_State* state);
int lua_CalendarDateInfo(lua_State* state);
int lua_CalendarAddDays(lua_State* state);
int lua_CalendarEvents(lua_State* state);
int lua_CalendarSeriesById(lua_State* state);
int lua_CalendarCreate(lua_State* state);
int lua_CalendarUpdate(lua_State* state);
int lua_CalendarRemove(lua_State* state);
}
