#pragma once

#include "widget_view_tree.h"

#include <string>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
bool ParseLuaViewTree(lua_State* state, int index, ViewNode& root,
    std::string& error);

int LuaViewBox(lua_State* state);
int LuaViewRow(lua_State* state);
int LuaViewColumn(lua_State* state);
int LuaViewStack(lua_State* state);
int LuaViewText(lua_State* state);
int LuaViewImage(lua_State* state);
int LuaViewButton(lua_State* state);
int LuaViewIcon(lua_State* state);
int LuaViewIconButton(lua_State* state);
int LuaViewShape(lua_State* state);
int LuaViewProgressBar(lua_State* state);
int LuaViewProgressRing(lua_State* state);
int LuaViewSpacer(lua_State* state);
}
