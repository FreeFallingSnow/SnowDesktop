#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_SlotsBinding(lua_State* state);
int lua_SlotsCollection(lua_State* state);
int lua_SlotsCanUndo(lua_State* state);
int lua_SlotsCanRedo(lua_State* state);
int lua_SlotsUndo(lua_State* state);
int lua_SlotsRedo(lua_State* state);
}
