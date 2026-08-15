#pragma once

#include "widget_view_tree.h"

#include <string>

extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
bool ParseLuaViewTree(lua_State* state, int index, ViewNode& root,
    std::string& error);

int LuaViewBox(lua_State* state);
int LuaViewRow(lua_State* state);
int LuaViewColumn(lua_State* state);
int LuaViewGrid(lua_State* state);
int LuaViewFlow(lua_State* state);
int LuaViewStack(lua_State* state);
int LuaViewScroll(lua_State* state);
int LuaViewList(lua_State* state);
int LuaViewGridList(lua_State* state);
int LuaViewVirtualList(lua_State* state);
int LuaViewVirtualGrid(lua_State* state);
int LuaViewListItem(lua_State* state);
int LuaViewText(lua_State* state);
int LuaViewStyledText(lua_State* state);
int LuaViewTextInput(lua_State* state);
int LuaViewTextArea(lua_State* state);
int LuaViewSearchBox(lua_State* state);
int LuaViewNumberInput(lua_State* state);
int LuaViewSelect(lua_State* state);
int LuaViewImage(lua_State* state);
int LuaViewButton(lua_State* state);
int LuaViewLink(lua_State* state);
int LuaViewToggle(lua_State* state);
int LuaViewCheckbox(lua_State* state);
int LuaViewRadioGroup(lua_State* state);
int LuaViewSlider(lua_State* state);
int LuaViewIcon(lua_State* state);
int LuaViewIconButton(lua_State* state);
int LuaViewShape(lua_State* state);
int LuaViewBadge(lua_State* state);
int LuaViewDivider(lua_State* state);
int LuaViewProgressBar(lua_State* state);
int LuaViewProgressRing(lua_State* state);
int LuaViewMeter(lua_State* state);
int LuaViewSparkline(lua_State* state);
int LuaViewLineChart(lua_State* state);
int LuaViewBarChart(lua_State* state);
int LuaViewWaveform(lua_State* state);
int LuaViewSpectrum(lua_State* state);
int LuaViewMonthCalendar(lua_State* state);
int LuaViewSlotSurface(lua_State* state);
int LuaViewSlotItem(lua_State* state);
int LuaViewSpacer(lua_State* state);
}
