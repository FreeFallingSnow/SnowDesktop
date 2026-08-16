#include "widget_view_lua.h"
#include "widget_view_contract.h"
#include "widget_resource_lua.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <unordered_set>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
bool ValidateObjectFields(lua_State* state, int index,
    std::initializer_list<std::string_view> allowed,
    std::string_view context, std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = std::string(context) + " cannot have a metatable";
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        if (lua_type(state, -2) != LUA_TSTRING)
        {
            lua_pop(state, 2);
            error = std::string(context) + " keys must be strings";
            return false;
        }
        std::size_t length = 0;
        const char* key = lua_tolstring(state, -2, &length);
        const std::string_view field(key ? key : "", length);
        if (std::find(allowed.begin(), allowed.end(), field) ==
            allowed.end())
        {
            lua_pop(state, 2);
            error = std::string(context) + " has unsupported field '" +
                std::string(field) + "'";
            return false;
        }
        lua_pop(state, 1);
    }
    return true;
}

bool ValidateArray(lua_State* state, int index,
    std::string_view context, std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = std::string(context) + " cannot have a metatable";
        return false;
    }
    const std::size_t length = lua_rawlen(state, index);
    std::size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++count;
        const bool validKey = lua_isinteger(state, -2) &&
            lua_tointeger(state, -2) > 0 &&
            static_cast<std::size_t>(lua_tointeger(state, -2)) <= length;
        lua_pop(state, 1);
        if (!validKey)
        {
            lua_pop(state, 1);
            error = std::string(context) +
                " must use contiguous integer keys";
            return false;
        }
    }
    if (count != length)
    {
        error = std::string(context) +
            " must use contiguous integer keys";
        return false;
    }
    return true;
}

bool FieldPresent(lua_State* state, int index, const char* field)
{
    index = lua_absindex(state, index);
    lua_getfield(state, index, field);
    const bool present = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return present;
}

bool ValidateNodeFields(lua_State* state, int index, ViewNodeType type,
    std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = "view node cannot have a metatable";
        return false;
    }
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        if (lua_type(state, -2) != LUA_TSTRING)
        {
            lua_pop(state, 2);
            error = "view node keys must be strings";
            return false;
        }
        std::size_t length = 0;
        const char* key = lua_tolstring(state, -2, &length);
        const std::string_view field(key ? key : "", length);
        if (!IsKnownViewNodeProperty(field))
        {
            lua_pop(state, 2);
            error = "view node has unsupported field '" +
                std::string(field) + "'";
            return false;
        }
        if (!ViewNodeAllowsProperty(type, field))
        {
            const char* typeName = ViewNodeTypeName(type);
            lua_pop(state, 2);
            error = std::string(typeName) + " nodes do not accept field '" +
                std::string(field) + "'";
            return false;
        }
        lua_pop(state, 1);
    }
    for (const std::string_view property :
        ViewNodeRequiredProperties(type))
    {
        const std::string name(property);
        if (!FieldPresent(state, index, name.c_str()))
        {
            error = std::string(ViewNodeTypeName(type)) +
                " nodes require " + name;
            return false;
        }
    }
    return true;
}

bool ReadActionValue(lua_State* state, int index,
    InteractionValue& output, std::size_t depth, std::size_t& nodes,
    std::size_t& stringBytes, std::unordered_set<const void*>& ancestors,
    std::string& error)
{
    using Value = InteractionValue;
    index = lua_absindex(state, index);
    if (++nodes > 256 || depth > 8)
    {
        error = "view action value exceeds its size or depth limit";
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
                error = "view action numbers must be finite";
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
            error = "view action strings exceed 16 KiB";
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
        error = "view action values must be serializable";
        return false;
    }

    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = "view action tables cannot have metatables";
        return false;
    }
    const void* identity = lua_topointer(state, index);
    if (!ancestors.insert(identity).second)
    {
        error = "view action values cannot be cyclic";
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
                error = "view action array keys must be contiguous";
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
            error = "view action object keys must be strings";
            return false;
        }
        lua_pop(state, 1);
    }
    if ((integerKeys && stringKeys) ||
        (integerKeys && maximumIndex != count))
    {
        ancestors.erase(identity);
        error = "view action tables must be arrays or objects";
        return false;
    }

    if (integerKeys)
    {
        output.type = Value::Type::Array;
        output.array.resize(count);
        for (std::size_t item = 0; item < count; ++item)
        {
            lua_rawgeti(state, index, static_cast<lua_Integer>(item + 1));
            if (!ReadActionValue(state, -1, output.array[item], depth + 1,
                    nodes, stringBytes, ancestors, error))
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
                error = "view action object key is invalid";
                return false;
            }
            stringBytes += keyLength;
            Value child;
            if (!ReadActionValue(state, -1, child, depth + 1,
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

bool ReadStringField(lua_State* state, int table, const char* field,
    std::string& value, bool required, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        if (required)
        {
            error = std::string("view node requires string field '") +
                field + "'";
            return false;
        }
        return true;
    }
    if (!lua_isstring(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field + "' must be a string";
        return false;
    }
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    value.assign(text ? text : "", length);
    lua_pop(state, 1);
    return true;
}

bool ReadFloatField(lua_State* state, int table, const char* field,
    float& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isnumber(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field + "' must be a number";
        return false;
    }
    value = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (!std::isfinite(value))
    {
        error = std::string("view field '") + field + "' must be finite";
        return false;
    }
    return true;
}

bool ReadEdgeInsetsField(lua_State* state, int table, const char* field,
    ViewEdgeInsets& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (lua_isnumber(state, -1))
    {
        const float uniform = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        if (!std::isfinite(uniform))
        {
            error = std::string("view field '") + field +
                "' must be finite";
            return false;
        }
        value = uniform;
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must be a number or edge-inset table";
        return false;
    }

    const int insets = lua_absindex(state, -1);
    const std::string context = std::string("view field '") + field + "'";
    if (!ValidateObjectFields(state, insets,
            { "horizontal", "vertical", "top", "right", "bottom", "left" },
            context, error))
    {
        lua_pop(state, 1);
        return false;
    }
    bool present = false;
    ViewEdgeInsets parsed;
    float component = 0.0f;
    if (FieldPresent(state, insets, "horizontal"))
    {
        present = true;
        if (!ReadFloatField(state, insets, "horizontal", component, error))
        {
            lua_pop(state, 1);
            return false;
        }
        parsed.left = parsed.right = component;
    }
    if (FieldPresent(state, insets, "vertical"))
    {
        present = true;
        if (!ReadFloatField(state, insets, "vertical", component, error))
        {
            lua_pop(state, 1);
            return false;
        }
        parsed.top = parsed.bottom = component;
    }
    const auto readSide = [&](const char* side, float& destination) {
        if (!FieldPresent(state, insets, side)) return true;
        present = true;
        return ReadFloatField(state, insets, side, destination, error);
    };
    if (!readSide("top", parsed.top) ||
        !readSide("right", parsed.right) ||
        !readSide("bottom", parsed.bottom) ||
        !readSide("left", parsed.left))
    {
        lua_pop(state, 1);
        return false;
    }
    lua_pop(state, 1);
    if (!present)
    {
        error = context + " must define horizontal/vertical or a side";
        return false;
    }
    value = parsed;
    return true;
}

bool ReadOffsetField(lua_State* state, int table, float& x, float& y,
    std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "offset");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = "view field 'offset' must be an { x, y } table";
        return false;
    }
    const int offset = lua_absindex(state, -1);
    if (!ValidateObjectFields(state, offset, { "x", "y" },
            "view field 'offset'", error))
    {
        lua_pop(state, 1);
        return false;
    }
    const bool hasX = FieldPresent(state, offset, "x");
    const bool hasY = FieldPresent(state, offset, "y");
    if (!hasX && !hasY)
    {
        lua_pop(state, 1);
        error = "view field 'offset' must define x or y";
        return false;
    }
    if (!ReadFloatField(state, offset, "x", x, error) ||
        !ReadFloatField(state, offset, "y", y, error))
    {
        lua_pop(state, 1);
        return false;
    }
    lua_pop(state, 1);
    return true;
}

bool ReadIntegerField(lua_State* state, int table, const char* field,
    int& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isinteger(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must be an integer";
        return false;
    }
    const lua_Integer parsed = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max())
    {
        error = std::string("view field '") + field +
            "' is outside the supported integer range";
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ReadSizeField(lua_State* state, int table, const char* field,
    std::size_t& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must be a positive integer";
        return false;
    }
    value = static_cast<std::size_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    return true;
}

bool ReadOptionalPositiveSizeField(lua_State* state, int table,
    const char* field, std::optional<std::size_t>& value,
    std::string& error)
{
    if (!FieldPresent(state, table, field)) return true;
    std::size_t parsed = 1;
    if (!ReadSizeField(state, table, field, parsed, error)) return false;
    value = parsed;
    return true;
}

bool ReadNonNegativeSizeField(lua_State* state, int table,
    const char* field, std::size_t& value, bool required,
    std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        if (!required) return true;
        error = std::string("view node requires integer field '") +
            field + "'";
        return false;
    }
    if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must be a non-negative integer";
        return false;
    }
    value = static_cast<std::size_t>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    return true;
}

bool ReadNumberArrayField(lua_State* state, int table, const char* field,
    std::vector<float>& values, bool required, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        if (!required) return true;
        error = std::string("view node requires array field '") +
            field + "'";
        return false;
    }
    if (!lua_istable(state, -1) ||
        !ValidateArray(state, -1, "view values", error))
    {
        if (error.empty()) error = "view field 'values' must be an array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t count = lua_rawlen(state, -1);
    if (count == 0 || count > ViewTreeLimits::MaximumSeriesPoints)
    {
        lua_pop(state, 1);
        error = "view values must contain 1 to 512 numbers";
        return false;
    }
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        if (lua_type(state, -1) != LUA_TNUMBER)
        {
            lua_pop(state, 2);
            error = "view values must contain only numbers";
            return false;
        }
        const double parsed = static_cast<double>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        if (!std::isfinite(parsed) || parsed < -1.0e9 || parsed > 1.0e9)
        {
            lua_pop(state, 1);
            error = "view values must be finite and between -1e9 and 1e9";
            return false;
        }
        values.push_back(static_cast<float>(parsed));
    }
    lua_pop(state, 1);
    return true;
}

bool ReadPositiveSizeArrayField(lua_State* state, int table,
    const char* field, std::vector<std::size_t>& values,
    std::size_t maximumCount, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1) ||
        !ValidateArray(state, -1, "view positive integer array", error))
    {
        if (error.empty())
            error = std::string("view field '") + field +
                "' must be an array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t count = lua_rawlen(state, -1);
    if (count > maximumCount)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' exceeds its item limit";
        return false;
    }
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) <= 0)
        {
            lua_pop(state, 2);
            error = std::string("view field '") + field +
                "' must contain positive integers";
            return false;
        }
        values.push_back(static_cast<std::size_t>(
            lua_tointeger(state, -1)));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool ReadBoolField(lua_State* state, int table, const char* field,
    bool& value, std::string& error);

