#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_ModuleRequire(lua_State* state);
}
