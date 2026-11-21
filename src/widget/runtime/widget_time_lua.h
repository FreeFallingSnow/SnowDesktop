#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_TimeNow(lua_State* state);
int lua_TimeMonotonic(lua_State* state);
int lua_TimeParts(lua_State* state);
int lua_TimeFormat(lua_State* state);
int lua_TimeAdd(lua_State* state);
int lua_TimeCompare(lua_State* state);
}