bool ReadChoiceOptionsField(lua_State* state, int table,
    std::vector<ViewChoiceOption>& options, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "options");
    if (!lua_istable(state, -1) ||
        !ValidateArray(state, -1, "radioGroup options", error))
    {
        if (error.empty()) error = "radioGroup options must be an array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t count = lua_rawlen(state, -1);
    if (count == 0 || count > ViewTreeLimits::MaximumChoiceOptions)
    {
        lua_pop(state, 1);
        error = "radioGroup options must contain 1 to 64 items";
        return false;
    }
    options.clear();
    options.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        if (!lua_istable(state, -1) ||
            !ValidateObjectFields(state, -1,
                { "key", "value", "label", "enabled" },
                "radioGroup option", error))
        {
            if (error.empty()) error = "radioGroup options must be objects";
            lua_pop(state, 2);
            return false;
        }
        ViewChoiceOption option;
        if (!ReadStringField(state, -1, "key", option.key, true, error) ||
            !ReadStringField(state, -1, "value", option.value, true,
                error) ||
            !ReadStringField(state, -1, "label", option.label, true,
                error) ||
            !ReadBoolField(state, -1, "enabled", option.enabled, error))
        {
            lua_pop(state, 2);
            return false;
        }
        options.push_back(std::move(option));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool ReadBoolField(lua_State* state, int table, const char* field,
    bool& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isboolean(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field + "' must be boolean";
        return false;
    }
    value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return true;
}

bool ReadLengthField(lua_State* state, int table, const char* field,
    ViewLength& length, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (lua_isnumber(state, -1))
    {
        length.kind = ViewLengthKind::Fixed;
        length.value = static_cast<float>(lua_tonumber(state, -1));
        lua_pop(state, 1);
        return true;
    }
    if (lua_isstring(state, -1))
    {
        const char* value = lua_tostring(state, -1);
        if (value && std::strcmp(value, "fill") == 0)
            length.kind = ViewLengthKind::Fill;
        else if (value && std::strcmp(value, "auto") == 0)
            length.kind = ViewLengthKind::Auto;
        else
        {
            lua_pop(state, 1);
            error = std::string("view field '") + field +
                "' must be a number, 'auto', or 'fill'";
            return false;
        }
        lua_pop(state, 1);
        return true;
    }
    lua_pop(state, 1);
    error = std::string("view field '") + field +
        "' must be a number, 'auto', or 'fill'";
    return false;
}

bool ReadGridFraction(lua_State* state, int index, float& value,
    std::string& error)
{
    index = lua_absindex(state, index);
    if (!lua_istable(state, index) ||
        !ValidateObjectFields(state, index, { "fr" },
            "grid fractional track", error))
    {
        if (error.empty())
            error = "grid fractional track must be a { fr = number } object";
        return false;
    }
    lua_getfield(state, index, "fr");
    const bool valid = lua_type(state, -1) == LUA_TNUMBER &&
        std::isfinite(static_cast<double>(lua_tonumber(state, -1))) &&
        lua_tonumber(state, -1) > 0.0 && lua_tonumber(state, -1) <= 1000.0;
    if (valid) value = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (!valid)
    {
        error = "grid fractional track weight must be between 0 and 1000";
        return false;
    }
    return true;
}

bool ReadGridTrack(lua_State* state, int index, ViewGridTrack& track,
    std::string& error)
{
    index = lua_absindex(state, index);
    if (lua_type(state, index) == LUA_TNUMBER)
    {
        const double value = static_cast<double>(lua_tonumber(state, index));
        if (!std::isfinite(value) || value < 0.0 || value > 100000.0)
        {
            error = "fixed grid track must be between 0 and 100000";
            return false;
        }
        track.kind = ViewGridTrackKind::Fixed;
        track.value = static_cast<float>(value);
        return true;
    }
    if (lua_isstring(state, index))
    {
        const char* value = lua_tostring(state, index);
        if (value && std::strcmp(value, "auto") == 0)
        {
            track.kind = ViewGridTrackKind::Auto;
            return true;
        }
        error = "grid track string must be 'auto'";
        return false;
    }
    if (!lua_istable(state, index))
    {
        error = "grid track must be a number, 'auto', { fr }, or { min, max }";
        return false;
    }

    const bool hasFraction = FieldPresent(state, index, "fr");
    const bool hasMinimum = FieldPresent(state, index, "min");
    const bool hasMaximum = FieldPresent(state, index, "max");
    if (hasFraction && !hasMinimum && !hasMaximum)
    {
        track.kind = ViewGridTrackKind::Fraction;
        return ReadGridFraction(state, index, track.value, error);
    }
    if (hasFraction || !hasMinimum || !hasMaximum ||
        !ValidateObjectFields(state, index, { "min", "max" },
            "grid minmax track", error))
    {
        if (error.empty())
            error = "grid minmax track must contain only min and max";
        return false;
    }

    lua_getfield(state, index, "min");
    const double minimum = lua_type(state, -1) == LUA_TNUMBER
        ? static_cast<double>(lua_tonumber(state, -1)) : -1.0;
    const bool validMinimum = lua_type(state, -1) == LUA_TNUMBER &&
        std::isfinite(minimum) && minimum >= 0.0 && minimum <= 100000.0;
    lua_pop(state, 1);
    if (!validMinimum)
    {
        error = "grid minmax minimum must be between 0 and 100000";
        return false;
    }

    track.kind = ViewGridTrackKind::MinMax;
    track.minimum = static_cast<float>(minimum);
    lua_getfield(state, index, "max");
    if (lua_type(state, -1) == LUA_TNUMBER)
    {
        const double maximum = static_cast<double>(lua_tonumber(state, -1));
        if (!std::isfinite(maximum) || maximum < minimum ||
            maximum > 100000.0)
        {
            lua_pop(state, 1);
            error = "grid minmax fixed maximum must be at least min and no more than 100000";
            return false;
        }
        track.maximumKind = ViewGridTrackKind::Fixed;
        track.maximumValue = static_cast<float>(maximum);
    }
    else if (lua_isstring(state, -1))
    {
        const char* maximum = lua_tostring(state, -1);
        if (!maximum || std::strcmp(maximum, "auto") != 0)
        {
            lua_pop(state, 1);
            error = "grid minmax maximum string must be 'auto'";
            return false;
        }
        track.maximumKind = ViewGridTrackKind::Auto;
    }
    else if (lua_istable(state, -1))
    {
        track.maximumKind = ViewGridTrackKind::Fraction;
        if (!ReadGridFraction(state, -1, track.maximumValue, error))
        {
            lua_pop(state, 1);
            return false;
        }
    }
    else
    {
        lua_pop(state, 1);
        error = "grid minmax maximum must be a number, 'auto', or { fr }";
        return false;
    }
    lua_pop(state, 1);
    return true;
}

bool ReadGridTracksField(lua_State* state, int table, const char* field,
    bool allowTrackArray, std::size_t& count,
    std::vector<ViewGridTrack>& tracks, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (lua_isinteger(state, -1))
    {
        const lua_Integer parsed = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (parsed <= 0 || parsed > 64)
        {
            error = std::string("view field '") + field +
                "' must be a positive integer from 1 to 64 or a track array";
            return false;
        }
        count = static_cast<std::size_t>(parsed);
        tracks.clear();
        return true;
    }
    if (!allowTrackArray || !lua_istable(state, -1) ||
        !ValidateArray(state, -1, std::string("view ") + field, error))
    {
        if (error.empty())
            error = std::string("view field '") + field +
                "' must be an integer or a grid track array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t length = lua_rawlen(state, -1);
    if (length == 0 || length > 64)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must contain 1 to 64 tracks";
        return false;
    }
    tracks.clear();
    tracks.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        ViewGridTrack track;
        if (!ReadGridTrack(state, -1, track, error))
        {
            lua_pop(state, 2);
            error = std::string("view field '") + field +
                "' track " + std::to_string(index + 1) + ": " + error;
            return false;
        }
        tracks.push_back(track);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    count = length;
    return true;
}

bool ReadOptionalColor(lua_State* state, int table, const char* field,
    std::optional<std::uint32_t>& value, std::string& error,
    std::optional<ViewThemeColorToken>* token = nullptr)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (token && lua_type(state, -1) == LUA_TSTRING)
    {
        const std::string_view name = lua_tostring(state, -1);
        lua_pop(state, 1);
        const struct Entry
        {
            std::string_view name;
            ViewThemeColorToken token;
        } entries[] = {
            { "widgetBackground", ViewThemeColorToken::WidgetBackground },
            { "surface", ViewThemeColorToken::Surface },
            { "surfaceVariant", ViewThemeColorToken::SurfaceVariant },
            { "textPrimary", ViewThemeColorToken::TextPrimary },
            { "textSecondary", ViewThemeColorToken::TextSecondary },
            { "textDisabled", ViewThemeColorToken::TextDisabled },
            { "border", ViewThemeColorToken::Border },
            { "borderStrong", ViewThemeColorToken::BorderStrong },
            { "systemAccent", ViewThemeColorToken::SystemAccent },
            { "accentText", ViewThemeColorToken::AccentText },
            { "info", ViewThemeColorToken::Info },
            { "success", ViewThemeColorToken::Success },
            { "warning", ViewThemeColorToken::Warning },
            { "error", ViewThemeColorToken::Error },
        };
        const auto found = std::find_if(std::begin(entries),
            std::end(entries), [name](const Entry& entry) {
                return entry.name == name;
            });
        if (found == std::end(entries))
        {
            error = std::string("view color field '") + field +
                "' contains an unsupported theme token";
            return false;
        }
        value.reset();
        *token = found->token;
        return true;
    }
    if (!lua_isinteger(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view color field '") + field +
            (token ? "' must be an integer color or theme token" :
                "' must be an integer color");
        return false;
    }
    const lua_Integer color = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (color < 0 || color > 0xFFFFFF)
    {
        error = std::string("view color field '") + field +
            "' must be between 0 and 0xFFFFFF";
        return false;
    }
    value = static_cast<std::uint32_t>(color);
    if (token) token->reset();
    return true;
}

bool ReadOptionalFloat(lua_State* state, int table, const char* field,
    std::optional<float>& value, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_isnumber(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view style field '") + field +
            "' must be a number";
        return false;
    }
    const float parsed = static_cast<float>(lua_tonumber(state, -1));
    lua_pop(state, 1);
    if (!std::isfinite(parsed))
    {
        error = std::string("view style field '") + field +
            "' must be finite";
        return false;
    }
    value = parsed;
    return true;
}

bool ReadOptionalNodeFloatField(lua_State* state, int table,
    const char* field, std::optional<float>& value, std::string& error)
{
    if (!FieldPresent(state, table, field)) return true;
    float parsed = 0.0f;
    if (!ReadFloatField(state, table, field, parsed, error)) return false;
    value = parsed;
    return true;
}

bool ReadStyle(lua_State* state, int table, ViewStyle& style,
    std::string& error)
{
    return ValidateObjectFields(state, table,
            { "background", "foreground", "borderColor", "borderWidth",
                "cornerRadius", "opacity" }, "view style", error) &&
        ReadOptionalColor(state, table, "background", style.background,
            error, &style.backgroundToken) &&
        ReadOptionalColor(state, table, "foreground", style.foreground,
            error, &style.foregroundToken) &&
        ReadOptionalColor(state, table, "borderColor", style.borderColor,
            error, &style.borderColorToken) &&
        ReadOptionalFloat(state, table, "borderWidth", style.borderWidth,
            error) &&
        ReadOptionalFloat(state, table, "cornerRadius", style.cornerRadius,
            error) &&
        ReadOptionalFloat(state, table, "opacity", style.opacity, error);
}

bool ReadStyleField(lua_State* state, int table, const char* field,
    ViewStyle& style, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field + "' must be a table";
        return false;
    }
    const bool ok = ReadStyle(state, -1, style, error);
    lua_pop(state, 1);
    return ok;
}

