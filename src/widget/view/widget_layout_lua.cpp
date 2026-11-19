#include "widget_layout_lua.h"

#include "common/constants.h"
#include "common/utils.h"
#include "ui/render/font_cu_rules.h"
#include "widget/runtime/widget_layout_context.h"
#include "widgets/widget_chrome_rules.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
namespace
{
const char kWidgetLayoutContextKey = 0;

WidgetLayoutContext* GetWidgetLayoutContext(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kWidgetLayoutContextKey);
    auto* context = static_cast<WidgetLayoutContext*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return context;
}
}

void BindWidgetLayoutContext(lua_State* state, WidgetLayoutContext* context)
{
    lua_pushlightuserdata(state, context);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kWidgetLayoutContextKey);
}

float CurrentLayoutContentWidth(const WidgetLayoutContext* state) noexcept
{
    if (!state) return 400.0f;
    if (state->layoutContentWidth > 0.0f)
        return state->layoutContentWidth;
    return std::max(1.0f,
        state->widgetRect.right - state->widgetRect.left);
}

float CurrentLayoutContentHeight(const WidgetLayoutContext* state) noexcept
{
    if (!state) return 300.0f;
    if (state->layoutContentHeight > 0.0f)
        return state->layoutContentHeight;
    return std::max(1.0f,
        state->widgetRect.bottom - state->widgetRect.top);
}

static int ReadPositiveRegistryInteger(
    lua_State* state, const char* key, int fallback = 1)
{
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    const int result = lua_isinteger(state, -1)
        ? std::max(1, static_cast<int>(lua_tointeger(state, -1)))
        : std::max(1, fallback);
    lua_pop(state, 1);
    return result;
}

static bool ReadRegistryBoolean(
    lua_State* state, const char* key, bool fallback)
{
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    const bool result = lua_isboolean(state, -1)
        ? lua_toboolean(state, -1) != 0
        : fallback;
    lua_pop(state, 1);
    return result;
}

int lua_LayoutWidth(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushnumber(L, s ? s->widgetRect.right - s->widgetRect.left : 400);
    return 1;
}

int lua_LayoutHeight(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushnumber(L, s ? s->widgetRect.bottom - s->widgetRect.top : 300);
    return 1;
}

int lua_LayoutColumns(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? s->gridColumns : 1);
    return 1;
}

int lua_LayoutRows(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? s->gridRows : 1);
    return 1;
}

int lua_LayoutSizeClass(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    int area = s ? s->gridColumns * s->gridRows : 1;
    lua_pushstring(L, area <= 2 ? "small" : (area <= 6 ? "medium" : "large"));
    return 1;
}

int lua_LayoutCellWidth(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? std::max(4, s->gridCellW) : 92);
    return 1;
}

int lua_LayoutCellHeight(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? std::max(4, s->gridCellH) : 116);
    return 1;
}

int lua_LayoutCellScale(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushnumber(L, CalculateWidgetCellScale(cellWidth, cellHeight));
    return 1;
}

int lua_LayoutCu(lua_State* L)
{
    const float value = static_cast<float>(luaL_checknumber(L, 1));
    auto* s = GetWidgetLayoutContext(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushinteger(L, static_cast<lua_Integer>(std::round(value * CalculateWidgetCellScale(cellWidth, cellHeight))));
    return 1;
}

int lua_LayoutFontCu(lua_State* L)
{
    const float value = static_cast<float>(luaL_checknumber(L, 1));
    auto* s = GetWidgetLayoutContext(L);
    const int cellWidth = s ? std::max(4, s->gridCellW) : kCellWidth;
    const int cellHeight = s ? std::max(4, s->gridCellH) : kMinCellHeight;
    lua_pushnumber(L, snowdesktop::font_cu_rules::Scale(
        value, CalculateWidgetCellScale(cellWidth, cellHeight)));
    return 1;
}

int lua_LayoutCellGap(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? std::max(0, s->gridGapY) : 8);
    return 1;
}

int lua_LayoutBarHeight(lua_State* L)
{
    auto* s = GetWidgetLayoutContext(L);
    lua_pushinteger(L, s ? std::max(16, s->barHeight) : 24);
    return 1;
}

int lua_LayoutContentWidth(lua_State* L)
{
    lua_pushnumber(L, CurrentLayoutContentWidth(GetWidgetLayoutContext(L)));
    return 1;
}

int lua_LayoutContentHeight(lua_State* L)
{
    lua_pushnumber(L, CurrentLayoutContentHeight(GetWidgetLayoutContext(L)));
    return 1;
}

static int PushRelativeLayoutUnit(
    lua_State* L, float extent, const char* functionName)
{
    const double percent = luaL_checknumber(L, 1);
    if (!std::isfinite(percent) || percent < 0.0 || percent > 100.0)
    {
        return luaL_error(L,
            "%s: percent must be finite and between 0 and 100",
            functionName);
    }
    lua_pushnumber(L,
        static_cast<double>(extent) * percent / 100.0);
    return 1;
}

