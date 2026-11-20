#include "widget_surface_interaction.h"

#include "widget/runtime/widget_engine.h"
#include "widget/runtime/widget_lua_binding_context.h"
#include "widget/data/widget_resource_lua.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/view/widget_surface_host.h"
#include "widget/view/widget_host_input_support.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "theme/surface_theme.h"
#include "common/constants.h"
#include "widget/view/widget_ui_metrics.h"
#include "widget/storage/widget_storage_value.h"
#include "widget/settings/widget_settings_declarations.h"
#include "widget/view/widget_duration_picker_lua.h"
#include "widget/view/widget_time_picker_lua.h"
#include "widget/view/widget_date_picker_lua.h"
#include "widget/view/widget_interaction_lua_value.h"
#include "widget/view/widget_scroll_rules.h"
#include "widget/view/widget_view_lua.h"
#include "widget/view/widget_layout_lua.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

using snowdesktop::widget_runtime::BoundWidgetId;
using snowdesktop::widget_runtime::LuaReadStorageField;
using snowdesktop::widget_runtime::CurrentLayoutContentWidth;
using snowdesktop::widget_runtime::CurrentLayoutContentHeight;
using snowdesktop::widget_runtime::ReadInteractionValue;
using snowdesktop::widget_runtime::PushInteractionValue;

using snowdesktop::lua_runtime::LuaTableUsesOnlyFields;
using snowdesktop::lua_runtime::LuaTableIsContiguousArray;

static bool IsValidUtf8Local(std::string_view value)
{
    if (value.empty()) return false;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0) > 0;
}

static std::string ReadRequiredStringField(lua_State* state, int table,
    const char* field)
{
    table = lua_absindex(state, table);
    lua_getfield(state, table, field);
    std::size_t length = 0;
    const char* value = luaL_checklstring(state, -1, &length);
    std::string result(value ? value : "", length);
    lua_pop(state, 1);
    return result;
}

int lua_InteractionRegion(lua_State* state)
{
    using namespace snowdesktop::widget_runtime;
    if (auto* d2d = GetD2D(state); d2d && d2d->backgroundLayerActive)
        return luaL_error(state,
            "interaction.region: backgroundLayer is decorative and cannot register interactions");
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    InteractionRegion region;
    region.key = ReadRequiredStringField(state, descriptor, "key");

    lua_getfield(state, descriptor, "shape");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int shape = lua_absindex(state, -1);
    const std::string type = ReadRequiredStringField(state, shape, "type");
    if (type == "rect")
        region.shape.type = InteractionShapeType::Rect;
    else if (type == "roundedRect")
        region.shape.type = InteractionShapeType::RoundedRect;
    else if (type == "circle")
        region.shape.type = InteractionShapeType::Circle;
    else
        return luaL_error(state, "interaction.region: unsupported shape type");
    lua_getfield(state, shape, "x");
    region.shape.x = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    lua_getfield(state, shape, "y");
    region.shape.y = static_cast<float>(luaL_checknumber(state, -1));
    lua_pop(state, 1);
    if (region.shape.type == InteractionShapeType::Circle)
    {
        lua_getfield(state, shape, "radius");
        region.shape.radius = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
    }
    else
    {
        lua_getfield(state, shape, "width");
        region.shape.width = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, shape, "height");
        region.shape.height = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        lua_getfield(state, shape, "radius");
        if (!lua_isnil(state, -1))
            region.shape.radius = static_cast<float>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "cursor");
    if (!lua_isnil(state, -1))
        region.cursor = luaL_checkstring(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "dragImage");
    if (!lua_isnil(state, -1))
    {
        const auto* image = TestResourceHandle(state, -1);
        if (!image || image->type != LuaResourceType::Image ||
            !IsWidgetRuntimeImageToken(image->name))
            return luaL_error(state, "interaction.region: dragImage must be a runtime image handle");
        region.dragImage = image->name;
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "tooltip");
    if (!lua_isnil(state, -1))
    {
        if (lua_isstring(state, -1))
        {
            std::size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            region.tooltip.assign(value ? value : "", length);
        }
        else if (lua_istable(state, -1))
        {
            const int tooltip = lua_absindex(state, -1);
            lua_pushnil(state);
            while (lua_next(state, tooltip) != 0)
            {
                std::size_t keyLength = 0;
                const char* key = lua_tolstring(state, -2, &keyLength);
                const std::string_view field(key ? key : "", keyLength);
                if (lua_type(state, -2) != LUA_TSTRING ||
                    (field != "title" && field != "text"))
                {
                    lua_pop(state, 3);
                    return luaL_error(state,
                        "interaction.region: tooltip supports only title and text");
                }
                lua_pop(state, 1);
            }
            lua_getfield(state, tooltip, "title");
            if (!lua_isnil(state, -1))
            {
                std::size_t length = 0;
                const char* value = luaL_checklstring(state, -1, &length);
                region.tooltipTitle.assign(value ? value : "", length);
            }
            lua_pop(state, 1);
            lua_getfield(state, tooltip, "text");
            if (!lua_isstring(state, -1))
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "interaction.region: rich tooltip requires string text");
            }
            std::size_t length = 0;
            const char* value = lua_tolstring(state, -1, &length);
            region.tooltip.assign(value ? value : "", length);
            lua_pop(state, 1);
            if (region.tooltip.empty())
            {
                lua_pop(state, 1);
                return luaL_error(state,
                    "interaction.region: rich tooltip text must not be empty");
            }
        }
        else
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "interaction.region: tooltip must be a string or tooltip descriptor");
        }
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "enabled");
    region.enabled = lua_isnil(state, -1) || lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "capturePointer");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isboolean(state, -1))
            return luaL_error(state,
                "interaction.region: capturePointer must be a boolean");
        region.capturePointer = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "focusable");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isboolean(state, -1))
            return luaL_error(state,
                "interaction.region: focusable must be a boolean");
        region.focusable = lua_toboolean(state, -1) != 0;
    }
    lua_pop(state, 1);
    lua_getfield(state, descriptor, "tabIndex");
    if (!lua_isnil(state, -1))
    {
        if (!lua_isinteger(state, -1))
            return luaL_error(state,
                "interaction.region: tabIndex must be an integer");
        const lua_Integer value = lua_tointeger(state, -1);
        if (value < -1 || value > 32767)
            return luaL_error(state,
                "interaction.region: tabIndex must be between -1 and 32767");
        region.tabIndex = static_cast<int>(value);
    }
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "accessibility");
    if (lua_istable(state, -1))
    {
        const int accessibility = lua_absindex(state, -1);
        lua_pushnil(state);
        while (lua_next(state, accessibility) != 0)
        {
            std::size_t keyLength = 0;
            const char* key = lua_tolstring(state, -2, &keyLength);
            const std::string_view field(key ? key : "", keyLength);
            const bool supported = lua_type(state, -2) == LUA_TSTRING &&
                (field == "role" || field == "label" ||
                    field == "value" || field == "hint" ||
                    field == "headingLevel" || field == "live" ||
                    field == "positionInSet" || field == "setSize" ||
                    field == "hidden");
            lua_pop(state, 1);
            if (!supported)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "interaction.region: unsupported accessibility field");
            }
        }
        const auto readAccessibilityString = [&](const char* field,
            std::string& output) {
            lua_getfield(state, accessibility, field);
            if (!lua_isnil(state, -1))
            {
                std::size_t length = 0;
                const char* value = luaL_checklstring(
                    state, -1, &length);
                output.assign(value ? value : "", length);
            }
            lua_pop(state, 1);
        };
        readAccessibilityString("role", region.accessibilityRole);
        readAccessibilityString("label", region.accessibilityLabel);
        readAccessibilityString("value", region.accessibilityValue);
        readAccessibilityString("hint", region.accessibilityHint);
        const auto readAccessibilityInteger = [&](const char* field,
            int& output) {
            lua_getfield(state, accessibility, field);
            if (!lua_isnil(state, -1))
            {
                if (!lua_isinteger(state, -1))
                    return false;
                const lua_Integer value = lua_tointeger(state, -1);
                if (value < (std::numeric_limits<int>::min)() ||
                    value > (std::numeric_limits<int>::max)())
                    return false;
                output = static_cast<int>(value);
            }
            lua_pop(state, 1);
            return true;
        };
        if (!readAccessibilityInteger("headingLevel",
                region.accessibilityHeadingLevel) ||
            !readAccessibilityInteger("positionInSet",
                region.accessibilityPositionInSet) ||
            !readAccessibilityInteger("setSize",
                region.accessibilitySetSize))
            return luaL_error(state,
                "interaction.region: accessibility indices must be integers");
        std::string live;
        readAccessibilityString("live", live);
        if (live.empty() || live == "off")
            region.accessibilityLive = AccessibilityLive::Off;
        else if (live == "polite")
            region.accessibilityLive = AccessibilityLive::Polite;
        else if (live == "assertive")
            region.accessibilityLive = AccessibilityLive::Assertive;
        else
            return luaL_error(state,
                "interaction.region: accessibility live must be off, polite, or assertive");
        lua_getfield(state, accessibility, "hidden");
        if (!lua_isnil(state, -1))
        {
            if (!lua_isboolean(state, -1))
                return luaL_error(state,
                    "interaction.region: accessibility hidden must be a boolean");
            region.accessibilityHidden =
                lua_toboolean(state, -1) != 0;
        }
        lua_pop(state, 1);
    }
    else if (!lua_isnil(state, -1))
        return luaL_error(state,
            "interaction.region: accessibility must be a table");
    lua_pop(state, 1);

    lua_getfield(state, descriptor, "events");
    if (lua_istable(state, -1))
    {
        const int events = lua_absindex(state, -1);
        for (const char* eventName : { "pointerEnter", "pointerLeave",
            "pointerDown", "pointerUp", "pointerMove", "click",
            "doubleClick", "fileDrop", "wheel", "contextMenu", "keyDown", "keyUp" })
        {
            lua_getfield(state, events, eventName);
            if (lua_isnil(state, -1))
            {
                lua_pop(state, 1);
                continue;
            }
            luaL_checktype(state, -1, LUA_TTABLE);
            const int actionTable = lua_absindex(state, -1);
            InteractionAction action;
            action.id = ReadRequiredStringField(state, actionTable, "id");
            lua_getfield(state, actionTable, "scope");
            if (!lua_isnil(state, -1))
            {
                const char* scope = luaL_checkstring(state, -1);
                if (std::strcmp(scope, "component") == 0)
                    action.contextMenuScope =
                        InteractionAction::ContextMenuScope::Component;
                else if (std::strcmp(scope, "element") != 0)
                    return luaL_error(state,
                        "interaction.region: action scope must be element or component");
            }
            lua_pop(state, 1);
            lua_getfield(state, actionTable, "value");
            std::size_t nodes = 0;
            std::size_t bytes = 0;
            std::unordered_set<const void*> ancestors;
            std::string error;
            if (!ReadInteractionValue(state, -1, action.value, 0,
                    nodes, bytes, ancestors, error))
                return luaL_error(state, "interaction.region: %s", error.c_str());
            lua_pop(state, 1);
            region.events.emplace(eventName, std::move(action));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    if (region.capturePointer &&
        !region.events.contains("pointerMove") &&
        !region.events.contains("pointerUp"))
    {
        return luaL_error(state,
            "interaction.region: capturePointer requires pointerMove or pointerUp");
    }

    auto* d2d = GetD2D(state);
    std::string error;
    if (!d2d || !d2d->engine ||
        !d2d->engine->RuntimeSubmitInteractionRegion(
            BoundWidgetId(state), std::move(region), error))
    {
        return luaL_error(state, "interaction.region: %s",
            error.empty() ? "host context is unavailable" : error.c_str());
    }
    return 0;
}

int lua_InteractionIsHovered(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionHovered(
            BoundWidgetId(state), key ? key : ""));
    return 1;
}