bool ReadShadowField(lua_State* state, int table,
    std::optional<ViewShadow>& shadow, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "shadow");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = "view field 'shadow' must be a table";
        return false;
    }
    ViewShadow parsed;
    std::optional<std::uint32_t> color;
    const bool ok = ValidateObjectFields(state, -1,
            { "color", "blur", "offsetX", "offsetY", "alpha" },
            "view shadow", error) &&
        ReadOptionalColor(state, -1, "color", color, error,
            &parsed.colorToken) &&
        ReadFloatField(state, -1, "blur", parsed.blur, error) &&
        ReadFloatField(state, -1, "offsetX", parsed.offsetX, error) &&
        ReadFloatField(state, -1, "offsetY", parsed.offsetY, error) &&
        ReadFloatField(state, -1, "alpha", parsed.alpha, error);
    lua_pop(state, 1);
    if (!ok) return false;
    if (color) parsed.color = *color;
    shadow = parsed;
    return true;
}

bool ReadTransformField(lua_State* state, int table,
    std::optional<ViewTransform>& transform, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "transform");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = "view field 'transform' must be a table";
        return false;
    }
    ViewTransform parsed;
    const bool ok = ValidateObjectFields(state, -1,
            { "translateX", "translateY", "scale", "scaleX", "scaleY",
                "rotate", "skewX", "skewY", "originX", "originY" },
            "view transform", error) &&
        ReadFloatField(state, -1, "translateX",
            parsed.translateX, error) &&
        ReadFloatField(state, -1, "translateY",
            parsed.translateY, error) &&
        ReadFloatField(state, -1, "scale", parsed.scale, error) &&
        ReadFloatField(state, -1, "scaleX", parsed.scaleX, error) &&
        ReadFloatField(state, -1, "scaleY", parsed.scaleY, error) &&
        ReadFloatField(state, -1, "rotate", parsed.rotate, error) &&
        ReadFloatField(state, -1, "skewX", parsed.skewX, error) &&
        ReadFloatField(state, -1, "skewY", parsed.skewY, error) &&
        ReadFloatField(state, -1, "originX", parsed.originX, error) &&
        ReadFloatField(state, -1, "originY", parsed.originY, error);
    lua_pop(state, 1);
    if (!ok) return false;
    transform = parsed;
    return true;
}

bool ReadStringArrayField(lua_State* state, int table, const char* field,
    std::vector<std::string>& values, std::size_t minimum,
    std::size_t maximum, bool required, std::string& error);

bool ReadTransitionField(lua_State* state, int table,
    std::optional<ViewTransition>& transition, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "transition");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = "view field 'transition' must be a table";
        return false;
    }
    const int transitionTable = lua_absindex(state, -1);
    ViewTransition parsed;
    int duration = static_cast<int>(parsed.durationMilliseconds);
    std::vector<std::string> propertyNames;
    bool ok = ValidateObjectFields(state, transitionTable,
            { "durationMs", "easing", "properties" },
            "view transition", error) &&
        ReadIntegerField(state, transitionTable, "durationMs",
            duration, error) &&
        ReadStringArrayField(state, transitionTable, "properties",
            propertyNames, 1, 4, true, error);
    if (ok)
    {
        lua_getfield(state, transitionTable, "easing");
        if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TSTRING)
        {
            error = "view transition easing must be a string";
            ok = false;
        }
        else if (lua_type(state, -1) == LUA_TSTRING)
        {
            const std::string_view easing = lua_tostring(state, -1);
            if (easing == "linear")
                parsed.easing = ViewTransitionEasing::Linear;
            else if (easing == "easeIn")
                parsed.easing = ViewTransitionEasing::EaseIn;
            else if (easing == "easeOut")
                parsed.easing = ViewTransitionEasing::EaseOut;
            else if (easing == "easeInOut")
                parsed.easing = ViewTransitionEasing::EaseInOut;
            else
            {
                error = "view transition easing must be linear, easeIn, easeOut, or easeInOut";
                ok = false;
            }
        }
        lua_pop(state, 1);
    }
    if (ok)
    {
        if (duration < 1 || duration > 2000)
        {
            error = "view transition durationMs must be between 1 and 2000";
            ok = false;
        }
        else
            parsed.durationMilliseconds =
                static_cast<std::uint32_t>(duration);
    }
    if (ok)
    {
        for (const auto& name : propertyNames)
        {
            std::optional<ViewTransitionProperty> property;
            if (name == "background")
                property = ViewTransitionProperty::Background;
            else if (name == "foreground")
                property = ViewTransitionProperty::Foreground;
            else if (name == "borderColor")
                property = ViewTransitionProperty::BorderColor;
            else if (name == "opacity")
                property = ViewTransitionProperty::Opacity;
            else if (name == "transform")
                property = ViewTransitionProperty::Transform;
            else if (name == "layout")
                property = ViewTransitionProperty::Layout;
            else
            {
                error = "view transition properties support background, foreground, borderColor, opacity, transform, and layout";
                ok = false;
                break;
            }
            if (std::find(parsed.properties.begin(), parsed.properties.end(),
                    *property) != parsed.properties.end())
            {
                error = "view transition properties must be unique";
                ok = false;
                break;
            }
            parsed.properties.push_back(*property);
        }
    }
    lua_pop(state, 1);
    if (!ok) return false;
    transition = std::move(parsed);
    return true;
}

bool ReadPresenceTransitionField(lua_State* state, int table,
    const char* field,
    std::optional<ViewPresenceTransition>& transition,
    std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' must be a table";
        return false;
    }
    const int transitionTable = lua_absindex(state, -1);
    ViewPresenceTransition parsed;
    int duration = static_cast<int>(parsed.durationMilliseconds);
    bool ok = ValidateObjectFields(state, transitionTable,
            { "durationMs", "easing", "opacity", "transform" },
            std::string("view ") + field, error) &&
        ReadIntegerField(state, transitionTable, "durationMs",
            duration, error) &&
        ReadOptionalFloat(state, transitionTable, "opacity",
            parsed.opacity, error) &&
        ReadTransformField(state, transitionTable,
            parsed.transform, error);
    if (ok)
    {
        lua_getfield(state, transitionTable, "easing");
        if (!lua_isnil(state, -1) && lua_type(state, -1) != LUA_TSTRING)
        {
            error = std::string("view ") + field +
                " easing must be a string";
            ok = false;
        }
        else if (lua_type(state, -1) == LUA_TSTRING)
        {
            const std::string_view easing = lua_tostring(state, -1);
            if (easing == "linear")
                parsed.easing = ViewTransitionEasing::Linear;
            else if (easing == "easeIn")
                parsed.easing = ViewTransitionEasing::EaseIn;
            else if (easing == "easeOut")
                parsed.easing = ViewTransitionEasing::EaseOut;
            else if (easing == "easeInOut")
                parsed.easing = ViewTransitionEasing::EaseInOut;
            else
            {
                error = std::string("view ") + field +
                    " easing must be linear, easeIn, easeOut, or easeInOut";
                ok = false;
            }
        }
        lua_pop(state, 1);
    }
    if (ok && (duration < 1 || duration > 2000))
    {
        error = std::string("view ") + field +
            " durationMs must be between 1 and 2000";
        ok = false;
    }
    if (ok && !parsed.opacity && !parsed.transform)
    {
        error = std::string("view ") + field +
            " requires opacity or transform";
        ok = false;
    }
    if (ok)
        parsed.durationMilliseconds =
            static_cast<std::uint32_t>(duration);
    lua_pop(state, 1);
    if (!ok) return false;
    transition = std::move(parsed);
    return true;
}

bool ReadStringArrayField(lua_State* state, int table, const char* field,
    std::vector<std::string>& values, std::size_t minimum,
    std::size_t maximum, bool required, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        if (!required) return true;
        error = std::string("view node requires array field '") +
            field + "'";
        return false;
    }
    const std::string context = std::string("view ") + field;
    if (!lua_istable(state, -1) ||
        !ValidateArray(state, -1, context, error))
    {
        if (error.empty()) error = std::string("view field '") + field +
            "' must be an array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t count = lua_rawlen(state, -1);
    if (count < minimum || count > maximum)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' has an invalid item count";
        return false;
    }
    values.clear();
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        if (lua_type(state, -1) != LUA_TSTRING)
        {
            lua_pop(state, 2);
            error = std::string("view field '") + field +
                "' must contain only strings";
            return false;
        }
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        values.emplace_back(text ? text : "", length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool ParseAction(lua_State* state, int index, InteractionAction& action,
    std::string& error);
bool ReadIconFontField(lua_State* state, int table,
    ViewIconFont& value, std::string& error);

bool ReadTextSpansField(lua_State* state, int table,
    std::vector<ViewTextSpan>& spans, std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "spans");
    if (!lua_istable(state, -1) ||
        !ValidateArray(state, -1, "view styledText spans", error))
    {
        if (error.empty()) error = "styledText spans must be an array";
        lua_pop(state, 1);
        return false;
    }
    const std::size_t count = lua_rawlen(state, -1);
    if (count == 0 || count > ViewTreeLimits::MaximumTextSpans)
    {
        lua_pop(state, 1);
        error = "styledText spans must contain 1 to 64 items";
        return false;
    }
    spans.clear();
    spans.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        lua_rawgeti(state, -1, static_cast<lua_Integer>(index + 1));
        if (!lua_istable(state, -1) ||
            !ValidateObjectFields(state, -1,
                { "key", "text", "glyph", "iconFont",
                    "foreground", "hoverForeground",
                    "pressedForeground", "fontSize", "bold", "italic",
                    "underline", "strikethrough", "cursor", "tooltip",
                    "accessibility", "events", "action" },
                "styledText span", error))
        {
            if (error.empty()) error = "styledText spans must be objects";
            lua_pop(state, 2);
            return false;
        }
        ViewTextSpan span;
        const bool hasText = FieldPresent(state, -1, "text");
        const bool hasGlyph = FieldPresent(state, -1, "glyph");
        if (hasText == hasGlyph)
        {
            lua_pop(state, 2);
            error = "styledText spans require exactly one of text or glyph";
            return false;
        }
        if (!hasGlyph && FieldPresent(state, -1, "iconFont"))
        {
            lua_pop(state, 2);
            error = "styledText span iconFont requires glyph";
            return false;
        }
        span.icon = hasGlyph;
        if (!ReadStringField(state, -1, "key", span.key, false, error) ||
            !ReadStringField(state, -1,
                hasGlyph ? "glyph" : "text", span.text, true, error) ||
            !ReadIconFontField(state, -1, span.iconFont, error) ||
            !ReadOptionalColor(state, -1, "foreground",
                span.foreground, error, &span.foregroundToken) ||
            !ReadOptionalColor(state, -1, "hoverForeground",
                span.hoverForeground, error,
                &span.hoverForegroundToken) ||
            !ReadOptionalColor(state, -1, "pressedForeground",
                span.pressedForeground, error,
                &span.pressedForegroundToken) ||
            !ReadOptionalNodeFloatField(state, -1, "fontSize",
                span.fontSize, error) ||
            !ReadBoolField(state, -1, "bold", span.bold, error) ||
            !ReadBoolField(state, -1, "italic", span.italic, error) ||
            !ReadBoolField(state, -1, "underline", span.underline,
                error) ||
            !ReadBoolField(state, -1, "strikethrough",
                span.strikethrough, error) ||
            !ReadStringField(state, -1, "cursor", span.cursor,
                false, error) ||
            !ReadStringField(state, -1, "tooltip", span.tooltip,
                false, error))
        {
            lua_pop(state, 2);
            return false;
        }

        lua_getfield(state, -1, "accessibility");
        if (lua_istable(state, -1))
        {
            if (!ValidateObjectFields(state, -1, { "label" },
                    "styledText span accessibility", error) ||
                !ReadStringField(state, -1, "label",
                    span.accessibilityLabel, false, error))
            {
                lua_pop(state, 3);
                return false;
            }
        }
        else if (!lua_isnil(state, -1))
        {
            lua_pop(state, 3);
            error = "styledText span accessibility must be a table";
            return false;
        }
        lua_pop(state, 1);

        lua_getfield(state, -1, "events");
        if (lua_istable(state, -1))
        {
            const int events = lua_absindex(state, -1);
            if (!ValidateObjectFields(state, events,
                    { "pointerEnter", "pointerLeave", "pointerDown",
                        "pointerMove", "pointerUp", "click",
                        "doubleClick", "wheel", "contextMenu", "keyDown", "keyUp" },
                    "styledText span events", error))
            {
                lua_pop(state, 3);
                return false;
            }
            for (const char* eventName : { "pointerEnter", "pointerLeave",
                "pointerDown", "pointerMove", "pointerUp", "click",
                "doubleClick", "wheel", "contextMenu", "keyDown", "keyUp" })
            {
                lua_getfield(state, events, eventName);
                if (!lua_isnil(state, -1))
                {
                    InteractionAction action;
                    if (!ParseAction(state, -1, action, error))
                    {
                        lua_pop(state, 4);
                        return false;
                    }
                    span.events.emplace(eventName, std::move(action));
                }
                lua_pop(state, 1);
            }
        }
        else if (!lua_isnil(state, -1))
        {
            lua_pop(state, 3);
            error = "styledText span events must be a table";
            return false;
        }
        lua_pop(state, 1);

        lua_getfield(state, -1, "action");
        if (!lua_isnil(state, -1))
        {
            InteractionAction action;
            if (!ParseAction(state, -1, action, error))
            {
                lua_pop(state, 3);
                return false;
            }
            span.events.insert_or_assign("click", std::move(action));
        }
        lua_pop(state, 1);
        spans.push_back(std::move(span));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    return true;
}

