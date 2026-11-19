#pragma once

#include "widget_draw_lua.h"

namespace snowdesktop::widget_runtime
{
// Layout values follow the render context across desktop and auxiliary surfaces.
struct WidgetLayoutContext : LuaDrawContext
{
    int gridColumns = 1;
    int gridRows = 1;
    int gridGapY = 8;
    int barHeight = 24;
    float layoutContentWidth = 0.0f;
    float layoutContentHeight = 0.0f;
};

void BindWidgetLayoutContext(lua_State* state, WidgetLayoutContext* context);
float CurrentLayoutContentWidth(const WidgetLayoutContext* state) noexcept;
float CurrentLayoutContentHeight(const WidgetLayoutContext* state) noexcept;

int lua_LayoutWidth(lua_State* state);
int lua_LayoutHeight(lua_State* state);
int lua_LayoutColumns(lua_State* state);
int lua_LayoutRows(lua_State* state);
int lua_LayoutSizeClass(lua_State* state);
int lua_LayoutCellWidth(lua_State* state);
int lua_LayoutCellHeight(lua_State* state);
int lua_LayoutCellScale(lua_State* state);
int lua_LayoutCu(lua_State* state);
int lua_LayoutFontCu(lua_State* state);
int lua_LayoutCellGap(lua_State* state);
int lua_LayoutBarHeight(lua_State* state);
int lua_LayoutContentWidth(lua_State* state);
int lua_LayoutContentHeight(lua_State* state);
int lua_LayoutVw(lua_State* state);
int lua_LayoutVh(lua_State* state);
int lua_LayoutVmin(lua_State* state);
int lua_LayoutVmax(lua_State* state);
int lua_LayoutRpx(lua_State* state);
int lua_LayoutRpxX(lua_State* state);
int lua_LayoutRpxY(lua_State* state);
}
