#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_UiTextInput(lua_State* state);
int lua_UiTextArea(lua_State* state);
int lua_UiFocusInput(lua_State* state);
int lua_ControlTextInput(lua_State* state);
int lua_ControlTextArea(lua_State* state);
int lua_ControlFocus(lua_State* state);
int lua_ControlBlur(lua_State* state);
int lua_UiButton(lua_State* state);
int lua_UiToggle(lua_State* state);
int lua_UiProgress(lua_State* state);
int lua_UiScrollArea(lua_State* state);
int lua_UiVirtualList(lua_State* state);
int lua_UiSetScrollOffset(lua_State* state);
}