bool ReadResourceField(lua_State* state, int table, const char* field,
    LuaResourceType expected, bool required, std::string& name,
    std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        if (!required) return true;
        error = std::string("view field '") + field +
            "' requires a package resource handle";
        return false;
    }
    const auto* handle = TestResourceHandle(state, -1);
    if (!handle || handle->type != expected)
    {
        lua_pop(state, 1);
        error = std::string("view field '") + field +
            "' has the wrong package resource type";
        return false;
    }
    name.assign(handle->name,
        std::char_traits<char>::length(handle->name));
    lua_pop(state, 1);
    if (name.empty() || name.size() > 64)
    {
        error = std::string("view field '") + field +
            "' has an invalid package resource name";
        return false;
    }
    return true;
}

bool ParseAlignment(std::string_view value, ViewAlignment& result)
{
    if (value == "start") result = ViewAlignment::Start;
    else if (value == "center") result = ViewAlignment::Center;
    else if (value == "end") result = ViewAlignment::End;
    else if (value == "stretch") result = ViewAlignment::Stretch;
    else return false;
    return true;
}

bool ReadAlignmentField(lua_State* state, int table, const char* field,
    ViewAlignment& value, bool allowAuto, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, field, text, false, error))
        return false;
    if (text.empty()) return true;
    if (allowAuto && text == "auto")
        value = ViewAlignment::Auto;
    else if (!ParseAlignment(text, value))
    {
        error = std::string("view field '") + field +
            "' has an unsupported alignment";
        return false;
    }
    return true;
}

bool ReadJustificationField(lua_State* state, int table,
    ViewJustification& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "justifyContent", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "start") value = ViewJustification::Start;
    else if (text == "center") value = ViewJustification::Center;
    else if (text == "end") value = ViewJustification::End;
    else if (text == "spaceBetween")
        value = ViewJustification::SpaceBetween;
    else if (text == "spaceAround")
        value = ViewJustification::SpaceAround;
    else if (text == "spaceEvenly")
        value = ViewJustification::SpaceEvenly;
    else
    {
        error = "view field 'justifyContent' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadFlexDirectionField(lua_State* state, int table,
    ViewFlexDirection& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "flexDirection", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "row") value = ViewFlexDirection::Row;
    else if (text == "rowReverse")
        value = ViewFlexDirection::RowReverse;
    else if (text == "column") value = ViewFlexDirection::Column;
    else if (text == "columnReverse")
        value = ViewFlexDirection::ColumnReverse;
    else
    {
        error = "view field 'flexDirection' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadFlexWrapField(lua_State* state, int table,
    ViewFlexWrap& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "flexWrap", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "noWrap") value = ViewFlexWrap::NoWrap;
    else if (text == "wrap") value = ViewFlexWrap::Wrap;
    else if (text == "wrapReverse") value = ViewFlexWrap::WrapReverse;
    else
    {
        error = "view field 'flexWrap' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadContentAlignmentField(lua_State* state, int table,
    ViewContentAlignment& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "alignContent", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "start") value = ViewContentAlignment::Start;
    else if (text == "center") value = ViewContentAlignment::Center;
    else if (text == "end") value = ViewContentAlignment::End;
    else if (text == "stretch") value = ViewContentAlignment::Stretch;
    else if (text == "spaceBetween")
        value = ViewContentAlignment::SpaceBetween;
    else if (text == "spaceAround")
        value = ViewContentAlignment::SpaceAround;
    else if (text == "spaceEvenly")
        value = ViewContentAlignment::SpaceEvenly;
    else
    {
        error = "view field 'alignContent' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadTextAlignmentField(lua_State* state, int table,
    ViewTextAlignment& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "textAlign", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "start") value = ViewTextAlignment::Start;
    else if (text == "center") value = ViewTextAlignment::Center;
    else if (text == "end") value = ViewTextAlignment::End;
    else
    {
        error = "view field 'textAlign' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadTextVerticalAlignmentField(lua_State* state, int table,
    ViewAlignment& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "verticalAlign", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "center") value = ViewAlignment::Center;
    else if (text == "start") value = ViewAlignment::Start;
    else if (text == "end") value = ViewAlignment::End;
    else
    {
        error = "view field 'verticalAlign' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadTextWrapField(lua_State* state, int table,
    ViewTextWrap& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "textWrap", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "noWrap") value = ViewTextWrap::NoWrap;
    else if (text == "wrap") value = ViewTextWrap::Wrap;
    else
    {
        error = "view field 'textWrap' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadTextOverflowField(lua_State* state, int table,
    ViewTextOverflow& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "overflowText", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "ellipsis")
        value = ViewTextOverflow::Ellipsis;
    else if (text == "clip") value = ViewTextOverflow::Clip;
    else
    {
        error = "view field 'overflowText' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadFontStyleField(lua_State* state, int table,
    ViewFontStyle& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "fontStyle", text, false, error))
        return false;
    if (text.empty()) return true;
    if (text == "normal") value = ViewFontStyle::Normal;
    else if (text == "italic") value = ViewFontStyle::Italic;
    else
    {
        error = "view field 'fontStyle' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadTextDirectionField(lua_State* state, int table,
    ViewTextDirection& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "textDirection", text, false, error))
        return false;
    if (text.empty() || text == "auto") value = ViewTextDirection::Auto;
    else if (text == "ltr") value = ViewTextDirection::LeftToRight;
    else if (text == "rtl") value = ViewTextDirection::RightToLeft;
    else
    {
        error = "view textDirection must be auto, ltr, or rtl";
        return false;
    }
    return true;
}

bool ReadValidationStateField(lua_State* state, int table,
    ViewValidationState& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "validationState", text, false, error))
        return false;
    if (text.empty() || text == "none") value = ViewValidationState::None;
    else if (text == "info") value = ViewValidationState::Info;
    else if (text == "success") value = ViewValidationState::Success;
    else if (text == "warning") value = ViewValidationState::Warning;
    else if (text == "error") value = ViewValidationState::Error;
    else
    {
        error = "view field 'validationState' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadSelectionModeField(lua_State* state, int table,
    ViewSelectionMode& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "selectionMode", text, false, error))
        return false;
    if (text.empty() || text == "none") value = ViewSelectionMode::None;
    else if (text == "single") value = ViewSelectionMode::Single;
    else if (text == "multiple") value = ViewSelectionMode::Multiple;
    else
    {
        error = "view field 'selectionMode' must be none, single, or multiple";
        return false;
    }
    return true;
}

bool ReadTextSelectionField(lua_State* state, int table,
    std::string_view value, std::optional<ViewTextSelection>& selection,
    std::string& error)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, "selection");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1) ||
        !ValidateObjectFields(state, -1, { "start", "finish" },
            "view text selection", error))
    {
        if (error.empty())
            error = "view field 'selection' must be a { start, finish } table";
        lua_pop(state, 1);
        return false;
    }
    ViewTextSelection parsed;
    if (!ReadNonNegativeSizeField(state, -1, "start",
            parsed.start, true, error) ||
        !ReadNonNegativeSizeField(state, -1, "finish",
            parsed.finish, true, error))
    {
        lua_pop(state, 1);
        return false;
    }
    lua_pop(state, 1);
    const auto boundary = [value](std::size_t offset) {
        return offset <= value.size() &&
            (offset == value.size() ||
                (static_cast<unsigned char>(value[offset]) & 0xC0u) !=
                    0x80u);
    };
    if (parsed.start > parsed.finish || !boundary(parsed.start) ||
        !boundary(parsed.finish))
    {
        error = "view text selection must use ordered UTF-8 byte boundaries within value";
        return false;
    }
    selection = parsed;
    return true;
}

bool ReadVisibilityField(lua_State* state, int table,
    ViewVisibility& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "visibility", text, false, error))
        return false;
    if (text.empty() || text == "visible") value = ViewVisibility::Visible;
    else if (text == "hidden") value = ViewVisibility::Hidden;
    else if (text == "collapsed") value = ViewVisibility::Collapsed;
    else
    {
        error = "view field 'visibility' must be visible, hidden, or collapsed";
        return false;
    }
    return true;
}

bool ReadImageFitField(lua_State* state, int table,
    ViewImageFit& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "fit", text, false, error))
        return false;
    if (text.empty() || text == "contain") value = ViewImageFit::Contain;
    else if (text == "fill") value = ViewImageFit::Fill;
    else if (text == "cover") value = ViewImageFit::Cover;
    else if (text == "none") value = ViewImageFit::None;
    else
    {
        error = "view field 'fit' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadImageAlignmentField(lua_State* state, int table,
    ViewImageAlignment& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "alignment", text, false, error))
        return false;
    if (text.empty() || text == "center")
        value = ViewImageAlignment::Center;
    else if (text == "start") value = ViewImageAlignment::Start;
    else if (text == "end") value = ViewImageAlignment::End;
    else
    {
        error = "view field 'alignment' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadImageInterpolationField(lua_State* state, int table,
    ViewImageInterpolation& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "interpolation", text, false,
            error))
        return false;
    if (text.empty() || text == "linear")
        value = ViewImageInterpolation::Linear;
    else if (text == "nearest")
        value = ViewImageInterpolation::Nearest;
    else
    {
        error = "view field 'interpolation' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadOverflowField(lua_State* state, int table,
    ViewOverflow& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "overflow", text, false, error))
        return false;
    if (text.empty() || text == "visible") value = ViewOverflow::Visible;
    else if (text == "clip") value = ViewOverflow::Clip;
    else
    {
        error = "view field 'overflow' must be visible or clip";
        return false;
    }
    return true;
}

