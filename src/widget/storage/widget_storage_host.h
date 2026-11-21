#pragma once

struct lua_State;

void BindWidgetHostStorage(lua_State* state, bool hasRenderContext);