int lua_InteractionIsPressed(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionPressed(
            BoundWidgetId(state), key ? key : ""));
    return 1;
}

int lua_InteractionIsFocused(lua_State* state)
{
    const char* key = luaL_checkstring(state, 1);
    auto* d2d = GetD2D(state);
    lua_pushboolean(state, d2d && d2d->engine &&
        d2d->engine->RuntimeInteractionFocused(
            BoundWidgetId(state), key ? key : ""));
    return 1;
}

int lua_InteractionScroll(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    const int descriptor = lua_absindex(state, 1);
    // Lua validation can longjmp. Keep the key rooted on the Lua stack and
    // construct owning C++ values only after the last argument check.
    lua_getfield(state, descriptor, "key");
    std::size_t keyLength = 0;
    const char* keyValue = luaL_checklstring(state, -1, &keyLength);
    const std::string_view key(keyValue, keyLength);
    if (key.size() > 128 || key.find('\0') != std::string_view::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "interaction.scroll: key must contain 1 to 128 bytes of valid UTF-8");
    }

    lua_getfield(state, descriptor, "shape");
    luaL_checktype(state, -1, LUA_TTABLE);
    const int shape = lua_absindex(state, -1);
    if (ReadRequiredStringField(state, shape, "type") != "rect")
        return luaL_error(state,
            "interaction.scroll: shape type must be 'rect'");
    const auto readNumber = [&](const char* field) {
        lua_getfield(state, shape, field);
        const double value = luaL_checknumber(state, -1);
        lua_pop(state, 1);
        return value;
    };
    const double x = readNumber("x");
    const double y = readNumber("y");
    const double width = readNumber("width");
    const double height = readNumber("height");
    lua_pop(state, 1);
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        std::abs(x) > 1'000'000.0 || std::abs(y) > 1'000'000.0 ||
        width <= 0.0 || height <= 0.0 ||
        width > 1'000'000.0 || height > 1'000'000.0)
    {
        return luaL_error(state,
            "interaction.scroll: shape geometry must be finite, positive, and bounded");
    }

    bool horizontal = false;
    lua_getfield(state, descriptor, "orientation");
    if (!lua_isnil(state, -1))
    {
        std::size_t length = 0;
        const char* value = luaL_checklstring(state, -1, &length);
        const std::string_view orientation(value, length);
        horizontal = orientation == "horizontal";
        if (!horizontal && orientation != "vertical")
            return luaL_error(state,
                "interaction.scroll: orientation must be vertical or horizontal");
    }
    lua_pop(state, 1);
    const char* contentField = horizontal ? "contentWidth" : "contentHeight";
    const char* rejectedField = horizontal ? "contentHeight" : "contentWidth";
    lua_getfield(state, descriptor, rejectedField);
    const bool hasRejectedExtent = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (hasRejectedExtent)
        return luaL_error(state, horizontal
            ? "interaction.scroll: horizontal scrolling uses contentWidth, not contentHeight"
            : "interaction.scroll: vertical scrolling uses contentHeight, not contentWidth");
    lua_getfield(state, descriptor, contentField);
    if (!lua_isinteger(state, -1))
        return luaL_error(state, horizontal
            ? "interaction.scroll: contentWidth must be an integer"
            : "interaction.scroll: contentHeight must be an integer");
    const lua_Integer requestedContentExtent = lua_tointeger(state, -1);
    lua_pop(state, 1);
    if (requestedContentExtent < 0 ||
        requestedContentExtent > 1'000'000)
    {
        return luaL_error(state, horizontal
            ? "interaction.scroll: contentWidth must be from 0 to 1000000"
            : "interaction.scroll: contentHeight must be from 0 to 1000000");
    }

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "interaction.scroll: host context is unavailable");
    const int viewportWidth = std::max(
        1, static_cast<int>(std::lround(width)));
    const int viewportHeight = std::max(
        1, static_cast<int>(std::lround(height)));
    const int viewportExtent = horizontal ? viewportWidth : viewportHeight;
    const int contentExtent = std::max(
        viewportExtent, static_cast<int>(requestedContentExtent));
    int offset = 0;
    {
        const auto widgetId = BoundWidgetId(state);
        const std::string controlId(key);
        LuaWidget::HostControl control;
        control.type = LuaWidget::HostControl::Type::Scroll;
        control.id = controlId;
        control.horizontal = horizontal;
        control.contentWidth = horizontal ? contentExtent : viewportWidth;
        control.viewportWidth = viewportWidth;
        control.contentHeight = horizontal ? viewportHeight : contentExtent;
        control.viewportHeight = viewportHeight;
        control.rect = {
            static_cast<LONG>(std::lround(x)),
            static_cast<LONG>(std::lround(y)),
            static_cast<LONG>(std::lround(x + width)),
            static_cast<LONG>(std::lround(y + height)),
        };
        d2d->engine->RuntimeRegisterHostControl(widgetId, std::move(control));
        offset = d2d->engine->RuntimeGetScrollOffset(widgetId, controlId);
    }
    lua_createtable(state, 0, 7);
    lua_pushinteger(state, offset);
    lua_setfield(state, -2, "offset");
    lua_pushinteger(state, std::max(0, contentExtent - viewportExtent));
    lua_setfield(state, -2, "maximum");
    lua_pushstring(state, horizontal ? "horizontal" : "vertical");
    lua_setfield(state, -2, "orientation");
    lua_pushinteger(state, viewportExtent);
    lua_setfield(state, -2, "viewportExtent");
    lua_pushinteger(state, contentExtent);
    lua_setfield(state, -2, "contentExtent");
    lua_pushinteger(state, viewportExtent);
    lua_setfield(state, -2,
        horizontal ? "viewportWidth" : "viewportHeight");
    lua_pushinteger(state, contentExtent);
    lua_setfield(state, -2,
        horizontal ? "contentWidth" : "contentHeight");
    return 1;
}