bool ReadOrientationField(lua_State* state, int table,
    ViewOrientation& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "orientation", text, false, error))
        return false;
    if (text.empty() || text == "horizontal")
        value = ViewOrientation::Horizontal;
    else if (text == "vertical")
        value = ViewOrientation::Vertical;
    else
    {
        error = "view field 'orientation' must be horizontal or vertical";
        return false;
    }
    return true;
}

bool ParseAction(lua_State* state, int index, InteractionAction& action,
    std::string& error)
{
    index = lua_absindex(state, index);
    if (!lua_istable(state, index))
    {
        error = "view actions must be tables";
        return false;
    }
    if (!ValidateObjectFields(state, index, { "id", "value", "scope" },
            "view action", error) ||
        !ReadStringField(state, index, "id", action.id, true, error))
        return false;
    std::string scope;
    if (!ReadStringField(state, index, "scope", scope, false, error))
        return false;
    if (!scope.empty())
    {
        if (scope == "element")
            action.contextMenuScope =
                InteractionAction::ContextMenuScope::Element;
        else if (scope == "component")
            action.contextMenuScope =
                InteractionAction::ContextMenuScope::Component;
        else
        {
            error = "view action scope must be element or component";
            return false;
        }
    }
    lua_getfield(state, index, "value");
    std::size_t nodes = 0;
    std::size_t stringBytes = 0;
    std::unordered_set<const void*> ancestors;
    const bool ok = ReadActionValue(state, -1, action.value, 0,
        nodes, stringBytes, ancestors, error);
    lua_pop(state, 1);
    return ok;
}

bool ParseNodeType(std::string_view type, ViewNodeType& result)
{
    const auto found = FindViewNodeType(type);
    if (!found) return false;
    result = *found;
    return true;
}

bool ReadShapeKindField(lua_State* state, int table,
    ViewShapeKind& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "shape", text, false, error))
        return false;
    if (text.empty() || text == "rectangle")
        value = ViewShapeKind::Rectangle;
    else if (text == "roundedRectangle")
        value = ViewShapeKind::RoundedRectangle;
    else if (text == "circle") value = ViewShapeKind::Circle;
    else if (text == "ellipse") value = ViewShapeKind::Ellipse;
    else
    {
        error = "view field 'shape' has an unsupported value";
        return false;
    }
    return true;
}

bool ReadIconFontField(lua_State* state, int table,
    ViewIconFont& value, std::string& error)
{
    std::string text;
    if (!ReadStringField(state, table, "iconFont", text, false, error))
        return false;
    if (text.empty() || text == "fa")
        value = ViewIconFont::FontAwesome;
    else if (text == "fluent" || text == "fluent-regular")
        value = ViewIconFont::Fluent;
    else
    {
        error = "view field 'iconFont' has an unsupported value";
        return false;
    }
    return true;
}

