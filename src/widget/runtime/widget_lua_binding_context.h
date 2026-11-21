#pragma once

#include <cstdint>
#include <string>

class WidgetEngine;
struct lua_State;

namespace snowdesktop::widget_runtime
{
void BindWidgetLuaEngine(lua_State* state, WidgetEngine* engine);
WidgetEngine* BoundWidgetEngine(lua_State* state);
std::wstring BoundWidgetId(lua_State* state);
std::uint64_t BoundWidgetRuntimeToken(lua_State* state);
bool RequirePermission(lua_State* state, const char* permission);
}
