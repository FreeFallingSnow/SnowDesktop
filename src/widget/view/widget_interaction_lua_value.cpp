#include "widget_interaction_lua_value.h"

#include <cmath>
#include <string_view>
#include <utility>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
bool ReadInteractionValue(lua_State* state, int index,
    snowdesktop::widget_runtime::InteractionValue& output,
    std::size_t depth, std::size_t& nodes, std::size_t& stringBytes,
    std::unordered_set<const void*>& ancestors, std::string& error)
{
    using Value = snowdesktop::widget_runtime::InteractionValue;
    index = lua_absindex(state, index);
    if (++nodes > 256 || depth > 8)
    {
        error = "interaction action value exceeds its size or depth limit";
        return false;
    }
    switch (lua_type(state, index))
    {
    case LUA_TNIL:
        output.type = Value::Type::Null;
        return true;
    case LUA_TBOOLEAN:
        output.type = Value::Type::Boolean;
        output.boolean = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        if (lua_isinteger(state, index))
        {
            output.type = Value::Type::Integer;
            output.integer = static_cast<long long>(lua_tointeger(state, index));
        }
        else
        {
            output.type = Value::Type::Number;
            output.number = static_cast<double>(lua_tonumber(state, index));
            if (!std::isfinite(output.number))
            {
                error = "interaction action numbers must be finite";
                return false;
            }
        }
        return true;
    case LUA_TSTRING:
    {
        std::size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (length > 16 * 1024 || stringBytes > 16 * 1024 - length)
        {
            error = "interaction action strings exceed 16 KiB";
            return false;
        }
        stringBytes += length;
        output.type = Value::Type::String;
        output.string.assign(value ? value : "", length);
        return true;
    }
    case LUA_TTABLE:
        break;
    default:
        error = "interaction action values must be serializable";
        return false;
    }

    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = "interaction action tables cannot have metatables";
        return false;
    }
    const void* identity = lua_topointer(state, index);
    if (!ancestors.insert(identity).second)
    {
        error = "interaction action values cannot be cyclic";
        return false;
    }

    bool integerKeys = false;
    bool stringKeys = false;
    std::size_t count = 0;
    std::size_t maximumIndex = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++count;
        if (lua_isinteger(state, -2))
        {
            const lua_Integer key = lua_tointeger(state, -2);
            if (key <= 0 || key > 256)
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                error = "interaction action array keys must be contiguous";
                return false;
            }
            integerKeys = true;
            maximumIndex = std::max(maximumIndex,
                static_cast<std::size_t>(key));
        }
        else if (lua_type(state, -2) == LUA_TSTRING)
            stringKeys = true;
        else
        {
            lua_pop(state, 2);
            ancestors.erase(identity);
            error = "interaction action object keys must be strings";
            return false;
        }
        lua_pop(state, 1);

    }
    if ((integerKeys && stringKeys) ||
        (integerKeys && maximumIndex != count))
    {
        ancestors.erase(identity);
        error = "interaction action tables must be arrays or objects";
        return false;
    }

    if (integerKeys)
    {
        output.type = Value::Type::Array;
        output.array.resize(count);
        for (std::size_t item = 0; item < count; ++item)
        {
            lua_rawgeti(state, index, static_cast<lua_Integer>(item + 1));
            if (!ReadInteractionValue(state, -1, output.array[item],
                    depth + 1, nodes, stringBytes, ancestors, error))
            {
                lua_pop(state, 1);
                ancestors.erase(identity);
                return false;
            }
            lua_pop(state, 1);
        }
    }
    else
    {
        output.type = Value::Type::Object;
        lua_pushnil(state);
        while (lua_next(state, index) != 0)
        {
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            if (!key || keyLength == 0 || keyLength > 128 ||
                stringBytes > 16 * 1024 - keyLength)
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                error = "interaction action object key is invalid";
                return false;
            }
            stringBytes += keyLength;
            Value child;
            if (!ReadInteractionValue(state, -1, child, depth + 1,
                    nodes, stringBytes, ancestors, error))
            {
                lua_pop(state, 2);
                ancestors.erase(identity);
                return false;
            }
            output.object.emplace(std::string(key, keyLength),
                std::move(child));
            lua_pop(state, 1);
        }
    }
    ancestors.erase(identity);
    return true;
}

// Values originate from bounded host providers or validated helper messages.
void PushInteractionValue(lua_State* state,
    const snowdesktop::widget_runtime::InteractionValue& value)
{
    using Type = snowdesktop::widget_runtime::InteractionValue::Type;
    switch (value.type)
    {
    case Type::Null:
        lua_pushnil(state);
        break;
    case Type::Boolean:
        lua_pushboolean(state, value.boolean ? 1 : 0);
        break;
    case Type::Integer:
        lua_pushinteger(state, static_cast<lua_Integer>(value.integer));
        break;
    case Type::Number:
        lua_pushnumber(state, static_cast<lua_Number>(value.number));
        break;
    case Type::String:
        lua_pushlstring(state, value.string.data(), value.string.size());
        break;
    case Type::Array:
        lua_createtable(state, static_cast<int>(value.array.size()), 0);
        for (std::size_t index = 0; index < value.array.size(); ++index)
        {
            PushInteractionValue(state, value.array[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
        break;
    case Type::Object:
        lua_createtable(state, 0, static_cast<int>(value.object.size()));
        for (const auto& [key, child] : value.object)
        {
            PushInteractionValue(state, child);
            lua_setfield(state, -2, key.c_str());
        }
        break;
    }
}

}