int lua_LayoutVw(lua_State* L)
{
    return PushRelativeLayoutUnit(
        L, CurrentLayoutContentWidth(GetWidgetLayoutContext(L)), "layout.vw");
}

int lua_LayoutVh(lua_State* L)
{
    return PushRelativeLayoutUnit(
        L, CurrentLayoutContentHeight(GetWidgetLayoutContext(L)), "layout.vh");
}

int lua_LayoutVmin(lua_State* L)
{
    auto* state = GetWidgetLayoutContext(L);
    return PushRelativeLayoutUnit(L, std::min(
        CurrentLayoutContentWidth(state),
        CurrentLayoutContentHeight(state)), "layout.vmin");
}

int lua_LayoutVmax(lua_State* L)
{
    auto* state = GetWidgetLayoutContext(L);
    return PushRelativeLayoutUnit(L, std::max(
        CurrentLayoutContentWidth(state),
        CurrentLayoutContentHeight(state)), "layout.vmax");
}

static double CheckReferencePixelValue(
    lua_State* state, const char* functionName)
{
    const double value = luaL_checknumber(state, 1);
    if (!std::isfinite(value) || std::abs(value) > 1000000.0)
    {
        luaL_error(state,
            "%s: value must be finite and between -1000000 and 1000000",
            functionName);
        return 0.0;
    }
    return value;
}

static float ReferenceAxisContentHeight(lua_State* state, WidgetLayoutContext* d2d)
{
    const float fullHeight = snowdesktop::widget_runtime::
        ReferenceSpanHeight(ReadPositiveRegistryInteger(
            state, "__widget_default_rows"));
    if (!d2d || std::strcmp(d2d->surfaceKind, "desktop") != 0)
        return fullHeight;
    const bool showTitle = ReadRegistryBoolean(
        state, "__widget_show_title", false);
    const bool bottomBarHover = ReadRegistryBoolean(
        state, "__widget_bottom_bar_hover", true);
    const int reservedBarHeight = snowdesktop::widget_chrome_rules::
        ReservedBottomBarHeight(showTitle, bottomBarHover,
            std::max(0, d2d->barHeight));
    return std::max(1.0f,
        fullHeight - static_cast<float>(reservedBarHeight));
}

static float CurrentAxisContentHeight(lua_State* state, WidgetLayoutContext* d2d)
{
    if (!d2d || std::strcmp(d2d->surfaceKind, "desktop") != 0)
        return CurrentLayoutContentHeight(d2d);
    const bool showTitle = ReadRegistryBoolean(
        state, "__widget_show_title", false);
    const bool bottomBarHover = ReadRegistryBoolean(
        state, "__widget_bottom_bar_hover", true);
    const int scaledBarHeight = static_cast<int>(std::round(
        static_cast<float>(d2d->barHeight) *
        CalculateWidgetCellScale(
            std::max(4, d2d->gridCellW),
            std::max(4, d2d->gridCellH))));
    const int reservedBarHeight = snowdesktop::widget_chrome_rules::
        ReservedBottomBarHeight(
            showTitle, bottomBarHover, scaledBarHeight);
    return std::max(1.0f,
        d2d->widgetRect.bottom - d2d->widgetRect.top -
            static_cast<float>(reservedBarHeight));
}

int lua_LayoutRpx(lua_State* L)
{
    const double value = CheckReferencePixelValue(L, "layout.rpx");
    auto* state = GetWidgetLayoutContext(L);
    const float scaled = snowdesktop::widget_runtime::ScaleReferencePixel(
        static_cast<float>(value),
        CurrentLayoutContentWidth(state),
        CurrentLayoutContentHeight(state),
        snowdesktop::widget_runtime::ReferenceSpanWidth(
            ReadPositiveRegistryInteger(L, "__widget_default_columns")),
        ReferenceAxisContentHeight(L, state));
    lua_pushnumber(L, scaled);
    return 1;
}

int lua_LayoutRpxX(lua_State* L)
{
    const double value = CheckReferencePixelValue(L, "layout.rpxX");
    auto* state = GetWidgetLayoutContext(L);
    const float scaled = snowdesktop::widget_runtime::ScaleReferenceAxis(
        static_cast<float>(value), CurrentLayoutContentWidth(state),
        snowdesktop::widget_runtime::ReferenceSpanWidth(
            ReadPositiveRegistryInteger(
                L, "__widget_default_columns")));
    lua_pushnumber(L, scaled);
    return 1;
}

int lua_LayoutRpxY(lua_State* L)
{
    const double value = CheckReferencePixelValue(L, "layout.rpxY");
    auto* state = GetWidgetLayoutContext(L);
    const float scaled = snowdesktop::widget_runtime::ScaleReferenceAxis(
        static_cast<float>(value), CurrentAxisContentHeight(L, state),
        ReferenceAxisContentHeight(L, state));
    lua_pushnumber(L, scaled);
    return 1;
}

}