bool ParseNode(lua_State* state, int index, ViewNode& node,
    std::size_t depth, std::size_t& parsedNodes, std::string& error)
{
    index = lua_absindex(state, index);
    if (!lua_istable(state, index))
    {
        error = "view nodes must be tables";
        return false;
    }
    if (++parsedNodes > ViewTreeLimits::MaximumNodes ||
        depth > ViewTreeLimits::MaximumDepth)
    {
        error = "view tree exceeds its node or depth limit";
        return false;
    }
    std::string type;
    if (!ReadStringField(state, index, "type", type, true, error) ||
        !ParseNodeType(type, node.type))
    {
        if (error.empty()) error = "unsupported view node type: " + type;
        return false;
    }
    const bool buttonNode = node.type == ViewNodeType::Button ||
        node.type == ViewNodeType::IconButton;
    const bool checkControlNode = node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox;
    const bool radioNode = node.type == ViewNodeType::RadioGroup;
    const bool selectNode = node.type == ViewNodeType::Select;
    const bool textInputNode = node.type == ViewNodeType::TextInput ||
        node.type == ViewNodeType::TextArea ||
        node.type == ViewNodeType::SearchBox;
    const bool numberInputNode = node.type == ViewNodeType::NumberInput;
    const bool inputNode = textInputNode || numberInputNode;
    const bool choiceNode = radioNode || selectNode;
    const bool sliderNode = node.type == ViewNodeType::Slider;
    const bool scrollNode = node.type == ViewNodeType::Scroll;
    const bool virtualListNode =
        node.type == ViewNodeType::VirtualList;
    const bool virtualGridNode =
        node.type == ViewNodeType::VirtualGrid;
    const bool virtualCollectionNode = virtualListNode || virtualGridNode;
    const bool collectionNode = node.type == ViewNodeType::List ||
        node.type == ViewNodeType::GridList || virtualCollectionNode;
    const bool scrollContainerNode = scrollNode || virtualCollectionNode;
    const bool listItemNode = node.type == ViewNodeType::ListItem;
    const bool styledTextNode = node.type == ViewNodeType::StyledText;
    const bool monthCalendarNode =
        node.type == ViewNodeType::MonthCalendar;
    const bool slotSurfaceNode = node.type == ViewNodeType::SlotSurface;
    const bool slotItemNode = node.type == ViewNodeType::SlotItem;
    const bool controlNode = checkControlNode || choiceNode || sliderNode ||
        inputNode || monthCalendarNode;
    const bool linkNode = node.type == ViewNodeType::Link;
    const bool labelNode = node.type == ViewNodeType::Button ||
        linkNode || checkControlNode;
    const bool actionNode = buttonNode || linkNode || controlNode ||
        listItemNode || slotItemNode;
    const bool iconNode = node.type == ViewNodeType::Icon ||
        node.type == ViewNodeType::IconButton;
    const bool textNode = node.type == ViewNodeType::Text ||
        styledTextNode ||
        node.type == ViewNodeType::Badge;
    const bool progressNode = node.type == ViewNodeType::ProgressBar ||
        node.type == ViewNodeType::ProgressRing ||
        node.type == ViewNodeType::Meter;
    const bool seriesNode = node.type == ViewNodeType::Sparkline ||
        node.type == ViewNodeType::LineChart ||
        node.type == ViewNodeType::BarChart ||
        node.type == ViewNodeType::Waveform ||
        node.type == ViewNodeType::Spectrum;
    const bool imageNode = node.type == ViewNodeType::Image;
    const bool referenceIconNode =
        node.type == ViewNodeType::ReferenceIcon;
    const bool imageVisualNode = imageNode || referenceIconNode;
    const bool dividerNode = node.type == ViewNodeType::Divider;
    const bool gridNode = node.type == ViewNodeType::Grid ||
        node.type == ViewNodeType::GridList || virtualGridNode;
    const bool positionedGridNode = node.type == ViewNodeType::Grid ||
        node.type == ViewNodeType::GridList;
    const bool flowNode = node.type == ViewNodeType::Flow;
    const bool flexContainerNode = node.type == ViewNodeType::Row ||
        node.type == ViewNodeType::Column;
    const bool textResourceNode = textNode || labelNode || radioNode ||
        monthCalendarNode;
    if (labelNode &&
        (FieldPresent(state, index, "text") ||
            FieldPresent(state, index, "glyph")))
    {
        error = "button, link, toggle, and checkbox nodes use 'label', not 'text' or 'glyph'";
        return false;
    }
    if (iconNode && (FieldPresent(state, index, "text") ||
            FieldPresent(state, index, "label")))
    {
        error = "icon nodes use 'glyph', not 'text' or 'label'";
        return false;
    }
    if (!labelNode && FieldPresent(state, index, "label"))
    {
        error = "only label-bearing action controls accept 'label'";
        return false;
    }
    if (!actionNode && FieldPresent(state, index, "action"))
    {
        error = "only action-capable nodes accept 'action'";
        return false;
    }
    if (!listItemNode && FieldPresent(state, index, "sticky"))
    {
        error = "only listItem nodes accept sticky";
        return false;
    }
    if (!styledTextNode && FieldPresent(state, index, "spans"))
    {
        error = "only styledText nodes accept spans";
        return false;
    }
    if (styledTextNode && FieldPresent(state, index, "text"))
    {
        error = "styledText nodes use spans, not text";
        return false;
    }
    if (!textNode && !labelNode && !iconNode &&
        FieldPresent(state, index, "text"))
    {
        error = "only text nodes accept 'text'";
        return false;
    }
    if (!iconNode && (FieldPresent(state, index, "glyph") ||
            FieldPresent(state, index, "iconFont")))
    {
        error = "only icon nodes accept 'glyph' and 'iconFont'";
        return false;
    }
    if (node.type != ViewNodeType::Shape &&
        FieldPresent(state, index, "shape"))
    {
        error = "only shape nodes accept 'shape'";
        return false;
    }
    if (!dividerNode && !radioNode && !sliderNode && !scrollNode &&
        node.type != ViewNodeType::List &&
        FieldPresent(state, index, "orientation"))
    {
        error = "only divider, radioGroup, slider, scroll, and list nodes accept orientation";
        return false;
    }
    if (!gridNode && FieldPresent(state, index, "columns"))
    {
        error = "only grid, gridList, and virtualGrid nodes accept columns";
        return false;
    }
    if (!positionedGridNode && FieldPresent(state, index, "rows"))
    {
        error = "only grid and gridList nodes accept rows";
        return false;
    }
    if (!gridNode && !flowNode && !virtualListNode &&
        (FieldPresent(state, index, "columnGap") ||
            FieldPresent(state, index, "rowGap")))
    {
        error = "only grid, collection-grid, flow, and virtualList nodes accept axis gaps";
        return false;
    }
    if (virtualListNode && FieldPresent(state, index, "columnGap"))
    {
        error = "virtualList nodes accept rowGap but reject columnGap";
        return false;
    }
    if (!flexContainerNode &&
        (FieldPresent(state, index, "flexDirection") ||
            FieldPresent(state, index, "flexWrap") ||
            FieldPresent(state, index, "alignContent")))
    {
        error = "flexDirection, flexWrap, and alignContent are reserved for row and column nodes";
        return false;
    }
    if (gridNode && !FieldPresent(state, index, "columns"))
    {
        error = "grid, gridList, and virtualGrid nodes require columns";
        return false;
    }
    if (!scrollContainerNode && FieldPresent(state, index, "showScrollbar"))
    {
        error = "only scroll and virtual collection nodes accept showScrollbar";
        return false;
    }
    if (!scrollNode && FieldPresent(state, index, "initialScrollKey"))
    {
        error = "only scroll nodes accept initialScrollKey";
        return false;
    }
    if (!virtualCollectionNode &&
        FieldPresent(state, index, "initialScrollIndex"))
    {
        error = "only virtual collection nodes accept initialScrollIndex";
        return false;
    }
    if (!virtualCollectionNode &&
        (FieldPresent(state, index, "itemCount") ||
            FieldPresent(state, index, "itemExtent") ||
            FieldPresent(state, index, "estimatedItemSize") ||
            FieldPresent(state, index, "layoutRevision") ||
            FieldPresent(state, index, "sectionHeaderIndices") ||
            FieldPresent(state, index, "stickyHeaderIndex") ||
            FieldPresent(state, index, "firstIndex") ||
            FieldPresent(state, index, "overscan")))
    {
        error = "virtual collection fields are reserved for virtualList and virtualGrid";
        return false;
    }
    const bool itemExtentSpecified =
        FieldPresent(state, index, "itemExtent");
    const bool estimatedItemSizeSpecified =
        FieldPresent(state, index, "estimatedItemSize");
    if (virtualCollectionNode &&
        (!FieldPresent(state, index, "itemCount") ||
            !FieldPresent(state, index, "firstIndex")))
    {
        error = "virtual collection nodes require itemCount and firstIndex";
        return false;
    }
    if ((virtualGridNode && (!itemExtentSpecified ||
            estimatedItemSizeSpecified)) ||
        (virtualListNode &&
            itemExtentSpecified == estimatedItemSizeSpecified))
    {
        error = "virtualList requires exactly one of itemExtent or estimatedItemSize; virtualGrid requires itemExtent";
        return false;
    }
    if ((!virtualListNode || !estimatedItemSizeSpecified) &&
        FieldPresent(state, index, "layoutRevision"))
    {
        error = "layoutRevision is reserved for variable virtualList nodes";
        return false;
    }
    if (!virtualListNode &&
        (FieldPresent(state, index, "sectionHeaderIndices") ||
            FieldPresent(state, index, "stickyHeaderIndex")))
    {
        error = "virtual section headers are reserved for virtualList";
        return false;
    }
    if (!progressNode && !sliderNode && !inputNode &&
        FieldPresent(state, index, "value"))
    {
        error = "only progress, slider, and input nodes accept 'value'";
        return false;
    }
    if (!checkControlNode && FieldPresent(state, index, "checked"))
    {
        error = "only toggle and checkbox nodes accept checked";
        return false;
    }
    if (!checkControlNode && !choiceNode &&
        FieldPresent(state, index, "checkedStyle"))
    {
        error = "only selection controls accept checkedStyle";
        return false;
    }
    if (!monthCalendarNode &&
        (FieldPresent(state, index, "year") ||
            FieldPresent(state, index, "month") ||
            FieldPresent(state, index, "firstDayOfWeek") ||
            FieldPresent(state, index, "selectedDate") ||
            FieldPresent(state, index, "todayDate") ||
            FieldPresent(state, index, "eventDates") ||
            FieldPresent(state, index, "weekdayLabels") ||
            FieldPresent(state, index, "showAdjacentDates") ||
            FieldPresent(state, index, "todayStyle") ||
            FieldPresent(state, index, "adjacentStyle") ||
            FieldPresent(state, index, "eventStyle")))
    {
        error = "calendar fields and styles are reserved for monthCalendar";
        return false;
    }
    if (monthCalendarNode &&
        (!FieldPresent(state, index, "year") ||
            !FieldPresent(state, index, "month") ||
            !FieldPresent(state, index, "selectedDate") ||
            !FieldPresent(state, index, "weekdayLabels")))
    {
        error = "monthCalendar requires year, month, selectedDate, and weekdayLabels";
        return false;
    }
    if (!slotSurfaceNode &&
        (FieldPresent(state, index, "binding") ||
            FieldPresent(state, index, "collection") ||
            FieldPresent(state, index, "revision")))
    {
        error = "binding, collection, and revision are reserved for slotSurface";
        return false;
    }
    if (!slotSurfaceNode && !slotItemNode &&
        FieldPresent(state, index, "child"))
    {
        error = "child is reserved for slotSurface and slotItem";
        return false;
    }
    if (!slotItemNode && !referenceIconNode &&
        FieldPresent(state, index, "reference"))
    {
        error = "reference is reserved for slotItem and referenceIcon";
        return false;
    }
    if (slotSurfaceNode)
    {
        const bool hasBinding = FieldPresent(state, index, "binding");
        const bool hasCollection = FieldPresent(state, index, "collection");
        if (hasBinding == hasCollection)
        {
            error = "slotSurface requires exactly one of binding or collection";
            return false;
        }
        if (FieldPresent(state, index, "child") &&
            FieldPresent(state, index, "children"))
        {
            error = "slotSurface cannot provide both child and children";
            return false;
        }
    }
    if (slotItemNode && FieldPresent(state, index, "child") &&
        FieldPresent(state, index, "children"))
    {
        error = "slotItem cannot provide both child and children";
        return false;
    }
    if (!seriesNode && FieldPresent(state, index, "values"))
    {
        error = "only data-series nodes accept values";
        return false;
    }
    if (!seriesNode && !sliderNode && !numberInputNode &&
        (FieldPresent(state, index, "min") ||
            FieldPresent(state, index, "max")))
    {
        error = "only data-series, slider, and numberInput nodes accept values, min, and max";
        return false;
    }
    if (!sliderNode && !numberInputNode && FieldPresent(state, index, "step"))
    {
        error = "only slider and numberInput nodes accept step";
        return false;
    }
    if (!choiceNode && (FieldPresent(state, index, "options") ||
            FieldPresent(state, index, "selectedValue")))
    {
        error = "only radioGroup and select nodes accept options and selectedValue";
        return false;
    }
    if (!inputNode && (FieldPresent(state, index, "selectAll") ||
            FieldPresent(state, index, "liveUpdate") ||
            FieldPresent(state, index, "maxBytes")))
    {
        error = "input fields are reserved for textInput, textArea, searchBox, and numberInput";
        return false;
    }
    if (!inputNode && !selectNode &&
        FieldPresent(state, index, "placeholder"))
    {
        error = "placeholder is reserved for input and select nodes";
        return false;
    }
    if (!selectNode && FieldPresent(state, index, "expanded"))
    {
        error = "expanded is reserved for select nodes";
        return false;
    }
    if (!progressNode && !seriesNode && !dividerNode && (
            FieldPresent(state, index, "thickness") ||
            FieldPresent(state, index, "trackOpacity") ||
            FieldPresent(state, index, "fillOpacity")))
    {
        error = "only progress and data-series nodes accept drawing fields";
        return false;
    }
    if (!imageVisualNode && (FieldPresent(state, index, "fit") ||
            FieldPresent(state, index, "alignment") ||
            FieldPresent(state, index, "interpolation") ||
            FieldPresent(state, index, "alt")))
    {
        error = "only image and referenceIcon nodes accept image visual fields";
        return false;
    }
    if (!imageNode && FieldPresent(state, index, "source"))
    {
        error = "only image nodes accept an image resource source";
        return false;
    }
    if (!textResourceNode && FieldPresent(state, index, "font"))
    {
        error = "only text and label-bearing nodes accept a font resource";
        return false;
    }
    if (imageVisualNode && !FieldPresent(state, index, "alt"))
    {
        error = "image and referenceIcon nodes require an explicit 'alt' field";
        return false;
    }
    if (checkControlNode && !FieldPresent(state, index, "checked"))
    {
        error = "toggle and checkbox nodes require checked";
        return false;
    }
    if ((sliderNode || inputNode) &&
        !FieldPresent(state, index, "value"))
    {
        error = "slider and input nodes require value";
        return false;
    }
    if (choiceNode && !FieldPresent(state, index, "selectedValue"))
    {
        error = "radioGroup and select nodes require selectedValue";
        return false;
    }
    if (!ValidateNodeFields(state, index, node.type, error)) return false;
    if (!ReadStringField(state, index, "key", node.key, true, error))
        return false;
    if (slotSurfaceNode)
    {
        const bool binding = FieldPresent(state, index, "binding");
        node.logicalSlotKind = binding
            ? LogicalSlotKind::Binding : LogicalSlotKind::Collection;
        std::size_t revision = 0;
        if (!ReadStringField(state, index,
                binding ? "binding" : "collection",
                node.logicalSlotId, true, error) ||
            !ReadNonNegativeSizeField(state, index, "revision",
                revision, false, error))
            return false;
        node.logicalSlotRevision = static_cast<std::uint64_t>(revision);
    }
    if (slotItemNode &&
        !ReadStringField(state, index, "reference",
            node.logicalSlotReference, true, error))
        return false;
    if (referenceIconNode &&
        !ReadStringField(state, index, "reference",
            node.itemReference, true, error))
        return false;
    if (node.type == ViewNodeType::Badge &&
        !FieldPresent(state, index, "padding"))
        node.padding = 4.0f;
    if (radioNode && !FieldPresent(state, index, "gap"))
        node.gap = 8.0f;
    if (inputNode && !FieldPresent(state, index, "padding"))
        node.padding = 8.0f;
    if (styledTextNode && !FieldPresent(state, index, "textWrap"))
        node.textWrap = ViewTextWrap::Wrap;
    if (styledTextNode && !FieldPresent(state, index, "overflowText"))
        node.overflowText = ViewTextOverflow::Clip;
    const char* contentField = iconNode ? "glyph" :
        (labelNode ? "label" : "text");
    const bool clipSpecified = FieldPresent(state, index, "clip");
    const bool overflowSpecified = FieldPresent(state, index, "overflow");
    const bool visibleSpecified = FieldPresent(state, index, "visible");
    const bool visibilitySpecified = FieldPresent(state, index, "visibility");
    const bool focusableSpecified = FieldPresent(state, index, "focusable");
    const bool tabIndexSpecified = FieldPresent(state, index, "tabIndex");
    bool focusable = false;
    int tabIndex = 0;
    std::size_t rowTrackCount = 0;
    std::size_t virtualLayoutRevision = 0;
    const bool initialScrollKeySpecified =
        FieldPresent(state, index, "initialScrollKey");
    std::string initialScrollKey;
    if (!ReadStringField(state, index, contentField,
            node.text, false, error) ||
        !ReadResourceField(state, index, "source", LuaResourceType::Image,
            imageNode, node.imageResourceName, error) ||
        !ReadResourceField(state, index, "font", LuaResourceType::Font,
            false, node.fontResourceName, error) ||
        !ReadStringField(state, index, "alt", node.alt, false, error) ||
        !ReadImageFitField(state, index, node.imageFit, error) ||
        !ReadImageAlignmentField(state, index,
            node.imageAlignment, error) ||
        !ReadImageInterpolationField(state, index,
            node.imageInterpolation, error) ||
        !ReadOptionalColor(state, index, "tint", node.imageTint, error,
            &node.imageTintToken) ||
        !ReadOrientationField(state, index, node.orientation, error) ||
        !ReadLengthField(state, index, "width", node.width, error) ||
        !ReadLengthField(state, index, "height", node.height, error) ||
        !ReadOptionalNodeFloatField(state, index, "minWidth",
            node.minimumWidth, error) ||
        !ReadOptionalNodeFloatField(state, index, "maxWidth",
            node.maximumWidth, error) ||
        !ReadOptionalNodeFloatField(state, index, "minHeight",
            node.minimumHeight, error) ||
        !ReadOptionalNodeFloatField(state, index, "maxHeight",
            node.maximumHeight, error) ||
        !ReadOptionalNodeFloatField(state, index, "aspectRatio",
            node.aspectRatio, error) ||
        !ReadEdgeInsetsField(state, index, "margin", node.margin, error) ||
        !ReadEdgeInsetsField(state, index, "padding", node.padding, error) ||
        !ReadOffsetField(state, index, node.offsetX, node.offsetY, error) ||
        !ReadIntegerField(state, index, "zIndex", node.zIndex, error) ||
        !ReadBoolField(state, index, "clip", node.clipChildren, error) ||
        !ReadOverflowField(state, index, node.overflow, error) ||
        !ReadShadowField(state, index, node.shadow, error) ||
        !ReadTransformField(state, index, node.transform, error) ||
        !ReadTransitionField(state, index, node.transition, error) ||
        !ReadPresenceTransitionField(state, index, "enterTransition",
            node.enterTransition, error) ||
        !ReadPresenceTransitionField(state, index, "exitTransition",
            node.exitTransition, error) ||
        !ReadFloatField(state, index, "gap", node.gap, error) ||
        !ReadGridTracksField(state, index, "columns",
            positionedGridNode, node.columns, node.columnTracks, error) ||
        !ReadGridTracksField(state, index, "rows",
            positionedGridNode, rowTrackCount, node.rowTracks, error) ||
        !ReadNonNegativeSizeField(state, index, "itemCount",
            node.itemCount, virtualCollectionNode, error) ||
        !ReadFloatField(state, index, "itemExtent",
            node.itemExtent, error) ||
        !ReadOptionalNodeFloatField(state, index, "estimatedItemSize",
            node.estimatedItemSize, error) ||
        !ReadNonNegativeSizeField(state, index, "layoutRevision",
            virtualLayoutRevision, false, error) ||
        !ReadPositiveSizeArrayField(state, index,
            "sectionHeaderIndices", node.sectionHeaderIndices,
            ViewTreeLimits::MaximumVirtualSectionHeaders, error) ||
        !ReadOptionalPositiveSizeField(state, index,
            "stickyHeaderIndex", node.stickyHeaderIndex, error) ||
        !ReadNonNegativeSizeField(state, index, "firstIndex",
            node.firstIndex, virtualCollectionNode, error) ||
        !ReadNonNegativeSizeField(state, index, "overscan",
            node.overscan, false, error) ||
        !ReadStringField(state, index, "initialScrollKey",
            initialScrollKey, false, error) ||
        !ReadOptionalPositiveSizeField(state, index,
            "initialScrollIndex", node.initialScrollIndex, error) ||
        !ReadSelectionModeField(state, index,
            node.selectionMode, error) ||
        !ReadStringArrayField(state, index, "selectedKeys",
            node.selectedKeys, 0,
            ViewTreeLimits::MaximumCollectionItems, false, error) ||
        !ReadOptionalNodeFloatField(state, index, "columnGap",
            node.columnGap, error) ||
        !ReadOptionalNodeFloatField(state, index, "rowGap",
            node.rowGap, error) ||
        !ReadOptionalPositiveSizeField(state, index, "gridColumn",
            node.gridColumn, error) ||
        !ReadOptionalPositiveSizeField(state, index, "gridRow",
            node.gridRow, error) ||
        !ReadSizeField(state, index, "columnSpan",
            node.columnSpan, error) ||
        !ReadSizeField(state, index, "rowSpan", node.rowSpan, error) ||
        !ReadLengthField(state, index, "flexBasis",
            node.flexBasis, error) ||
        !ReadFloatField(state, index, "flexGrow", node.flexGrow, error) ||
        !ReadFloatField(state, index, "flexShrink",
            node.flexShrink, error) ||
        !ReadFlexDirectionField(state, index,
            node.flexDirection, error) ||
        !ReadFlexWrapField(state, index, node.flexWrap, error) ||
        !ReadContentAlignmentField(state, index,
            node.alignContent, error) ||
        !ReadFloatField(state, index, "fontSize", node.fontSize, error) ||
        !ReadNonNegativeSizeField(state, index, "fontWeight",
            node.fontWeight, false, error) ||
        !ReadFontStyleField(state, index, node.fontStyle, error) ||
        !ReadStringField(state, index, "locale", node.locale, false, error) ||
        !ReadTextDirectionField(state, index,
            node.textDirection, error) ||
        !ReadOptionalNodeFloatField(state, index, "lineHeight",
            node.lineHeight, error) ||
        !ReadFloatField(state, index, "letterSpacing",
            node.letterSpacing, error) ||
        !ReadFloatField(state, index, "thickness", node.thickness, error) ||
        !ReadFloatField(state, index, "trackOpacity",
            node.trackOpacity, error) ||
        !ReadFloatField(state, index, "fillOpacity",
            node.fillOpacity, error) ||
        !ReadBoolField(state, index, "bold", node.bold, error) ||
        !ReadBoolField(state, index, "checked", node.checked, error) ||
        !ReadBoolField(state, index, "indeterminate",
            node.indeterminate, error) ||
        !ReadBoolField(state, index, "selected", node.selected, error) ||
        !ReadBoolField(state, index, "sticky", node.sticky, error) ||
        !ReadBoolField(state, index, "expanded", node.expanded, error) ||
        !ReadBoolField(state, index, "selectAll", node.selectAll, error) ||
        !ReadBoolField(state, index, "liveUpdate", node.liveUpdate, error) ||
        !ReadBoolField(state, index, "readOnly", node.readOnly, error) ||
        !ReadBoolField(state, index, "required", node.required, error) ||
        !ReadBoolField(state, index, "busy", node.busy, error) ||
        !ReadValidationStateField(state, index,
            node.validationState, error) ||
        !ReadStringField(state, index, "validationMessage",
            node.validationMessage, false, error) ||
        !ReadBoolField(state, index, "showScrollbar",
            node.showScrollbar, error) ||
        !ReadBoolField(state, index, "showAdjacentDates",
            node.showAdjacentDates, error) ||
        !ReadBoolField(state, index, "visible", node.visible, error) ||
        !ReadVisibilityField(state, index, node.visibility, error) ||
        !ReadBoolField(state, index, "enabled", node.enabled, error) ||
        !ReadBoolField(state, index, "focusable", focusable, error) ||
        !ReadIntegerField(state, index, "tabIndex", tabIndex, error) ||
        !ReadStringField(state, index, "cursor", node.cursor, false, error) ||
        !ReadStringField(state, index, "tooltip",
            node.tooltip, false, error) ||
        !ReadStringField(state, index, "accessKey",
            node.accessKey, false, error) ||
        !ReadStringField(state, index, "acceleratorText",
            node.acceleratorText, false, error) ||
        !ReadAlignmentField(state, index, "alignItems",
            node.alignItems, false, error) ||
        !ReadAlignmentField(state, index, "alignSelf",
            node.alignSelf, true, error) ||
        !ReadJustificationField(state, index,
            node.justifyContent, error) ||
        !ReadTextAlignmentField(state, index, node.textAlign, error) ||
        !ReadTextVerticalAlignmentField(state, index,
            node.verticalAlign, error) ||
        !ReadTextWrapField(state, index, node.textWrap, error) ||
        !ReadNonNegativeSizeField(state, index, "maxLines",
            node.maximumLines, false, error) ||
        !ReadTextOverflowField(state, index,
            node.overflowText, error) ||
        !ReadShapeKindField(state, index, node.shapeKind, error) ||
        !ReadIconFontField(state, index, node.iconFont, error) ||
        !ReadStyleField(state, index, "style", node.style, error) ||
        !ReadStyleField(state, index, "hoverStyle",
            node.hoverStyle, error) ||
        !ReadStyleField(state, index, "pressedStyle",
            node.pressedStyle, error) ||
        !ReadStyleField(state, index, "focusStyle",
            node.focusStyle, error) ||
        !ReadStyleField(state, index, "disabledStyle",
            node.disabledStyle, error) ||
        !ReadStyleField(state, index, "validationStyle",
            node.validationStyle, error) ||
        !ReadStyleField(state, index, "checkedStyle",
            node.checkedStyle, error) ||
        !ReadStyleField(state, index, "selectedStyle",
            node.selectedStyle, error) ||
        !ReadStyleField(state, index, "todayStyle",
            node.todayStyle, error) ||
        !ReadStyleField(state, index, "adjacentStyle",
            node.adjacentStyle, error) ||
        !ReadStyleField(state, index, "eventStyle",
            node.eventStyle, error))
        return false;

    node.virtualLayoutRevision =
        static_cast<std::uint64_t>(virtualLayoutRevision);

    if (initialScrollKeySpecified)
    {
        if (initialScrollKey.empty() || initialScrollKey.size() > 128)
        {
            error = "view field 'initialScrollKey' must contain 1 to 128 bytes";
            return false;
        }
        node.initialScrollKey = std::move(initialScrollKey);
    }
    if (node.initialScrollIndex &&
        *node.initialScrollIndex > node.itemCount)
    {
        error = "view field 'initialScrollIndex' is outside the collection";
        return false;
    }

    if (focusableSpecified) node.focusable = focusable;
    if (tabIndexSpecified) node.tabIndex = tabIndex;
    if (node.accessKey.size() == 1 &&
        node.accessKey[0] >= 'a' && node.accessKey[0] <= 'z')
        node.accessKey[0] = static_cast<char>(
            node.accessKey[0] - 'a' + 'A');

    if (visibilitySpecified)
    {
        if (visibleSpecified &&
            ((node.visibility == ViewVisibility::Visible && !node.visible) ||
                (node.visibility == ViewVisibility::Collapsed &&
                    node.visible) ||
                node.visibility == ViewVisibility::Hidden))
        {
            error = "view visible and visibility describe conflicting states";
            return false;
        }
        node.visible = node.visibility != ViewVisibility::Collapsed;
    }
    else
        node.visibility = node.visible
            ? ViewVisibility::Visible : ViewVisibility::Collapsed;

    if (rowTrackCount > 0 && node.rowTracks.empty())
        node.rowTracks.resize(rowTrackCount);

    if (overflowSpecified)
    {
        const bool overflowClips = node.overflow == ViewOverflow::Clip;
        if (clipSpecified && node.clipChildren != overflowClips)
        {
            error = "view clip and overflow must describe the same clipping behavior";
            return false;
        }
        node.clipChildren = overflowClips;
    }

    if (styledTextNode)
    {
        if (!ReadTextSpansField(state, index, node.spans, error))
            return false;
        node.text.clear();
        for (const auto& span : node.spans) node.text += span.text;
    }

    if (monthCalendarNode)
    {
        std::size_t year = 0;
        std::size_t month = 0;
        std::size_t firstDay = 1;
        std::vector<std::string> weekdayLabels;
        if (!ReadSizeField(state, index, "year", year, error) ||
            !ReadSizeField(state, index, "month", month, error) ||
            !ReadSizeField(state, index, "firstDayOfWeek", firstDay,
                error) ||
            !ReadStringField(state, index, "selectedDate",
                node.calendarSelectedDate, true, error) ||
            !ReadStringField(state, index, "todayDate",
                node.calendarTodayDate, false, error) ||
            !ReadStringArrayField(state, index, "eventDates",
                node.calendarEventDates, 0,
                ViewTreeLimits::MaximumCalendarEventDates, false,
                error) ||
            !ReadStringArrayField(state, index, "weekdayLabels",
                weekdayLabels, 7, 7, true, error))
            return false;
        node.calendarYear = static_cast<int>(year);
        node.calendarMonth = static_cast<int>(month);
        node.firstDayOfWeek = static_cast<int>(firstDay);
        std::copy(weekdayLabels.begin(), weekdayLabels.end(),
            node.weekdayLabels.begin());
    }

    if (textInputNode)
    {
        if (!ReadStringField(state, index, "value",
                node.inputValue, true, error) ||
            !ReadTextSelectionField(state, index, node.inputValue,
                node.textSelection, error))
            return false;
        if (node.textSelection && node.selectAll)
        {
            error = "view input selection cannot be combined with selectAll";
            return false;
        }
    }
    else if (progressNode || sliderNode || numberInputNode)
    {
        if (!ReadFloatField(state, index, "value", node.value, error))
            return false;
    }
    if (inputNode || selectNode)
    {
        if (!ReadStringField(state, index, "placeholder",
                node.placeholder, false, error))
            return false;
    }
    if (inputNode)
    {
        if (
            !ReadNonNegativeSizeField(state, index, "maxBytes",
                node.maximumUtf8Bytes, false, error))
            return false;
    }

    if (scrollContainerNode && !FieldPresent(state, index, "orientation"))
        node.orientation = ViewOrientation::Vertical;
    if (node.type == ViewNodeType::List &&
        !FieldPresent(state, index, "orientation"))
        node.orientation = ViewOrientation::Vertical;

    if (dividerNode && node.orientation == ViewOrientation::Vertical)
    {
        if (!FieldPresent(state, index, "width"))
            node.width = { ViewLengthKind::Auto, 0.0f };
        if (!FieldPresent(state, index, "height"))
            node.height = { ViewLengthKind::Fill, 0.0f };
    }

    if (sliderNode || numberInputNode)
    {
        if (!ReadFloatField(state, index, "min", node.minimum, error) ||
            !ReadFloatField(state, index, "max", node.maximum, error) ||
            !ReadFloatField(state, index, "step", node.step, error))
            return false;
        if (sliderNode && node.orientation == ViewOrientation::Vertical)
        {
            if (!FieldPresent(state, index, "width"))
                node.width = { ViewLengthKind::Auto, 0.0f };
            if (!FieldPresent(state, index, "height"))
                node.height = { ViewLengthKind::Fill, 0.0f };
        }
    }

    if (choiceNode)
    {
        if (!ReadStringField(state, index, "selectedValue",
                node.selectedValue, true, error) ||
            !ReadChoiceOptionsField(state, index, node.options, error))
            return false;
    }

    if (seriesNode)
    {
        if (!ReadNumberArrayField(state, index, "values",
                node.values, true, error))
            return false;
        const bool hasMinimum = FieldPresent(state, index, "min");
        const bool hasMaximum = FieldPresent(state, index, "max");
        if (hasMinimum != hasMaximum)
        {
            error = "data-series nodes must provide both min and max";
            return false;
        }
        if (hasMinimum)
        {
            float minimum = 0.0f;
            float maximum = 0.0f;
            if (!ReadFloatField(state, index, "min", minimum, error) ||
                !ReadFloatField(state, index, "max", maximum, error))
                return false;
            node.seriesMinimum = minimum;
            node.seriesMaximum = maximum;
        }
    }

    lua_getfield(state, index, "accessibility");
    if (lua_istable(state, -1))
    {
        if (!ValidateObjectFields(state, -1, { "role", "label" },
                "view accessibility", error) ||
            !ReadStringField(state, -1, "role",
                node.accessibilityRole, false, error) ||
            !ReadStringField(state, -1, "label",
                node.accessibilityLabel, false, error))
        {
            lua_pop(state, 1);
            return false;
        }
    }
    else if (!lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        error = "view accessibility must be a table";
        return false;
    }
    lua_pop(state, 1);

    lua_getfield(state, index, "events");
    if (lua_istable(state, -1))
    {
        const int events = lua_absindex(state, -1);
        if (!ValidateObjectFields(state, events,
                { "pointerEnter", "pointerLeave", "pointerDown",
                    "pointerMove", "pointerUp", "click", "doubleClick", "wheel", "contextMenu",
                    "keyDown", "keyUp", "change", "selectionChange",
                    "focus", "blur", "submit", "scrollEnd" },
                "view events", error))
        {
            lua_pop(state, 1);
            return false;
        }
        for (const char* eventName : { "pointerEnter", "pointerLeave",
            "pointerDown", "pointerMove", "pointerUp", "click", "doubleClick", "wheel",
            "contextMenu", "keyDown", "keyUp", "change",
            "selectionChange", "focus", "blur", "submit", "scrollEnd" })
        {
            lua_getfield(state, events, eventName);
            if (!lua_isnil(state, -1))
            {
                InteractionAction action;
                if (!ParseAction(state, -1, action, error))
                {
                    lua_pop(state, 2);
                    return false;
                }
                node.events.emplace(eventName, std::move(action));
            }
            lua_pop(state, 1);
        }
    }
    else if (!lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        error = "view events must be a table";
        return false;
    }
    lua_pop(state, 1);

    if (actionNode)
    {
        lua_getfield(state, index, "action");
        if (!lua_isnil(state, -1))
        {
            InteractionAction action;
            if (!ParseAction(state, -1, action, error))
            {
                lua_pop(state, 1);
                return false;
            }
            node.events.insert_or_assign(
                controlNode ? "change" : "click", std::move(action));
        }
        lua_pop(state, 1);
    }

    const bool singleSlotChild = (slotSurfaceNode || slotItemNode) &&
        FieldPresent(state, index, "child");
    lua_getfield(state, index, singleSlotChild ? "child" : "children");
    if (singleSlotChild && !lua_isnil(state, -1))
    {
        ViewNode parsed;
        if (!ParseNode(state, -1, parsed, depth + 1,
                parsedNodes, error))
        {
            lua_pop(state, 1);
            return false;
        }
        node.children.push_back(std::move(parsed));
        lua_pop(state, 1);
        return true;
    }
    if (lua_istable(state, -1))
    {
        if (!ValidateArray(state, -1, "view children", error))
        {
            lua_pop(state, 1);
            return false;
        }
        const std::size_t count = lua_rawlen(state, -1);
        if (count > ViewTreeLimits::MaximumNodes)
        {
            lua_pop(state, 1);
            error = "view child array exceeds the node limit";
            return false;
        }
        node.children.reserve(count);
        for (std::size_t child = 0; child < count; ++child)
        {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(child + 1));
            ViewNode parsed;
            if (!ParseNode(state, -1, parsed, depth + 1,
                    parsedNodes, error))
            {
                lua_pop(state, 2);
                return false;
            }
            node.children.push_back(std::move(parsed));
            lua_pop(state, 1);
        }
    }
    else if (!lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        error = "view children must be an array";
        return false;
    }
    lua_pop(state, 1);

    if (collectionNode)
    {
        const auto readContent = [&](const char* field,
            std::optional<ViewNode>& output) {
            lua_getfield(state, index, field);
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                return true;
            }
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                error = std::string("view field '") + field +
                    "' must be a view node";
                return false;
            }
            ViewNode parsed;
            if (!ParseNode(state, -1, parsed, depth + 1,
                    parsedNodes, error))
            {
                lua_pop(state, 1);
                return false;
            }
            lua_pop(state, 1);
            output = std::move(parsed);
            return true;
        };
        std::optional<ViewNode> emptyContent;
        std::optional<ViewNode> loadingContent;
        if (!readContent("emptyContent", emptyContent) ||
            !readContent("loadingContent", loadingContent))
            return false;
        if (node.busy && loadingContent)
        {
            node.children.clear();
            node.children.push_back(std::move(*loadingContent));
            node.collectionContent = ViewCollectionContent::Loading;
        }
        else
        {
            const bool empty = virtualCollectionNode
                ? node.itemCount == 0 : node.children.empty();
            if (empty && emptyContent)
            {
                node.children.clear();
                node.children.push_back(std::move(*emptyContent));
                node.collectionContent = ViewCollectionContent::Empty;
            }
        }
    }
    return true;
}