int lua_InteractionSetScrollOffset(lua_State* state)
{
    std::size_t keyLength = 0;
    const char* keyValue = luaL_checklstring(state, 1, &keyLength);
    const std::string_view key(keyValue, keyLength);
    if (key.size() > 128 || key.find('\0') != std::string_view::npos ||
        !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "interaction.setScrollOffset: key must contain 1 to 128 bytes of valid UTF-8");
    }
    const lua_Integer requested = luaL_checkinteger(state, 2);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "interaction.setScrollOffset: host context is unavailable");
    int offset = 0;
    {
        const auto widgetId = BoundWidgetId(state);
        const std::string controlId(key);
        d2d->engine->RuntimeSetScrollOffset(widgetId, controlId,
            static_cast<int>(std::clamp<lua_Integer>(requested, 0, 1'000'000)));
        offset = d2d->engine->RuntimeGetScrollOffset(widgetId, controlId);
    }
    lua_pushinteger(state, offset);
    return 1;
}

int lua_UiDurationPicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetDurationPickerLua,
            sizeof(kWidgetDurationPickerLua) - 1, "@host/ui.durationPicker") != LUA_OK)
        return lua_error(state);
    lua_pushvalue(state, 1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.time_picker.hour")); lua_setfield(state, -2, "hours");
    lua_pushstring(state, _L("app.widget.time_picker.minute")); lua_setfield(state, -2, "minutes");
    lua_pushstring(state, _L("app.widget.duration_picker.second")); lua_setfield(state, -2, "seconds");
    lua_pushstring(state, _L("app.widget.duration_picker.invalid")); lua_setfield(state, -2, "invalid");
    lua_pushstring(state, _L("app.widget.duration_picker.increase")); lua_setfield(state, -2, "increase");
    lua_pushstring(state, _L("app.widget.duration_picker.decrease")); lua_setfield(state, -2, "decrease");
    lua_pushstring(state, _L("app.widget.date_picker.confirm")); lua_setfield(state, -2, "confirm");
    lua_call(state, 2, 1);
    return 1;
}

int lua_UiTimePicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetTimePickerLua, sizeof(kWidgetTimePickerLua)-1, "@host/ui.timePicker") != LUA_OK) return lua_error(state);
    lua_pushvalue(state,1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.time_picker.time")); lua_setfield(state,-2,"time");
    lua_pushstring(state, _L("app.widget.time_picker.startTime")); lua_setfield(state,-2,"startTime");
    lua_pushstring(state, _L("app.widget.time_picker.endTime")); lua_setfield(state,-2,"endTime");
    lua_pushstring(state, _L("app.widget.time_picker.hour")); lua_setfield(state,-2,"hour");
    lua_pushstring(state, _L("app.widget.time_picker.minute")); lua_setfield(state,-2,"minute");
    lua_pushstring(state, _L("app.widget.time_picker.invalid")); lua_setfield(state,-2,"invalid");
    lua_pushstring(state, _L("app.widget.time_picker.order")); lua_setfield(state,-2,"order");
    lua_pushstring(state, _L("app.widget.date_picker.clear")); lua_setfield(state,-2,"clear");
    lua_pushstring(state, _L("app.widget.date_picker.confirm")); lua_setfield(state,-2,"confirm");
    lua_call(state,2,1);
    return 1;
}

