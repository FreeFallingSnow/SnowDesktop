#include "widget_lua_lifecycle.h"

#include <cstdlib>
#include <iostream>
#include <string>

extern "C" {
#include <lualib.h>
}

namespace
{
using snowdesktop::widget_runtime::WidgetLuaLifecycle;

int setupCalls = 0;
int disposeCalls = 0;
int eventCalls = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void PushContext(lua_State* state)
{
    lua_createtable(state, 0, 1);
    lua_pushinteger(state, 42);
    lua_setfield(state, -2, "token");
}

int SetupModel(lua_State* state)
{
    lua_getfield(state, 1, "token");
    Check(lua_tointeger(state, -1) == 42,
        "setup must receive the host context");
    lua_pop(state, 1);
    ++setupCalls;
    lua_createtable(state, 0, 1);
    lua_pushliteral(state, "ready");
    lua_setfield(state, -2, "status");
    return 1;
}

int DisposeModel(lua_State* state)
{
    lua_getfield(state, 1, "token");
    const bool contextMatches = lua_tointeger(state, -1) == 42;
    lua_pop(state, 1);
    lua_getfield(state, 2, "status");
    const bool modelMatches = lua_isstring(state, -1) &&
        std::string(lua_tostring(state, -1)) == "ready";
    lua_pop(state, 1);
    const bool reasonMatches = lua_isstring(state, 3) &&
        std::string(lua_tostring(state, 3)) == "unload";
    Check(contextMatches && modelMatches && reasonMatches,
        "dispose must receive context, the retained model, and reason");
    ++disposeCalls;
    return 0;
}

int HandleEvent(lua_State* state)
{
    lua_getfield(state, 1, "token");
    const bool contextMatches = lua_tointeger(state, -1) == 42;
    lua_pop(state, 1);
    lua_getfield(state, 2, "status");
    const bool modelMatches = lua_isstring(state, -1) &&
        std::string(lua_tostring(state, -1)) == "ready";
    lua_pop(state, 1);
    lua_getfield(state, 3, "kind");
    const bool eventMatches = lua_isstring(state, -1) &&
        std::string(lua_tostring(state, -1)) == "timer";
    lua_pop(state, 1);
    Check(contextMatches && modelMatches && eventMatches,
        "event must receive context, the retained model, and payload");
    ++eventCalls;
    return 0;
}

int SetupFailure(lua_State* state)
{
    return luaL_error(state, "setup exploded");
}

int StoreDefinition(lua_State* state, lua_CFunction setup,
    lua_CFunction dispose, lua_CFunction event = nullptr)
{
    lua_createtable(state, 0, 3);
    if (setup)
    {
        lua_pushcfunction(state, setup);
        lua_setfield(state, -2, "setup");
    }
    if (dispose)
    {
        lua_pushcfunction(state, dispose);
        lua_setfield(state, -2, "dispose");
    }
    if (event)
    {
        lua_pushcfunction(state, event);
        lua_setfield(state, -2, "event");
    }
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

void TestSetupModelRenderAndDispose()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be created");
    const int definition = StoreDefinition(
        state, SetupModel, DisposeModel, HandleEvent);
    WidgetLuaLifecycle lifecycle;
    std::string error;
    Check(lifecycle.Setup(state, definition, PushContext, error) &&
            error.empty() && lifecycle.SetupCompleted() &&
            setupCalls == 1,
        "setup must run once and retain its model");
    Check(!lifecycle.Setup(state, definition, PushContext, error) &&
            setupCalls == 1,
        "setup must reject repeated execution");

    const int entryTop = lua_gettop(state);
    Check(lifecycle.PushRenderArguments(state, PushContext) &&
            lua_gettop(state) == entryTop + 2,
        "render arguments must contain context and model");
    lua_getfield(state, -1, "status");
    Check(std::string(lua_tostring(state, -1)) == "ready",
        "render must receive the retained setup model");
    lua_settop(state, entryTop);

    lua_createtable(state, 0, 1);
    lua_pushliteral(state, "timer");
    lua_setfield(state, -2, "kind");
    bool eventInvoked = false;
    Check(lifecycle.Event(state, definition, PushContext, -1,
            eventInvoked, error) && eventInvoked && eventCalls == 1,
        "event must execute with the retained setup model");
    lua_pop(state, 1);

    Check(lifecycle.Dispose(state, definition, PushContext,
            "unload", error) && disposeCalls == 1 &&
            lifecycle.DisposeInvoked(),
        "dispose must execute once");
    Check(lifecycle.Dispose(state, definition, PushContext,
            "shutdown", error) && disposeCalls == 1,
        "repeated dispose must be idempotent");
    lifecycle.Release(state);
    luaL_unref(state, LUA_REGISTRYINDEX, definition);
    lua_close(state);
}

void TestNoSetupAndSetupFailure()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be created");
    std::string error;

    int definition = StoreDefinition(state, nullptr, nullptr);
    WidgetLuaLifecycle empty;
    Check(empty.Setup(state, definition, PushContext, error),
        "a missing optional setup callback must produce a nil model");
    const int entryTop = lua_gettop(state);
    Check(empty.PushRenderArguments(state, PushContext) &&
            lua_isnil(state, -1),
        "render must receive nil when setup is absent");
    lua_settop(state, entryTop);
    lua_createtable(state, 0, 0);
    bool eventInvoked = true;
    Check(empty.Event(state, definition, PushContext, -1,
            eventInvoked, error) && !eventInvoked,
        "a missing optional event callback must be accepted");
    lua_pop(state, 1);
    Check(empty.Dispose(state, definition, PushContext,
            "unload", error),
        "a missing optional dispose callback must be accepted");
    empty.Release(state);
    luaL_unref(state, LUA_REGISTRYINDEX, definition);

    definition = StoreDefinition(state, SetupFailure, nullptr);
    WidgetLuaLifecycle failed;
    Check(!failed.Setup(state, definition, PushContext, error) &&
            error.find("setup exploded") != std::string::npos &&
            !failed.SetupCompleted(),
        "setup failures must remain observable and must not retain a model");
    failed.Release(state);
    luaL_unref(state, LUA_REGISTRYINDEX, definition);
    lua_close(state);
}
}

int main()
{
    TestSetupModelRenderAndDispose();
    TestNoSetupAndSetupFailure();
    std::cout << "widget Lua lifecycle tests passed\n";
    return 0;
}
