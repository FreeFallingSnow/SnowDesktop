#include "widget_system_control_data.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
void PushSystemControlJson(lua_State* state, const JsonValue& value)
{
    luaL_checkstack(state, 3, "JSON value exceeds Lua stack capacity");
    switch (value.type)
    {
    case JsonValue::Type::Null: lua_pushnil(state); break;
    case JsonValue::Type::Boolean: lua_pushboolean(state, value.boolean); break;
    case JsonValue::Type::Number: lua_pushnumber(state, value.number); break;
    case JsonValue::Type::String:
        lua_pushlstring(state, value.string.data(), value.string.size()); break;
    case JsonValue::Type::Array:
        lua_createtable(state, static_cast<int>(value.array.size()), 0);
        for (std::size_t i = 0; i < value.array.size(); ++i)
        {
            PushSystemControlJson(state, value.array[i]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        }
        break;
    case JsonValue::Type::Object:
        lua_createtable(state, 0, static_cast<int>(value.object.size()));
        for (const auto& [key, child] : value.object)
        {
            PushSystemControlJson(state, child);
            lua_setfield(state, -2, key.c_str());
        }
        break;
    }
}
}