int lua_UiDatePicker(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    if (luaL_loadbuffer(state, kWidgetDatePickerLua,
            sizeof(kWidgetDatePickerLua) - 1, "@host/ui.datePicker") != LUA_OK)
        return lua_error(state);
    lua_pushvalue(state, 1);
    lua_newtable(state);
    lua_pushstring(state, _L("app.widget.date_picker.date"));
    lua_setfield(state, -2, "date");
    lua_pushstring(state, _L("app.widget.date_picker.startDate"));
    lua_setfield(state, -2, "startDate");
    lua_pushstring(state, _L("app.widget.date_picker.endDate"));
    lua_setfield(state, -2, "endDate");
    lua_pushstring(state, _L("app.widget.date_picker.year"));
    lua_setfield(state, -2, "year");
    lua_pushstring(state, _L("app.widget.date_picker.month"));
    lua_setfield(state, -2, "month");
    lua_pushstring(state, _L("app.widget.date_picker.previous"));
    lua_setfield(state, -2, "previous");
    lua_pushstring(state, _L("app.widget.date_picker.next"));
    lua_setfield(state, -2, "next");
    lua_pushstring(state, _L("app.widget.date_picker.today"));
    lua_setfield(state, -2, "today");
    lua_pushstring(state, _L("app.widget.date_picker.clear"));
    lua_setfield(state, -2, "clear");
    lua_pushstring(state, _L("app.widget.date_picker.confirm"));
    lua_setfield(state, -2, "confirm");
    lua_pushstring(state, _L("app.widget.date_picker.invalid"));
    lua_setfield(state, -2, "invalid");
    lua_pushstring(state, _L("app.widget.date_picker.order"));
    lua_setfield(state, -2, "order");
    lua_pushstring(state, _L("app.widget.date_picker.weekday1"));
    lua_setfield(state, -2, "weekday1");
    lua_pushstring(state, _L("app.widget.date_picker.weekday2"));
    lua_setfield(state, -2, "weekday2");
    lua_pushstring(state, _L("app.widget.date_picker.weekday3"));
    lua_setfield(state, -2, "weekday3");
    lua_pushstring(state, _L("app.widget.date_picker.weekday4"));
    lua_setfield(state, -2, "weekday4");
    lua_pushstring(state, _L("app.widget.date_picker.weekday5"));
    lua_setfield(state, -2, "weekday5");
    lua_pushstring(state, _L("app.widget.date_picker.weekday6"));
    lua_setfield(state, -2, "weekday6");
    lua_pushstring(state, _L("app.widget.date_picker.weekday7"));
    lua_setfield(state, -2, "weekday7");
    lua_call(state, 2, 1);
    return 1;
}

int lua_UiMenu(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    return 1;
}

int lua_UiMetrics(lua_State* L)
{
    auto* d2d = GetD2D(L);
    const WidgetSystemEnvironment system = QueryWidgetSystemEnvironment();
    const auto metrics = snowdesktop::widget_runtime::
        ResolveSemanticUiMetrics(
            d2d ? d2d->semanticUiMetrics :
                snowdesktop::widget_runtime::SemanticUiMetricTokens{},
            d2d ? d2d->semanticCuScale : 1.0f,
            static_cast<float>(system.textScale),
            IsPanelSurface(CurrentWidgetSurface(d2d)) ? 1.0f :
                snowdesktop::widget_runtime::ResolveSemanticRowScale(
                    d2d ? d2d->gridRows : 2));

    lua_createtable(L, 0, 1);
    lua_pushnumber(L, metrics.layoutRowHeight);
    lua_setfield(L, -2, "layoutRowHeight");
    return 1;
}

int lua_WidgetInfo(lua_State* L)
{
    auto* s = GetD2D(L);
    lua_createtable(L, 0, 5);
    if (!s)
        return 1;
    lua_pushstring(L, WideToUtf8(BoundWidgetId(L)).c_str()); lua_setfield(L, -2, "id");
    lua_pushnumber(L, s->widgetRect.right - s->widgetRect.left); lua_setfield(L, -2, "width");
    lua_pushnumber(L, s->widgetRect.bottom - s->widgetRect.top); lua_setfield(L, -2, "height");
    lua_pushboolean(L, s->engine &&
        s->engine->RuntimeIsWidgetSelected(BoundWidgetId(L)));
    lua_setfield(L, -2, "selected");
    const std::wstring selectedPackageId =
        s->engine
            ? s->engine->RuntimeSelectedWidgetPackageId()
            : std::wstring{};
    lua_pushstring(
        L, WideToUtf8(selectedPackageId).c_str());
    lua_setfield(L, -2, "selectedPackageId");
    return 1;
}

int lua_WidgetHasPermission(lua_State* L)
{
    const char* permission = luaL_checkstring(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "__widget_permissions");
    if (!lua_istable(L, -1))
    {
        lua_pop(L, 1);
        lua_pushboolean(L, false);
        return 1;
    }
    lua_getfield(L, -1, permission ? permission : "");
    const bool granted = lua_toboolean(L, -1) != 0;
    lua_pop(L, 2);
    lua_pushboolean(L, granted);
    return 1;
}

int lua_WidgetSetTitle(lua_State* L)
{
    const char* title = luaL_checkstring(L, 1);
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeSetWidgetTitle(BoundWidgetId(L), Utf8ToWide(title ? title : ""));
    return 0;
}

int lua_WidgetOpenSettings(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetSettings(BoundWidgetId(L));
    return 0;
}

int lua_WidgetInvalidate(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeInvalidateHost(BoundWidgetId(L));
    return 0;
}

int lua_WidgetLog(lua_State* L)
{
    const char* level = luaL_optstring(L, 1, "info");
    const char* message = luaL_optstring(L, 2, "");
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeAddLog(BoundWidgetId(L), level ? level : "info", message ? message : "");
    return 0;
}

int lua_WidgetTheme(lua_State* L)
{
    auto* s = GetD2D(L);
    LuaWidgetTheme theme;
    if (s && s->engine)
        theme = s->engine->RuntimeGetWidgetTheme(BoundWidgetId(L));
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, theme.bg); lua_setfield(L, -2, "bg");
    lua_pushinteger(L, theme.border); lua_setfield(L, -2, "border");
    lua_pushnumber(L, theme.alpha); lua_setfield(L, -2, "alpha");
    lua_pushnumber(L, theme.borderAlpha); lua_setfield(L, -2, "borderAlpha");
    lua_pushnumber(L, theme.gradientEndA); lua_setfield(L, -2, "gradientEndA");
    lua_pushnumber(L, theme.cornerRadius); lua_setfield(L, -2, "cornerRadius");
    lua_pushinteger(L, theme.contentTheme); lua_setfield(L, -2, "contentTheme");
    return 1;
}

int lua_WidgetOpenPanel(lua_State* L)
{
    std::string title;
    int width = 520;
    int height = 620;
    if (lua_istable(L, 1))
    {
        title = LuaReadStorageField(L, 1, "title");
        lua_getfield(L, 1, "width");
        if (lua_isnumber(L, -1))
            width = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 1, "height");
        if (lua_isnumber(L, -1))
            height = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
    }
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetPanel(
            BoundWidgetId(L), Utf8ToWide(title),
            width, height);
    return 0;
}

int lua_WidgetClosePanel(lua_State* L)
{
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeCloseWidgetPanel(
            BoundWidgetId(L));
    return 0;
}

