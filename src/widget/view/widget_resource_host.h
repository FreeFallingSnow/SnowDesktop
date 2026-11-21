#pragma once

#include <string>

struct lua_State;

int lua_ResourceImage(lua_State* state);
int lua_ResourceFont(lua_State* state);
int lua_ResourceStatus(lua_State* state);
int lua_DrawIcon(lua_State* state);
struct D2DState;
void PrewarmWidgetPackageResources(D2DState* state);
void ReleasePackageImageResources(D2DState* state, lua_State* luaState);
void BindWidgetDrawHost(lua_State* state, D2DState* context);
void ClearRuntimeImagesForWidget(D2DState* state, const std::wstring& widgetId);
struct LuaWidget;
void CapturePackageImageResources(LuaWidget& widget);
