#pragma once

#include <algorithm>
#include <cstring>
#include <string>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
enum class LuaResourceType : unsigned char
{
    Image,
    Font,
};

struct LuaResourceHandle
{
    LuaResourceType type = LuaResourceType::Image;
    char name[65]{};
};

inline constexpr char kResourceHandleMetatable[] =
    "SnowDesktop.PackageResourceHandle";

inline LuaResourceHandle* TestResourceHandle(lua_State* state, int index)
{
    return static_cast<LuaResourceHandle*>(
        luaL_testudata(state, index, kResourceHandleMetatable));
}

inline int LuaResourceHandleToString(lua_State* state)
{
    const auto* handle = TestResourceHandle(state, 1);
    if (!handle)
        return luaL_error(state, "invalid package resource handle");
    const char* type = handle->type == LuaResourceType::Image
        ? "image" : "font";
    lua_pushfstring(state, "resource.%s(%s)", type, handle->name);
    return 1;
}

inline void PushResourceHandle(lua_State* state, LuaResourceType type,
    const std::string& name)
{
    auto* handle = static_cast<LuaResourceHandle*>(
        lua_newuserdata(state, sizeof(LuaResourceHandle)));
    *handle = {};
    handle->type = type;
    std::memcpy(handle->name, name.data(),
        std::min(name.size(), sizeof(handle->name) - 1));
    if (luaL_newmetatable(state, kResourceHandleMetatable))
    {
        lua_pushcfunction(state, LuaResourceHandleToString);
        lua_setfield(state, -2, "__tostring");
        lua_pushliteral(state, "package resource handle");
        lua_setfield(state, -2, "__metatable");
    }
    lua_setmetatable(state, -2);
}
}
