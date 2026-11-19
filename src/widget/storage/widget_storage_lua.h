#pragma once

#include "widget/view/widget_interaction_types.h"

#include <string>
#include <vector>

struct lua_State;

namespace snowdesktop::widget_runtime
{
struct WidgetStorageLuaHost
{
    std::string (*prefix)(lua_State*);
    bool (*canWrite)(lua_State*);
    bool (*secretReference)(lua_State*, const std::string&, std::string&);
    bool (*filesystemHandle)(lua_State*, const std::string&, std::string&);
    bool (*entityReference)(lua_State*, const std::string&);
    bool (*typedDefault)(lua_State*, const std::string&, InteractionValue&);
    std::string (*defaultValue)(lua_State*, const std::string&);
    std::vector<std::string> (*managedKeys)(lua_State*);
};

void BindWidgetStorageContext(lua_State* state, const WidgetStorageLuaHost* host);
int lua_StorageGet(lua_State* state);
int lua_StorageSet(lua_State* state);
int lua_StorageRemove(lua_State* state);
int lua_StorageKeys(lua_State* state);
int lua_StorageTransaction(lua_State* state);
}