int lua_ViewVirtualRange(lua_State* state)
{
    using snowdesktop::widget_runtime::ComputeViewVirtualRange;
    using snowdesktop::widget_runtime::ComputeViewVirtualItemScrollOffset;
    using snowdesktop::widget_runtime::ViewTreeLimits;
    using snowdesktop::widget_runtime::ViewVirtualRange;
    luaL_checktype(state, 1, LUA_TTABLE);
    lua_settop(state, 1);
    const int descriptor = lua_absindex(state, 1);
    std::string error;
    if (!LuaTableUsesOnlyFields(state, descriptor,
            { "key", "itemCount", "itemExtent", "viewportExtent",
                "estimatedItemSize", "layoutRevision", "columns",
                "orientation", "columnGap", "rowGap", "overscan",
                "initialScrollIndex",
                "sectionHeaderIndices" },
            "view.virtualRange", error))
        return luaL_error(state, "view.virtualRange: %s", error.c_str());

    const std::string key = ReadRequiredStringField(
        state, descriptor, "key");
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos || !IsValidUtf8Local(key))
    {
        return luaL_error(state,
            "view.virtualRange: key must contain 1 to 128 bytes of valid UTF-8");
    }
    const auto readInteger = [&](const char* field, lua_Integer fallback) {
        lua_getfield(state, descriptor, field);
        const lua_Integer value = lua_isnil(state, -1)
            ? fallback : luaL_checkinteger(state, -1);
        lua_pop(state, 1);
        return value;
    };
    const auto readNumber = [&](const char* field, double fallback) {
        lua_getfield(state, descriptor, field);
        const double value = lua_isnil(state, -1)
            ? fallback : static_cast<double>(luaL_checknumber(state, -1));
        lua_pop(state, 1);
        return value;
    };
    const auto hasField = [&](const char* field) {
        lua_getfield(state, descriptor, field);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        return present;
    };
    const lua_Integer itemCountValue = readInteger("itemCount", -1);
    const lua_Integer columnsValue = readInteger("columns", 1);
    const lua_Integer overscanValue = readInteger("overscan", 2);
    const lua_Integer initialScrollIndexValue =
        readInteger("initialScrollIndex", 0);
    const lua_Integer layoutRevisionValue =
        readInteger("layoutRevision", 0);
    const double itemExtentValue = readNumber(
        "itemExtent", std::numeric_limits<double>::quiet_NaN());
    const double estimatedItemSizeValue = readNumber(
        "estimatedItemSize", std::numeric_limits<double>::quiet_NaN());
    const double viewportExtentValue = readNumber(
        "viewportExtent", std::numeric_limits<double>::quiet_NaN());
    const double columnGapValue = readNumber("columnGap", 0.0);
    const double rowGapValue = readNumber("rowGap", 0.0);
    std::string orientation = "vertical";
    lua_getfield(state, descriptor, "orientation");
    if (!lua_isnil(state, -1))
    {
        size_t length = 0;
        const char* value = luaL_checklstring(state, -1, &length);
        orientation.assign(value, length);
    }
    lua_pop(state, 1);
    if (orientation != "vertical" && orientation != "horizontal")
        return luaL_error(state,
            "view.virtualRange: orientation must be vertical or horizontal");
    const bool horizontal = orientation == "horizontal";
    if ((horizontal && hasField("rowGap")) ||
        (!horizontal && hasField("columnGap")))
        return luaL_error(state, horizontal
            ? "view.virtualRange: horizontal ranges use columnGap, not rowGap"
            : "view.virtualRange: vertical ranges use rowGap, not columnGap");
    const double mainGapValue = horizontal
        ? columnGapValue : rowGapValue;
    if (itemCountValue < 0 ||
        itemCountValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualItemCount) ||
        columnsValue <= 0 || columnsValue > 64 ||
        overscanValue < 0 ||
        overscanValue > static_cast<lua_Integer>(
            ViewTreeLimits::MaximumVirtualOverscan) ||
        initialScrollIndexValue < 0 ||
        initialScrollIndexValue > itemCountValue ||
        layoutRevisionValue < 0)
    {
        return luaL_error(state,
            "view.virtualRange: integer arguments exceed their limits");
    }

    std::vector<std::size_t> sectionHeaderIndices;
    lua_getfield(state, descriptor, "sectionHeaderIndices");
    if (!lua_isnil(state, -1))
    {
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "view.virtualRange: sectionHeaderIndices must be an array");
        }
        if (!LuaTableIsContiguousArray(state, -1,
                "view.virtualRange.sectionHeaderIndices", error))
        {
            lua_pop(state, 1);
            return luaL_error(state, "view.virtualRange: %s",
                error.c_str());
        }
        const std::size_t count = lua_rawlen(state, -1);
        if (count > ViewTreeLimits::MaximumVirtualSectionHeaders)
        {
            lua_pop(state, 1);
            return luaL_error(state,
                "view.virtualRange: sectionHeaderIndices exceeds its item limit");
        }
        sectionHeaderIndices.reserve(count);
        std::size_t previous = 0;
        for (std::size_t item = 1; item <= count; ++item)
        {
            lua_rawgeti(state, -1, static_cast<lua_Integer>(item));
            const bool valid = lua_isinteger(state, -1) &&
                lua_tointeger(state, -1) > 0 &&
                lua_tointeger(state, -1) <= itemCountValue &&
                static_cast<std::size_t>(lua_tointeger(state, -1)) >
                    previous;
            if (!valid)
            {
                lua_pop(state, 2);
                return luaL_error(state,
                    "view.virtualRange: sectionHeaderIndices must be sorted unique bounded 1-based indices");
            }
            previous = static_cast<std::size_t>(
                lua_tointeger(state, -1));
            sectionHeaderIndices.push_back(previous);
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);

    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "view.virtualRange: host context is unavailable");
    const bool fixedExtent = std::isfinite(itemExtentValue);
    const bool variableExtent = std::isfinite(estimatedItemSizeValue);
    if (fixedExtent == variableExtent ||
        (variableExtent && columnsValue != 1) ||
        (!sectionHeaderIndices.empty() && columnsValue != 1) ||
        (horizontal && (columnsValue != 1 ||
            !sectionHeaderIndices.empty())))
    {
        return luaL_error(state,
            "view.virtualRange: provide exactly one extent; horizontal ranges require one list column and reject section headers");
    }
    ViewVirtualRange range;
    ViewVirtualRange visibleRange;
    if (variableExtent)
    {
        if (!d2d->engine->RuntimeComputeVariableVirtualRange(
                BoundWidgetId(state), key,
                static_cast<std::size_t>(itemCountValue),
                static_cast<float>(estimatedItemSizeValue),
                static_cast<float>(mainGapValue),
                static_cast<float>(viewportExtentValue),
                horizontal,
                static_cast<std::uint64_t>(layoutRevisionValue),
                static_cast<std::size_t>(overscanValue),
                static_cast<std::size_t>(initialScrollIndexValue),
                range, error))
            return luaL_error(state, "view.virtualRange: %s", error.c_str());
        if (!d2d->engine->RuntimeComputeVariableVirtualRange(
                BoundWidgetId(state), key,
                static_cast<std::size_t>(itemCountValue),
                static_cast<float>(estimatedItemSizeValue),
                static_cast<float>(mainGapValue),
                static_cast<float>(viewportExtentValue),
                horizontal,
                static_cast<std::uint64_t>(layoutRevisionValue), 0,
                static_cast<std::size_t>(initialScrollIndexValue),
                visibleRange, error))
            return luaL_error(state, "view.virtualRange: %s", error.c_str());
    }
    else
    {
        float requestedOffset = static_cast<float>(
            d2d->engine->RuntimeGetScrollOffset(BoundWidgetId(state), key));
        if (initialScrollIndexValue > 0 &&
            !d2d->engine->RuntimeHasScrollOffset(
                BoundWidgetId(state), key))
        {
            if (!ComputeViewVirtualItemScrollOffset(
                    static_cast<std::size_t>(itemCountValue),
                    static_cast<float>(itemExtentValue),
                    static_cast<std::size_t>(columnsValue),
                    static_cast<float>(mainGapValue),
                    static_cast<float>(viewportExtentValue), 0.0f,
                    static_cast<std::size_t>(initialScrollIndexValue),
                    "nearest", requestedOffset, error))
                return luaL_error(state,
                    "view.virtualRange: %s", error.c_str());
        }
        if (!ComputeViewVirtualRange(
            static_cast<std::size_t>(itemCountValue),
            static_cast<float>(itemExtentValue),
            static_cast<std::size_t>(columnsValue),
            static_cast<float>(mainGapValue),
            static_cast<float>(viewportExtentValue),
            requestedOffset,
            static_cast<std::size_t>(overscanValue), range, error))
            return luaL_error(state,
                "view.virtualRange: %s", error.c_str());
        if (!ComputeViewVirtualRange(
            static_cast<std::size_t>(itemCountValue),
            static_cast<float>(itemExtentValue),
            static_cast<std::size_t>(columnsValue),
            static_cast<float>(mainGapValue),
            static_cast<float>(viewportExtentValue), range.offset, 0,
            visibleRange, error))
            return luaL_error(state,
                "view.virtualRange: %s", error.c_str());
    }

    std::optional<std::size_t> stickyHeaderIndex;
    const auto nextHeader = std::upper_bound(sectionHeaderIndices.begin(),
        sectionHeaderIndices.end(), visibleRange.firstIndex);
    if (nextHeader != sectionHeaderIndices.begin())
        stickyHeaderIndex = *std::prev(nextHeader);

    lua_createtable(state, 0, 7);
    lua_pushinteger(state, static_cast<lua_Integer>(range.firstIndex));
    lua_setfield(state, -2, "firstIndex");
    lua_pushinteger(state, static_cast<lua_Integer>(range.lastIndex));
    lua_setfield(state, -2, "lastIndex");
    lua_pushnumber(state, range.offset);
    lua_setfield(state, -2, "offset");
    lua_pushnumber(state, range.maximum);
    lua_setfield(state, -2, "maximum");
    lua_pushnumber(state, range.viewportExtent);
    lua_setfield(state, -2, "viewportExtent");
    lua_pushnumber(state, range.contentExtent);
    lua_setfield(state, -2, "contentExtent");
    if (stickyHeaderIndex)
    {
        lua_pushinteger(state,
            static_cast<lua_Integer>(*stickyHeaderIndex));
        lua_setfield(state, -2, "stickyHeaderIndex");
    }
    return 1;
}

