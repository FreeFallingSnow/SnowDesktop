#pragma once

#include "widget_task_broker.h"

#include <optional>
#include <string>
#include <unordered_map>

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_TaskStart(lua_State* state);
int lua_TaskCancel(lua_State* state);
int lua_HttpRequest(lua_State* state);
int lua_HttpCancel(lua_State* state);

struct LuaTaskStartHost
{
    using Arguments = std::unordered_map<std::string, std::string>;

    // An empty result means there is no host bound to the Lua state.
    std::optional<TaskStartResult> (*startTask)(
        lua_State*, const std::string&, Arguments&&);
    bool (*hasPackageImage)(lua_State*, int);
    bool (*isValidUtf8)(const std::string&);
};

int StartTaskFromLua(lua_State* state, const LuaTaskStartHost& host);
}
