#pragma once
#include "widget_gpu_sampler.h"
extern "C" {
#include <lua.h>
}

namespace snowdesktop::widget_runtime
{
// Detailed subscriptions can display an enumerated device even when one or all
// counters fail. The legacy aggregate availability contract is unchanged.
inline bool WidgetGpuValueAvailable(const WidgetGpuDataSnapshot& snapshot, bool includeDetails)
{
    return snapshot.available || (includeDetails && !snapshot.adapters.empty());
}

inline void PushWidgetGpuAdapters(lua_State* state,
    const WidgetGpuDataSnapshot& snapshot, bool includeDetails)
{
    lua_createtable(state, static_cast<int>(snapshot.adapters.size()), 0);
    int adapterIndex = 1;
    for (const auto& adapter : snapshot.adapters)
    {
        lua_createtable(state, 0, includeDetails ? 11 : 7);
        const auto string = [state](const char* key, const std::string& value) {
            lua_pushlstring(state, value.data(), value.size()); lua_setfield(state, -2, key);
        };
        const auto bytes = [state](const char* key, std::uint64_t value) {
            lua_pushinteger(state, static_cast<lua_Integer>(value)); lua_setfield(state, -2, key);
        };
        const auto flag = [state](const char* key, bool value) {
            lua_pushboolean(state, value); lua_setfield(state, -2, key);
        };
        string("id", adapter.id);
        string("name", adapter.name);
        lua_pushnumber(state, adapter.usagePercent);
        lua_setfield(state, -2, "usagePercent");
        bytes("dedicatedMemoryBytes", adapter.dedicatedMemoryBytes);
        bytes("dedicatedUsedBytes", adapter.dedicatedUsedBytes);
        bytes("sharedMemoryBytes", adapter.sharedMemoryBytes);
        bytes("sharedUsedBytes", adapter.sharedUsedBytes);
        if (includeDetails)
        {
            flag("usageAvailable", adapter.usageAvailable && !snapshot.warmingUp);
            flag("dedicatedUsageAvailable", adapter.dedicatedUsageAvailable);
            flag("sharedUsageAvailable", adapter.sharedUsageAvailable);
            lua_createtable(state, static_cast<int>(adapter.engines.size()), 0);
            int engineIndex = 1;
            if (adapter.usageAvailable && !snapshot.warmingUp)
                for (const auto& engine : adapter.engines)
                {
                    lua_createtable(state, 0, 4);
                    lua_pushinteger(state, engine.physical);
                    lua_setfield(state, -2, "physicalIndex");
                    lua_pushinteger(state, engine.engine);
                    lua_setfield(state, -2, "engineIndex");
                    string("type", engine.type);
                    lua_pushnumber(state, engine.usagePercent);
                    lua_setfield(state, -2, "usagePercent");
                    lua_rawseti(state, -2, engineIndex++);
                }
            lua_setfield(state, -2, "engines");
        }
        lua_rawseti(state, -2, adapterIndex++);
    }
}
}