static std::string LuaViewScrollKey(lua_State* state, const char* api)
{
    std::size_t length = 0;
    const char* raw = luaL_checklstring(state, 1, &length);
    std::string key(raw ? raw : "", length);
    if (key.empty() || key.size() > 128 ||
        key.find('\0') != std::string::npos || !IsValidUtf8Local(key))
    {
        luaL_error(state,
            "%s: key must contain 1 to 128 bytes of valid UTF-8", api);
    }
    return key;
}

static int PushViewScrollResult(lua_State* state, bool succeeded,
    int offset, int maximum, bool changed, const std::string& error)
{
    if (!succeeded)
    {
        lua_pushnil(state);
        lua_pushlstring(state, error.data(), error.size());
        return 2;
    }
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, offset);
    lua_setfield(state, -2, "offset");
    lua_pushinteger(state, maximum);
    lua_setfield(state, -2, "maximum");
    lua_pushboolean(state, changed ? 1 : 0);
    lua_setfield(state, -2, "changed");
    return 1;
}

static int LuaViewScroll(lua_State* state, bool relative)
{
    const char* api = relative ? "view.scrollBy" : "view.scrollTo";
    if (lua_gettop(state) != 2)
        return luaL_error(state, "%s: expected key and offset", api);
    const std::string key = LuaViewScrollKey(state, api);
    const lua_Integer requested = luaL_checkinteger(state, 2);
    if ((!relative && requested < 0) || requested < -1'000'000 ||
        requested > 1'000'000)
        return luaL_error(state, "%s: offset exceeds its limit", api);
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state, "%s: host context is unavailable", api);
    if (!d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state)))
        return luaL_error(state, "%s cannot mutate scroll state during render", api);
    int offset = 0;
    int maximum = 0;
    bool changed = false;
    std::string error;
    const bool succeeded = d2d->engine->RuntimeScrollView(
        BoundWidgetId(state), key, static_cast<int>(requested), relative,
        offset, maximum, changed, error);
    return PushViewScrollResult(
        state, succeeded, offset, maximum, changed, error);
}

int lua_ViewScrollTo(lua_State* state)
{
    return LuaViewScroll(state, false);
}

int lua_ViewScrollBy(lua_State* state)
{
    return LuaViewScroll(state, true);
}

int lua_ViewScrollToIndex(lua_State* state)
{
    if (lua_gettop(state) < 2 || lua_gettop(state) > 3)
        return luaL_error(state,
            "view.scrollToIndex: expected key, index, and optional alignment");
    const std::string key = LuaViewScrollKey(state, "view.scrollToIndex");
    const lua_Integer index = luaL_checkinteger(state, 2);
    if (index <= 0 || index > 1'000'000)
        return luaL_error(state,
            "view.scrollToIndex: index must be between 1 and 1000000");
    const std::string_view alignment = luaL_optstring(state, 3, "nearest");
    if (alignment != "nearest" && alignment != "start" &&
        alignment != "center" && alignment != "end")
        return luaL_error(state,
            "view.scrollToIndex: alignment must be nearest, start, center, or end");
    auto* d2d = GetD2D(state);
    if (!d2d || !d2d->engine)
        return luaL_error(state,
            "view.scrollToIndex: host context is unavailable");
    if (!d2d->engine->RuntimeCanWriteWidgetStorage(BoundWidgetId(state)))
        return luaL_error(state,
            "view.scrollToIndex cannot mutate scroll state during render");
    int offset = 0;
    int maximum = 0;
    bool changed = false;
    std::string error;
    const bool succeeded = d2d->engine->RuntimeScrollViewToIndex(
        BoundWidgetId(state), key, static_cast<std::size_t>(index),
        alignment, offset, maximum, changed, error);
    return PushViewScrollResult(
        state, succeeded, offset, maximum, changed, error);
}

int lua_WidgetOpenDialog(lua_State* L)
{
    std::string title;
    int width = 520;
    int height = 420;
    bool dismissOnOutside = false;
    bool dismissOnEscape = true;
    if (lua_istable(L, 1))
    {
        title = LuaReadStorageField(L, 1, "title");
        lua_getfield(L, 1, "width");
        if (lua_isnumber(L, -1))
            width = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 1, "height");
        if (lua_isnumber(L, -1))
            height = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        lua_getfield(L, 1, "dismissOnOutside");
        if (!lua_isnil(L, -1))
            dismissOnOutside = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 1, "dismissOnEscape");
        if (!lua_isnil(L, -1))
            dismissOnEscape = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    auto* s = GetD2D(L);
    if (s && s->engine)
        s->engine->RuntimeOpenWidgetDialog(
            BoundWidgetId(L), Utf8ToWide(title),
            width, height, dismissOnOutside,
            dismissOnEscape);
    return 0;
}

int lua_WidgetOpenPopover(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    const std::string title =
        LuaReadStorageField(L, 1, "title");
    const std::string anchorKey =
        LuaReadStorageField(L, 1, "anchorKey");
    std::string placement =
        LuaReadStorageField(L, 1, "placement");
    if (placement.empty()) placement = "auto";
    int width = 360;
    int height = 280;
    bool dismissOnOutside = true;
    bool dismissOnEscape = true;
    lua_getfield(L, 1, "width");
    if (lua_isnumber(L, -1))
        width = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "height");
    if (lua_isnumber(L, -1))
        height = static_cast<int>(lua_tointeger(L, -1));
    lua_pop(L, 1);
    lua_getfield(L, 1, "dismissOnOutside");
    if (!lua_isnil(L, -1))
        dismissOnOutside = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_getfield(L, 1, "dismissOnEscape");
    if (!lua_isnil(L, -1))
        dismissOnEscape = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    auto* s = GetD2D(L);
    const bool opened = s && s->engine &&
        s->engine->RuntimeOpenWidgetPopover(
            BoundWidgetId(L), Utf8ToWide(title),
            anchorKey, std::move(placement), width, height,
            dismissOnOutside, dismissOnEscape);
    lua_pushboolean(L, opened ? 1 : 0);
    return 1;
}

