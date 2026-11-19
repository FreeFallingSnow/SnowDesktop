#include "widget_lua_binding_context.h"

#include "common/utils.h"
#include "widget_engine.h"

namespace snowdesktop::widget_runtime
{
namespace
{
const char kBoundWidgetEngineKey = 0;
}

void BindWidgetLuaEngine(lua_State* state, WidgetEngine* engine)
{
    lua_pushlightuserdata(state, engine);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kBoundWidgetEngineKey);
}

WidgetEngine* BoundWidgetEngine(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kBoundWidgetEngineKey);
    auto* engine = static_cast<WidgetEngine*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return engine;
}

std::wstring BoundWidgetId(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_id");
    const char* value = lua_tostring(state, -1);
    const std::wstring result = Utf8ToWide(value ? value : "");
    lua_pop(state, 1);
    return result;
}

std::uint64_t BoundWidgetRuntimeToken(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_runtime_token");
    const lua_Integer value = lua_isinteger(state, -1)
        ? lua_tointeger(state, -1) : 0;
    lua_pop(state, 1);
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

bool RequirePermission(lua_State* L, const char* permission)
{
    auto* host = BoundWidgetEngine(L);
    if (!host) return false;
    if (host->RuntimeHasPermission(BoundWidgetId(L), permission))
        return true;
    std::string msg = std::string("Permission denied: ") + permission;
    host->RuntimeRecordError(BoundWidgetId(L), msg);
    luaL_error(L, "%s", msg.c_str());
    return false;
}

}