int MakeNode(lua_State* state, const char* type)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    const int source = lua_absindex(state, 1);
    lua_newtable(state);
    const int result = lua_absindex(state, -1);
    lua_pushnil(state);
    while (lua_next(state, source) != 0)
    {
        lua_pushvalue(state, -2);
        lua_pushvalue(state, -2);
        lua_rawset(state, result);
        lua_pop(state, 1);
    }
    lua_pushstring(state, type);
    lua_setfield(state, result, "type");
    return 1;
}
}

bool ParseLuaViewTree(lua_State* state, int index, ViewNode& root,
    std::string& error)
{
    error.clear();
    std::size_t parsedNodes = 0;
    return ParseNode(state, index, root, 0, parsedNodes, error);
}

int LuaViewBox(lua_State* state) { return MakeNode(state, "box"); }
int LuaViewRow(lua_State* state) { return MakeNode(state, "row"); }
int LuaViewColumn(lua_State* state) { return MakeNode(state, "column"); }
int LuaViewGrid(lua_State* state) { return MakeNode(state, "grid"); }
int LuaViewFlow(lua_State* state) { return MakeNode(state, "flow"); }
int LuaViewStack(lua_State* state) { return MakeNode(state, "stack"); }
int LuaViewScroll(lua_State* state) { return MakeNode(state, "scroll"); }
int LuaViewList(lua_State* state) { return MakeNode(state, "list"); }
int LuaViewGridList(lua_State* state)
{
    return MakeNode(state, "gridList");
}
int LuaViewVirtualList(lua_State* state)
{
    return MakeNode(state, "virtualList");
}
int LuaViewVirtualGrid(lua_State* state)
{
    return MakeNode(state, "virtualGrid");
}
int LuaViewListItem(lua_State* state)
{
    return MakeNode(state, "listItem");
}
int LuaViewText(lua_State* state) { return MakeNode(state, "text"); }
int LuaViewStyledText(lua_State* state)
{
    return MakeNode(state, "styledText");
}
int LuaViewTextInput(lua_State* state)
{
    return MakeNode(state, "textInput");
}
int LuaViewTextArea(lua_State* state)
{
    return MakeNode(state, "textArea");
}
int LuaViewSearchBox(lua_State* state)
{
    return MakeNode(state, "searchBox");
}
int LuaViewNumberInput(lua_State* state)
{
    return MakeNode(state, "numberInput");
}
int LuaViewSelect(lua_State* state) { return MakeNode(state, "select"); }
int LuaViewImage(lua_State* state) { return MakeNode(state, "image"); }
int LuaViewReferenceIcon(lua_State* state)
{
    return MakeNode(state, "referenceIcon");
}
int LuaViewButton(lua_State* state) { return MakeNode(state, "button"); }
int LuaViewLink(lua_State* state) { return MakeNode(state, "link"); }
int LuaViewToggle(lua_State* state) { return MakeNode(state, "toggle"); }
int LuaViewCheckbox(lua_State* state)
{
    return MakeNode(state, "checkbox");
}
int LuaViewRadioGroup(lua_State* state)
{
    return MakeNode(state, "radioGroup");
}
int LuaViewSlider(lua_State* state) { return MakeNode(state, "slider"); }
int LuaViewIcon(lua_State* state) { return MakeNode(state, "icon"); }
int LuaViewIconButton(lua_State* state)
{
    return MakeNode(state, "iconButton");
}
int LuaViewShape(lua_State* state) { return MakeNode(state, "shape"); }
int LuaViewBadge(lua_State* state) { return MakeNode(state, "badge"); }
int LuaViewDivider(lua_State* state) { return MakeNode(state, "divider"); }
int LuaViewProgressBar(lua_State* state)
{
    return MakeNode(state, "progressBar");
}
int LuaViewProgressRing(lua_State* state)
{
    return MakeNode(state, "progressRing");
}
int LuaViewMeter(lua_State* state) { return MakeNode(state, "meter"); }
int LuaViewSparkline(lua_State* state)
{
    return MakeNode(state, "sparkline");
}
int LuaViewLineChart(lua_State* state)
{
    return MakeNode(state, "lineChart");
}
int LuaViewBarChart(lua_State* state)
{
    return MakeNode(state, "barChart");
}
int LuaViewWaveform(lua_State* state)
{
    return MakeNode(state, "waveform");
}
int LuaViewSpectrum(lua_State* state)
{
    return MakeNode(state, "spectrum");
}
int LuaViewMonthCalendar(lua_State* state)
{
    return MakeNode(state, "monthCalendar");
}
int LuaViewSlotSurface(lua_State* state)
{
    return MakeNode(state, "slotSurface");
}
int LuaViewSlotItem(lua_State* state)
{
    return MakeNode(state, "slotItem");
}
int LuaViewSpacer(lua_State* state) { return MakeNode(state, "spacer"); }
}