void WidgetEngine::CloseWidgetPanelSurface(
    const std::wstring& widgetId, std::string_view surface)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    auto& widget = widgets_[index];
    const std::string closingSurface = IsPanelSurface(surface)
        ? NormalizeWidgetSurface(surface)
        : NormalizeWidgetSurface(widget.panelSurface);
    CloseHostInputPanel(widgetId);
    widget.panelActive = false;
    if (d2dState_)
        for (const char* closing : {"panel", "dialog", "popover"})
            d2dState_->packageImageCache.ForgetOwner(
                WideToUtf8(widgetId) + "|" + closing);
    if (d2dState_) d2dState_->textLayoutCache.Erase(widgetId, closingSurface);
    ApplyWidgetHostVisibility(
        widget, widget.desktopVisible || widget.panelActive ||
            widget.keepRuntimeActiveForHiddenPage);
    widget.panelSurface = "panel";
    widget.panelInitialKeyboardFocusPending = false;
    widget.panelViewTransitions.Clear();
    widget.panelViewTransitionFramePending = false;
    widget.panelViewKeyboardFocusKey.clear();
    widget.panelViewFocusCueVisible = false;
    widget.panelInteractionRegions.CancelPointerPress();
    const auto transition = widget.panelInteractionRegions.ClearHover();
    if (transition.Changed())
        DispatchInteractionTransition(
            widget, transition, 0, 0, closingSurface);
    std::erase_if(widget.hostControls, [](const auto& control) {
        return IsPanelSurface(control.surface);
    });
    std::erase_if(pressedViewKeyTargets_,
        [&widgetId](const auto& entry) {
            return entry.second.widgetId == widgetId &&
                IsPanelSurface(entry.second.surface);
        });
}

LuaWidgetTheme WidgetEngine::RuntimeGetWidgetTheme(const std::wstring& widgetId) const
{
    int idx = FindWidget(widgetId);
    const auto desktop = idx >= 0 ? widgets_[idx].theme : LuaWidgetTheme{};
    const auto surface = d2dState_ && d2dState_->currentWidgetId == widgetId
        ? CurrentWidgetSurface(d2dState_) : std::string_view("desktop");
    return snowdesktop::widget_runtime::ResolveSurfaceTheme(
        desktop, panelTheme_ ? &*panelTheme_ : nullptr, surface);
}

