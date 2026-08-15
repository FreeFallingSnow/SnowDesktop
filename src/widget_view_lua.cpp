#include "widget_view_lua.h"
#include "widget_resource_lua.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
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

bool ReadOptionalColor(lua_State* state, int table, const char* field,
    std::optional<std::uint32_t>& value, std::string& error)
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
        error = std::string("view style field '") + field +
            "' must be an integer color";
        return false;
    }
    const lua_Integer color = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (color < 0 || color > 0xFFFFFF)
    {
        error = std::string("view style field '") + field +
            "' must be between 0 and 0xFFFFFF";
        return false;
    }
    value = static_cast<std::uint32_t>(color);
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
            error) &&
        ReadOptionalColor(state, table, "foreground", style.foreground,
            error) &&
        ReadOptionalColor(state, table, "borderColor", style.borderColor,
            error) &&
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
    else
    {
        error = "view field 'justifyContent' has an unsupported value";
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
    if (!ValidateObjectFields(state, index, { "id", "value" },
            "view action", error) ||
        !ReadStringField(state, index, "id", action.id, true, error))
        return false;
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
    if (type == "box") result = ViewNodeType::Box;
    else if (type == "row") result = ViewNodeType::Row;
    else if (type == "column") result = ViewNodeType::Column;
    else if (type == "grid") result = ViewNodeType::Grid;
    else if (type == "stack") result = ViewNodeType::Stack;
    else if (type == "text") result = ViewNodeType::Text;
    else if (type == "image") result = ViewNodeType::Image;
    else if (type == "button") result = ViewNodeType::Button;
    else if (type == "toggle") result = ViewNodeType::Toggle;
    else if (type == "checkbox") result = ViewNodeType::Checkbox;
    else if (type == "icon") result = ViewNodeType::Icon;
    else if (type == "iconButton") result = ViewNodeType::IconButton;
    else if (type == "shape") result = ViewNodeType::Shape;
    else if (type == "badge") result = ViewNodeType::Badge;
    else if (type == "divider") result = ViewNodeType::Divider;
    else if (type == "progressBar") result = ViewNodeType::ProgressBar;
    else if (type == "progressRing") result = ViewNodeType::ProgressRing;
    else if (type == "meter") result = ViewNodeType::Meter;
    else if (type == "sparkline") result = ViewNodeType::Sparkline;
    else if (type == "lineChart") result = ViewNodeType::LineChart;
    else if (type == "barChart") result = ViewNodeType::BarChart;
    else if (type == "waveform") result = ViewNodeType::Waveform;
    else if (type == "spectrum") result = ViewNodeType::Spectrum;
    else if (type == "spacer") result = ViewNodeType::Spacer;
    else return false;
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
    if (!ValidateObjectFields(state, index,
            { "type", "key", "text", "label", "glyph", "iconFont",
                "source", "font", "fit", "alignment", "interpolation",
                "alt",
                "shape", "orientation", "value", "values", "min", "max",
                "thickness", "trackOpacity",
                "fillOpacity", "width", "height",
                "padding", "gap", "columns", "columnGap", "rowGap",
                "flexGrow", "fontSize", "bold",
                "checked", "visible", "enabled", "cursor", "alignItems",
                "alignSelf", "justifyContent", "textAlign", "style",
                "hoverStyle", "pressedStyle", "checkedStyle",
                "accessibility", "events",
                "action", "children" }, "view node", error))
        return false;
    std::string type;
    if (!ReadStringField(state, index, "type", type, true, error) ||
        !ParseNodeType(type, node.type))
    {
        if (error.empty()) error = "unsupported view node type: " + type;
        return false;
    }
    const bool buttonNode = node.type == ViewNodeType::Button ||
        node.type == ViewNodeType::IconButton;
    const bool controlNode = node.type == ViewNodeType::Toggle ||
        node.type == ViewNodeType::Checkbox;
    const bool labelNode = node.type == ViewNodeType::Button ||
        controlNode;
    const bool actionNode = buttonNode || controlNode;
    const bool iconNode = node.type == ViewNodeType::Icon ||
        node.type == ViewNodeType::IconButton;
    const bool textNode = node.type == ViewNodeType::Text ||
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
    const bool dividerNode = node.type == ViewNodeType::Divider;
    const bool gridNode = node.type == ViewNodeType::Grid;
    const bool textResourceNode = textNode || labelNode;
    if (labelNode &&
        (FieldPresent(state, index, "text") ||
            FieldPresent(state, index, "glyph")))
    {
        error = "button, toggle, and checkbox nodes use 'label', not 'text' or 'glyph'";
        return false;
    }
    if (iconNode && (FieldPresent(state, index, "text") ||
            FieldPresent(state, index, "label")))
    {
        error = "icon nodes use 'glyph', not 'text' or 'label'";
        return false;
    }
    if (!actionNode &&
        (FieldPresent(state, index, "label") ||
            FieldPresent(state, index, "action")))
    {
        error = "only button, toggle, and checkbox nodes accept 'label' and 'action'";
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
    if (!dividerNode && FieldPresent(state, index, "orientation"))
    {
        error = "only divider nodes accept orientation";
        return false;
    }
    if (!gridNode && (FieldPresent(state, index, "columns") ||
            FieldPresent(state, index, "columnGap") ||
            FieldPresent(state, index, "rowGap")))
    {
        error = "only grid nodes accept columns, columnGap, and rowGap";
        return false;
    }
    if (gridNode && !FieldPresent(state, index, "columns"))
    {
        error = "grid nodes require columns";
        return false;
    }
    if (!progressNode && FieldPresent(state, index, "value"))
    {
        error = "only progress nodes accept 'value'";
        return false;
    }
    if (!controlNode && FieldPresent(state, index, "checked"))
    {
        error = "only toggle and checkbox nodes accept checked";
        return false;
    }
    if (!controlNode && FieldPresent(state, index, "checkedStyle"))
    {
        error = "only toggle and checkbox nodes accept checkedStyle";
        return false;
    }
    if (!seriesNode && (FieldPresent(state, index, "values") ||
            FieldPresent(state, index, "min") ||
            FieldPresent(state, index, "max")))
    {
        error = "only data-series nodes accept values, min, and max";
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
    if (!imageNode && (FieldPresent(state, index, "source") ||
            FieldPresent(state, index, "fit") ||
            FieldPresent(state, index, "alignment") ||
            FieldPresent(state, index, "interpolation") ||
            FieldPresent(state, index, "alt")))
    {
        error = "only image nodes accept image resource fields";
        return false;
    }
    if (!textResourceNode && FieldPresent(state, index, "font"))
    {
        error = "only text, badge, button, toggle, and checkbox nodes accept a font resource";
        return false;
    }
    if (imageNode && !FieldPresent(state, index, "alt"))
    {
        error = "image nodes require an explicit 'alt' field";
        return false;
    }
    if (controlNode && !FieldPresent(state, index, "checked"))
    {
        error = "toggle and checkbox nodes require checked";
        return false;
    }
    if (!ReadStringField(state, index, "key", node.key, true, error))
        return false;
    if (node.type == ViewNodeType::Badge &&
        !FieldPresent(state, index, "padding"))
        node.padding = 4.0f;
    const char* contentField = iconNode ? "glyph" :
        (labelNode ? "label" : "text");
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
        !ReadOrientationField(state, index, node.orientation, error) ||
        !ReadLengthField(state, index, "width", node.width, error) ||
        !ReadLengthField(state, index, "height", node.height, error) ||
        !ReadFloatField(state, index, "padding", node.padding, error) ||
        !ReadFloatField(state, index, "gap", node.gap, error) ||
        !ReadSizeField(state, index, "columns", node.columns, error) ||
        !ReadOptionalNodeFloatField(state, index, "columnGap",
            node.columnGap, error) ||
        !ReadOptionalNodeFloatField(state, index, "rowGap",
            node.rowGap, error) ||
        !ReadFloatField(state, index, "flexGrow", node.flexGrow, error) ||
        !ReadFloatField(state, index, "fontSize", node.fontSize, error) ||
        !ReadFloatField(state, index, "value", node.value, error) ||
        !ReadFloatField(state, index, "thickness", node.thickness, error) ||
        !ReadFloatField(state, index, "trackOpacity",
            node.trackOpacity, error) ||
        !ReadFloatField(state, index, "fillOpacity",
            node.fillOpacity, error) ||
        !ReadBoolField(state, index, "bold", node.bold, error) ||
        !ReadBoolField(state, index, "checked", node.checked, error) ||
        !ReadBoolField(state, index, "visible", node.visible, error) ||
        !ReadBoolField(state, index, "enabled", node.enabled, error) ||
        !ReadStringField(state, index, "cursor", node.cursor, false, error) ||
        !ReadAlignmentField(state, index, "alignItems",
            node.alignItems, false, error) ||
        !ReadAlignmentField(state, index, "alignSelf",
            node.alignSelf, true, error) ||
        !ReadJustificationField(state, index,
            node.justifyContent, error) ||
        !ReadTextAlignmentField(state, index, node.textAlign, error) ||
        !ReadShapeKindField(state, index, node.shapeKind, error) ||
        !ReadIconFontField(state, index, node.iconFont, error) ||
        !ReadStyleField(state, index, "style", node.style, error) ||
        !ReadStyleField(state, index, "hoverStyle",
            node.hoverStyle, error) ||
        !ReadStyleField(state, index, "pressedStyle",
            node.pressedStyle, error) ||
        !ReadStyleField(state, index, "checkedStyle",
            node.checkedStyle, error))
        return false;

    if (dividerNode && node.orientation == ViewOrientation::Vertical)
    {
        if (!FieldPresent(state, index, "width"))
            node.width = { ViewLengthKind::Auto, 0.0f };
        if (!FieldPresent(state, index, "height"))
            node.height = { ViewLengthKind::Fill, 0.0f };
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
                    "pointerUp", "click", "doubleClick", "contextMenu",
                    "change" },
                "view events", error))
        {
            lua_pop(state, 1);
            return false;
        }
        for (const char* eventName : { "pointerEnter", "pointerLeave",
            "pointerDown", "pointerUp", "click", "doubleClick",
            "contextMenu", "change" })
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

    lua_getfield(state, index, "children");
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
int LuaViewStack(lua_State* state) { return MakeNode(state, "stack"); }
int LuaViewText(lua_State* state) { return MakeNode(state, "text"); }
int LuaViewImage(lua_State* state) { return MakeNode(state, "image"); }
int LuaViewButton(lua_State* state) { return MakeNode(state, "button"); }
int LuaViewToggle(lua_State* state) { return MakeNode(state, "toggle"); }
int LuaViewCheckbox(lua_State* state)
{
    return MakeNode(state, "checkbox");
}
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
int LuaViewSpacer(lua_State* state) { return MakeNode(state, "spacer"); }
}
