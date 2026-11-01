#include "widget/runtime/lua_runtime.h"
extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
struct State
{
    LuaRuntimeQuota quota;
    lua_State* lua = nullptr;
    State()
    {
        quota.memoryLimit = 256u * 1024u;
        lua = lua_newstate(snowdesktop::lua_runtime::QuotaAllocator, &quota);
        Require(lua != nullptr, "allocate bounded Lua state");
        lua_pushlightuserdata(lua, &quota);
        lua_setfield(lua, LUA_REGISTRYINDEX, "__quota_ptr");
        luaL_openlibs(lua);
    }
    ~State() { if (lua) lua_close(lua); }
};
int Run(State& state, const char* source, std::int64_t budget = 100000)
{
    lua_settop(state.lua, 0);
    Require(luaL_loadstring(state.lua, source) == LUA_OK, "load independent Lua scenario");
    return snowdesktop::lua_runtime::ProtectedCall(state.lua, 0, 1, budget, std::chrono::seconds(1));
}
void Recovered(State& state)
{
    Require(Run(state, "return 6 * 7") == LUA_OK && lua_tointeger(state.lua, -1) == 42,
        "a terminated callback leaves its runtime usable by the next callback");
}
int Reenter(lua_State* lua)
{
    lua_getglobal(lua, "nested");
    const int result = snowdesktop::lua_runtime::ProtectedCall(lua, 0, 1, 1000000, std::chrono::seconds(1));
    if (result != LUA_OK) return lua_error(lua);
    return 1;
}
void Budgets()
{
    State state;
    Require(Run(state, "while true do end", 10000) != LUA_OK,
        "an unbounded author callback is stopped by the execution budget");
    Recovered(state);
    lua_pushcfunction(state.lua, Reenter); lua_setglobal(state.lua, "reenter");
    Require(Run(state,
        "nested = function() local n=0 for i=1,50000 do n=n+i end return n end; return reenter()", 2500) != LUA_OK,
        "host reentry cannot reset the outer callback budget");
    Recovered(state);
    Require(Run(state, "return string.rep('x', 1024 * 1024)") == LUA_ERRMEM,
        "author allocations cannot exceed the isolated runtime memory budget");
    lua_settop(state.lua, 0); lua_gc(state.lua, LUA_GCCOLLECT);
    Recovered(state);
}
}
int main()
{
    try { Budgets(); std::cout << "PASS Lua execution, nested-call and memory budgets with recovery\n"; return 0; }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