void WidgetEngine::SetPanelTheme(const PersonalizationSettings& appearance)
{
    const auto rgb = [](float r, float g, float b) {
        const auto byte = [](float v) {
            return static_cast<int>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        return (byte(r) << 16) | (byte(g) << 8) | byte(b);
    };
    LuaWidgetTheme theme;
    theme.bg = rgb(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB);
    theme.border = rgb(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB);
    theme.alpha = appearance.widgetAlpha;
    theme.borderAlpha = appearance.widgetBorderAlpha;
    theme.gradientEndA = appearance.gradientEndA;
    theme.cornerRadius = 18.0f;
    theme.contentTheme = appearance.contentTheme;
    panelTheme_ = theme;
}

void WidgetEngine::SetWidgetTheme(const std::wstring& widgetId, const LuaWidgetTheme& theme)
{
    int idx = FindWidget(widgetId);
    if (idx >= 0)
        widgets_[idx].theme = theme;
}

void WidgetEngine::SetWidgetLayoutMetrics(
    const std::wstring& widgetId,
    int columns, int rows, int cellWidth, int cellHeight,
    int gapY, int barHeight,
    DWRITE_FONT_WEIGHT fontWeight, float semanticCuScale,
    const snowdesktop::widget_runtime::SemanticUiMetricTokens&
        semanticUiMetrics)
{
    const int index = FindWidget(widgetId);
    const auto metrics = snowdesktop::widget_runtime::NormalizeLayoutMetrics(
            cellWidth, cellHeight, gapY, barHeight,
            fontWeight, semanticCuScale, semanticUiMetrics);
    if (index >= 0)
        widgets_[index].layoutMetrics = metrics;
    // A package chunk and its setup callback may query ui.metrics() before the
    // LuaWidget has entered widgets_. Keep the pending execution state aligned
    // with the instance and span that the host is about to load.
    if (d2dState_)
    {
        snowdesktop::widget_runtime::ApplyLayoutSpan(
            *d2dState_, columns, rows);
        snowdesktop::widget_runtime::ApplyLayoutMetrics(*d2dState_, metrics);
    }
}

void WidgetEngine::SetWidgetSurfaceContext(
    const std::wstring& widgetId,
    const LuaWidgetSurfaceContext& context)
{
    const int index = FindWidget(widgetId);
    if (index < 0) return;
    widgets_[index].surfaceContext = context;
    widgets_[index].surfaceContext.dpiX =
        std::max<UINT>(USER_DEFAULT_SCREEN_DPI, context.dpiX);
    widgets_[index].surfaceContext.dpiY =
        std::max<UINT>(USER_DEFAULT_SCREEN_DPI, context.dpiY);
}

void WidgetEngine::SetDesktopImageSpinOutput(
    snowdesktop::widget_runtime::WidgetImageSpin* output)
{
    if (d2dState_) d2dState_->desktopImageSpin = output;
}

void WidgetEngine::SetDesktopSurfacePaintOutput(
    snowdesktop::widget_runtime::WidgetSurfacePaint* output)
{
    if (d2dState_) d2dState_->desktopSurfacePaint = output;
}

LuaWidgetContextState WidgetEngine::RuntimeGetWidgetContextState(
    const std::wstring& widgetId) const
{
    LuaWidgetContextState result;
    const int index = FindWidget(widgetId);
    if (index < 0) return result;
    const LuaWidget& widget = widgets_[index];
    result.surface = widget.surfaceContext;
    result.visible = widget.hostVisible;
    result.preview = widget.preview;
    result.focused = !widget.viewKeyboardFocusKey.empty() ||
        !widget.panelViewKeyboardFocusKey.empty() ||
        HasFocusedHostInputFor(widgetId);
    result.selected = widgetSelectedProvider_
        ? widgetSelectedProvider_(widgetId) : false;
    return result;
}

void WidgetEngine::RuntimeOpenWidgetSettings(const std::wstring& widgetId)
{
    if (snowdesktop::widget_runtime::IsDryLoad()) return;
    if (openWidgetSettingsCallback_)
        openWidgetSettingsCallback_(widgetId, L"");
}

void WidgetEngine::PrepareWidgetPanelFocus(const std::wstring& widgetId)
{
    if (const int index = FindWidget(widgetId); index >= 0)
    {
        widgets_[index].panelInitialKeyboardFocusPending = true;
        widgets_[index].panelViewKeyboardFocusKey.clear();
        widgets_[index].panelViewFocusCueVisible = false;
    }
}

void WidgetEngine::RuntimeOpenWidgetPanel(
    const std::wstring& widgetId, std::wstring title,
    int width, int height)
{
    if (snowdesktop::widget_runtime::IsDryLoad() || !openWidgetPanelCallback_)
        return;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    if (request.desktopGesture && d2dState_)
        request.desktopOpenerKey = d2dState_->desktopPopupOpenerKey;
    request.title = std::move(title);
    request.surface = "panel";
    request.width = std::clamp(width, 320, 900);
    request.height = std::clamp(height, 280, 900);
    const std::string_view sourceSurface = CurrentWidgetSurface(d2dState_);
    const int index = FindWidget(widgetId);
    if (trustedGestureState_.Active() && IsPanelSurface(sourceSurface) && index >= 0)
    {
        const auto& widget = widgets_[index];
        float x = 0.0f, y = 0.0f;
        if (InteractionRegionsForSurface(widget, sourceSurface).LastPointer(x, y))
        {
            const LONG left = widget.panelBounds.left + static_cast<LONG>(std::floor(x));
            const LONG top = widget.panelBounds.top + static_cast<LONG>(std::floor(y));
            request.hasAnchor = true;
            request.anchorRect = {left, top, left, top};
        }
    }
    openWidgetPanelCallback_(request);
}

void WidgetEngine::RuntimeOpenWidgetDialog(
    const std::wstring& widgetId, std::wstring title,
    int width, int height, bool dismissOnOutside,
    bool dismissOnEscape)
{
    if (snowdesktop::widget_runtime::IsDryLoad() ||
        !openWidgetPanelCallback_)
        return;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    if (request.desktopGesture && d2dState_)
        request.desktopOpenerKey = d2dState_->desktopPopupOpenerKey;
    request.title = std::move(title);
    request.surface = "dialog";
    request.width = std::clamp(width, 320, 900);
    request.height = std::clamp(height, 280, 900);
    request.dismissOnOutside = dismissOnOutside;
    request.dismissOnEscape = dismissOnEscape;
    request.modal = true;
    openWidgetPanelCallback_(request);
}

bool WidgetEngine::RuntimeOpenWidgetPopover(
    const std::wstring& widgetId, std::wstring title,
    std::string anchorKey, std::string placement,
    int width, int height, bool dismissOnOutside,
    bool dismissOnEscape)
{
    if (snowdesktop::widget_runtime::IsDryLoad() ||
        !openWidgetPanelCallback_ ||
        !trustedGestureState_.Active() || anchorKey.empty() ||
        CurrentWidgetSurface(d2dState_) != "desktop")
        return false;
    static constexpr std::array<std::string_view, 9> placements = {
        "auto", "top", "bottom", "left", "right",
        "topStart", "topEnd", "bottomStart", "bottomEnd"
    };
    if (std::find(placements.begin(), placements.end(), placement) ==
        placements.end())
        return false;
    const int index = FindWidget(widgetId);
    if (index < 0 || !widgets_[index].valid ||
        widgets_[index].preview)
        return false;
    const auto* anchor =
        widgets_[index].interactionRegions.Find(anchorKey);
    if (!anchor || !anchor->enabled)
        return false;
    float left = anchor->shape.x;
    float top = anchor->shape.y;
    float right = anchor->shape.x + anchor->shape.width;
    float bottom = anchor->shape.y + anchor->shape.height;
    if (anchor->shape.type ==
        snowdesktop::widget_runtime::InteractionShapeType::Circle)
    {
        left = anchor->shape.x - anchor->shape.radius;
        top = anchor->shape.y - anchor->shape.radius;
        right = anchor->shape.x + anchor->shape.radius;
        bottom = anchor->shape.y + anchor->shape.radius;
    }
    if (anchor->clip)
    {
        left = std::max(left, anchor->clip->x);
        top = std::max(top, anchor->clip->y);
        right = std::min(right,
            anchor->clip->x + anchor->clip->width);
        bottom = std::min(bottom,
            anchor->clip->y + anchor->clip->height);
    }
    if (right <= left || bottom <= top)
        return false;
    const RECT ownerBounds = widgets_[index].lastBounds;
    LuaWidgetPanelRequest request;
    request.widgetId = widgetId;
    request.desktopGesture = trustedGestureState_.Active() &&
        CurrentWidgetSurface(d2dState_) == "desktop";
    request.title = std::move(title);
    request.surface = "popover";
    request.anchorKey = anchorKey;
    request.placement = std::move(placement);
    request.anchorRect = {
        ownerBounds.left + static_cast<LONG>(std::floor(left)),
        ownerBounds.top + static_cast<LONG>(std::floor(top)),
        ownerBounds.left + static_cast<LONG>(std::ceil(right)),
        ownerBounds.top + static_cast<LONG>(std::ceil(bottom))
    };
    request.hasAnchor = true;
    request.showHeader = !request.title.empty();
    request.width = std::clamp(width, 200, 720);
    request.height = std::clamp(height, 120, 720);
    request.dismissOnOutside = dismissOnOutside;
    request.dismissOnEscape = dismissOnEscape;
    openWidgetPanelCallback_(request);
    return true;
}

void WidgetEngine::RuntimeCloseWidgetPanel(
    const std::wstring& widgetId)
{
    if (!snowdesktop::widget_runtime::IsDryLoad() && closeWidgetPanelCallback_)
        closeWidgetPanelCallback_(widgetId);
}

bool WidgetEngine::RuntimeSubmitNativeMarquee(
    const std::wstring& widgetId,
    LuaWidget::NativeMarqueeText marquee, std::string& error)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "host widget instance is unavailable";
        return false;
    }
    auto& widget = widgets_[index];
    if (!WidgetDeclaresNativeMarquee(widget))
    {
        error = "the package must declare draw.marqueeText as a required or optional feature";
        return false;
    }
    if (CurrentWidgetSurface(d2dState_) != "desktop")
    {
        error = "only the desktop render surface is supported";
        return false;
    }
    auto& surface = widget.desktopMarquee;
    if (!surface.collecting)
    {
        error = "the API is only available while the host records render()";
        return false;
    }
    if (surface.pendingMarquees.size() >= 32)
    {
        error = "a render may submit at most 32 marquee texts";
        return false;
    }
    if (std::any_of(surface.pendingMarquees.begin(),
            surface.pendingMarquees.end(), [&marquee](const auto& value) {
                return value.key == marquee.key;
            }))
    {
        error = "duplicate key: " + marquee.key;
        return false;
    }
    surface.pendingMarquees.push_back(std::move(marquee));
    error.clear();
    return true;
}

bool WidgetEngine::RuntimeSubmitInteractionRegion(
    const std::wstring& widgetId,
    snowdesktop::widget_runtime::InteractionRegion region,
    std::string& error)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
    {
        error = "interaction regions require API v2";
        return false;
    }
    auto& regions = InteractionRegionsForSurface(
        widgets_[index], CurrentWidgetSurface(d2dState_));
    return regions.Submit(
        std::move(region), error);
}

bool WidgetEngine::RuntimeInteractionHovered(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)).IsHovered(key);
}

bool WidgetEngine::RuntimeInteractionPressed(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 &&
        InteractionRegionsForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)).IsPressed(key);
}

bool WidgetEngine::RuntimeInteractionFocused(
    const std::wstring& widgetId, std::string_view key) const
{
    const int index = FindWidget(widgetId);
    return index >= 0 && !key.empty() &&
        ViewFocusForSurface(widgets_[index],
            CurrentWidgetSurface(d2dState_)) == key;
}
