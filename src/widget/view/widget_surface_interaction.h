#pragma once

struct lua_State;

int lua_InteractionRegion(lua_State* state);
int lua_InteractionIsHovered(lua_State* state);
int lua_InteractionIsPressed(lua_State* state);
int lua_InteractionIsFocused(lua_State* state);
int lua_InteractionScroll(lua_State* state);
int lua_InteractionSetScrollOffset(lua_State* state);
int lua_UiDurationPicker(lua_State* state);
int lua_UiTimePicker(lua_State* state);
int lua_UiDatePicker(lua_State* state);
int lua_UiMenu(lua_State* state);
int lua_UiMetrics(lua_State* state);
int lua_WidgetInfo(lua_State* state);
int lua_WidgetHasPermission(lua_State* state);
int lua_WidgetSetTitle(lua_State* state);
int lua_WidgetOpenSettings(lua_State* state);
int lua_WidgetInvalidate(lua_State* state);
int lua_WidgetLog(lua_State* state);
int lua_WidgetTheme(lua_State* state);
int lua_WidgetOpenPanel(lua_State* state);
int lua_WidgetClosePanel(lua_State* state);
int lua_ViewVirtualRange(lua_State* state);
int lua_ViewScrollTo(lua_State* state);
int lua_ViewScrollBy(lua_State* state);
int lua_ViewScrollToIndex(lua_State* state);
int lua_WidgetOpenDialog(lua_State* state);
int lua_WidgetOpenPopover(lua_State* state);
