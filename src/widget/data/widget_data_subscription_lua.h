#pragma once

#include <memory>
#include <string>
#include <string_view>

struct lua_State;

namespace snowdesktop::widget_runtime
{
struct WidgetRuntimeImagePixels;
struct WidgetScreenTimeDataSnapshot;

struct WidgetDataLuaImages
{
    std::string (*registerImage)(lua_State*, std::string_view,
        const std::shared_ptr<const WidgetRuntimeImagePixels>&, const char*, bool);
    void (*retainScreenTimeIcons)(lua_State*, const WidgetScreenTimeDataSnapshot&);
};

void BindWidgetDataLuaImages(lua_State* state, const WidgetDataLuaImages& images);
void RegisterDataSubscriptionHandle(lua_State* state);
int lua_DataSubscribe(lua_State* state);
}
