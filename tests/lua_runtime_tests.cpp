#include "lua_runtime.h"

extern "C" {
#include <lauxlib.h>
}

#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Expect(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    ++failures;
}

struct TestState
{
    TestState()
        : state(luaL_newstate())
    {
        lua_pushlightuserdata(state, &quota);
        lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    }

    ~TestState()
    {
        if (state) lua_close(state);
    }

    TestState(const TestState&) = delete;
    TestState& operator=(const TestState&) = delete;

    lua_State* state = nullptr;
    LuaRuntimeQuota quota;
};

bool RunChunk(lua_State* state, const char* source)
{
    if (luaL_loadstring(state, source) != LUA_OK)
        return false;
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        state, 0, 0, 1000000, std::chrono::milliseconds(100));
    if (status != LUA_OK)
        lua_pop(state, 1);
    return status == LUA_OK;
}

struct ReentryProbe
{
    lua_State* nestedState = nullptr;
    LuaRuntimeQuota* quota = nullptr;
    bool outerHookPresentBefore = false;
    bool outerHookPresentAfter = false;
    bool nestedHookCleared = false;
    bool stackPreserved = false;
    bool nestedDeadlineShared = false;
};

struct LoadReentryProbe
{
    lua_State* nestedState = nullptr;
    lua_State* activeState = nullptr;
    bool nestedCallbackRan = false;
};

int ReenterSameState(lua_State* state)
{
    auto* probe = static_cast<ReentryProbe*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    const int entryTop = lua_gettop(state);
    probe->outerHookPresentBefore = lua_gethook(state) != nullptr;
    const auto outerDeadline = probe->quota
        ? probe->quota->deadline
        : std::chrono::steady_clock::time_point{};
    lua_getglobal(state, "inner");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        state, 0, 1);
    const bool result = status == LUA_OK &&
        lua_isinteger(state, -1) && lua_tointeger(state, -1) == 42;
    lua_pop(state, 1);
    probe->outerHookPresentAfter = lua_gethook(state) != nullptr;
    probe->nestedDeadlineShared = probe->quota &&
        probe->quota->deadline == outerDeadline;
    probe->stackPreserved = lua_gettop(state) == entryTop;
    if (!result)
        return luaL_error(state, "nested same-state callback failed");
    return 0;
}

int RecurseThroughHost(lua_State* state)
{
    lua_getglobal(state, "recursive_callback");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        state, 0, 0);
    if (status != LUA_OK)
        return lua_error(state);
    return 0;
}

int ReenterShortCallback(lua_State* state)
{
    lua_getglobal(state, "short_callback");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        state, 0, 0);
    if (status != LUA_OK)
        return lua_error(state);
    return 0;
}

int ReenterOtherState(lua_State* state)
{
    auto* probe = static_cast<ReentryProbe*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    const int entryTop = lua_gettop(state);
    probe->outerHookPresentBefore = lua_gethook(state) != nullptr;

    lua_getglobal(probe->nestedState, "inner");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        probe->nestedState, 0, 1);
    const bool result = status == LUA_OK &&
        lua_isinteger(probe->nestedState, -1) &&
        lua_tointeger(probe->nestedState, -1) == 84;
    lua_pop(probe->nestedState, 1);

    probe->nestedHookCleared =
        lua_gethook(probe->nestedState) == nullptr;
    probe->outerHookPresentAfter = lua_gethook(state) != nullptr;
    probe->stackPreserved = lua_gettop(state) == entryTop;
    if (!result)
        return luaL_error(state, "nested cross-state callback failed");
    return 0;
}

int ReenterDuringLoad(lua_State* state)
{
    auto* probe = static_cast<LoadReentryProbe*>(
        lua_touserdata(state, lua_upvalueindex(1)));
    probe->activeState = probe->nestedState;
    lua_getglobal(probe->nestedState, "during_load_reentry");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        probe->nestedState, 0, 1);
    probe->nestedCallbackRan = status == LUA_OK &&
        lua_isstring(probe->nestedState, -1) &&
        std::string(lua_tostring(probe->nestedState, -1)) == "old-vm";
    lua_pop(probe->nestedState, 1);
    if (!probe->nestedCallbackRan)
        return luaL_error(state, "load-time host reentry failed");
    return 0;
}

void RegisterHostCallback(lua_State* state, const char* name,
    lua_CFunction callback, ReentryProbe* probe)
{
    lua_pushlightuserdata(state, probe);
    lua_pushcclosure(state, callback, 1);
    lua_setglobal(state, name);
}

void TestStackGuard()
{
    TestState test;
    lua_pushinteger(test.state, 7);
    const int entryTop = lua_gettop(test.state);
    {
        snowdesktop::lua_runtime::StackGuard guard(test.state);
        lua_pushinteger(test.state, 8);
        lua_pushinteger(test.state, 9);
    }
    Expect(lua_gettop(test.state) == entryTop,
        "stack guard restores the active Lua API frame");
}

void TestSameStateReentry()
{
    TestState test;
    ReentryProbe probe;
    probe.quota = &test.quota;
    RegisterHostCallback(
        test.state, "host_reenter", ReenterSameState, &probe);
    Expect(RunChunk(test.state,
        "function inner() return 42 end\n"
        "function outer() host_reenter(); return 7 end"),
        "same-state reentry fixture loads");

    const int entryTop = lua_gettop(test.state);
    lua_getglobal(test.state, "outer");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        test.state, 0, 1);
    Expect(status == LUA_OK,
        "a host callback can synchronously reenter the same Lua VM");
    Expect(lua_isinteger(test.state, -1) &&
        lua_tointeger(test.state, -1) == 7,
        "the outer callback result survives same-state reentry");
    lua_pop(test.state, 1);
    Expect(probe.outerHookPresentBefore &&
        probe.outerHookPresentAfter,
        "the outer quota hook is restored around nested same-state calls");
    Expect(probe.stackPreserved,
        "same-state reentry preserves the host callback stack");
    Expect(probe.nestedDeadlineShared,
        "same-state reentry shares the outer execution deadline");
    Expect(lua_gethook(test.state) == nullptr,
        "the quota hook is removed after the outer callback");
    Expect(lua_gettop(test.state) == entryTop,
        "the outer stack height is preserved after same-state reentry");
}

void TestReentryDepthLimit()
{
    TestState test;
    lua_pushcfunction(test.state, RecurseThroughHost);
    lua_setglobal(test.state, "host_recurse");
    Expect(RunChunk(test.state,
        "function recursive_callback() host_recurse() end"),
        "recursive reentry fixture loads");

    const int entryTop = lua_gettop(test.state);
    lua_getglobal(test.state, "recursive_callback");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        test.state, 0, 0);
    Expect(status != LUA_OK,
        "recursive host reentry stops at the protected-call depth limit");
    const char* error = lua_tostring(test.state, -1);
    Expect(error && std::string(error).find("nesting limit exceeded") !=
        std::string::npos,
        "recursive host reentry returns a diagnostic Lua error");
    lua_pop(test.state, 1);
    Expect(test.quota.executionExceeded,
        "the runtime records a recursive callback limit violation");
    Expect(lua_gethook(test.state) == nullptr,
        "recursive host reentry leaves no quota hook installed");
    Expect(lua_gettop(test.state) == entryTop,
        "recursive host reentry restores the Lua stack");
}

void TestRepeatedShortReentryCannotResetQuota()
{
    TestState test;
    lua_pushcfunction(test.state, ReenterShortCallback);
    lua_setglobal(test.state, "host_short_reenter");
    Expect(RunChunk(test.state,
        "function short_callback() return 1 end\n"
        "function quota_loop()\n"
        "  while true do host_short_reenter() end\n"
        "end"),
        "short reentry quota fixture loads");

    lua_getglobal(test.state, "quota_loop");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        test.state, 0, 0, 10000, std::chrono::seconds(1));
    Expect(status != LUA_OK,
        "short nested calls cannot reset the outer instruction counter");
    const char* error = lua_tostring(test.state, -1);
    Expect(error && std::string(error).find("quota exceeded") !=
        std::string::npos,
        "repeated short reentry terminates with a quota diagnostic");
    lua_pop(test.state, 1);
    Expect(test.quota.executionExceeded,
        "repeated short reentry records a quota violation");
    Expect(lua_gethook(test.state) == nullptr,
        "repeated short reentry leaves no quota hook installed");
}

void TestCrossStateReentry()
{
    TestState outer;
    TestState nested;
    ReentryProbe probe;
    probe.nestedState = nested.state;
    RegisterHostCallback(
        outer.state, "host_reenter", ReenterOtherState, &probe);
    Expect(RunChunk(nested.state,
        "function inner() return 84 end"),
        "cross-state nested fixture loads");
    Expect(RunChunk(outer.state,
        "function outer() host_reenter(); return 21 end"),
        "cross-state outer fixture loads");

    const int outerTop = lua_gettop(outer.state);
    const int nestedTop = lua_gettop(nested.state);
    lua_getglobal(outer.state, "outer");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        outer.state, 0, 1);
    Expect(status == LUA_OK,
        "a host callback can synchronously enter another Lua VM");
    Expect(lua_isinteger(outer.state, -1) &&
        lua_tointeger(outer.state, -1) == 21,
        "the outer callback result uses its original Lua VM");
    lua_pop(outer.state, 1);
    Expect(probe.outerHookPresentBefore &&
        probe.outerHookPresentAfter,
        "cross-state reentry does not replace the outer quota hook");
    Expect(probe.nestedHookCleared,
        "the nested VM quota hook is removed on return");
    Expect(probe.stackPreserved,
        "cross-state reentry preserves the outer host callback stack");
    Expect(lua_gethook(outer.state) == nullptr,
        "the outer VM hook is removed after cross-state reentry");
    Expect(lua_gettop(outer.state) == outerTop &&
        lua_gettop(nested.state) == nestedTop,
        "cross-state reentry preserves both Lua stacks");
}

void TestLoadTransactionPinsStateAcrossHostReentry()
{
    TestState loading;
    TestState previous;
    Expect(RunChunk(previous.state,
        "function during_load_reentry() return 'old-vm' end"),
        "load-time reentry fixture loads");

    LoadReentryProbe probe;
    probe.nestedState = previous.state;
    probe.activeState = loading.state;
    lua_pushlightuserdata(loading.state, &probe);
    lua_pushcclosure(loading.state, ReenterDuringLoad, 1);
    lua_setglobal(loading.state, "host_load_reenter");

    lua_newtable(loading.state);
    lua_getglobal(loading.state, "host_load_reenter");
    lua_setfield(loading.state, -2, "host_load_reenter");
    Expect(luaL_loadstring(loading.state,
        "host_load_reenter(); name = 'new-vm'") == LUA_OK,
        "load transaction chunk compiles");
    lua_pushvalue(loading.state, -2);
    Expect(lua_setupvalue(loading.state, -2, 1) != nullptr,
        "load transaction chunk receives its sandbox");

    const int status = snowdesktop::lua_runtime::ProtectedCall(
        loading.state, 0, 0);
    Expect(status == LUA_OK,
        "load transaction survives synchronous host reentry");
    if (status != LUA_OK)
    {
        lua_pop(loading.state, 2);
        return;
    }
    Expect(probe.nestedCallbackRan &&
        probe.activeState == previous.state,
        "host reentry can replace an ambient VM pointer");

    const int sandboxRef = luaL_ref(
        loading.state, LUA_REGISTRYINDEX);
    lua_rawgeti(loading.state, LUA_REGISTRYINDEX, sandboxRef);
    lua_getfield(loading.state, -1, "name");
    Expect(lua_isstring(loading.state, -1) &&
        std::string(lua_tostring(loading.state, -1)) == "new-vm",
        "the pinned loading VM remains usable after host reentry");
    lua_pop(loading.state, 2);
    luaL_unref(loading.state, LUA_REGISTRYINDEX, sandboxRef);
}

void TestQuotaErrorIsProtected()
{
    TestState test;
    Expect(luaL_loadstring(test.state, "while true do end") == LUA_OK,
        "quota fixture loads");
    const int status = snowdesktop::lua_runtime::ProtectedCall(
        test.state, 0, 0, 10000, std::chrono::seconds(1));
    Expect(status != LUA_OK,
        "an execution quota violation returns as a Lua error");
    const char* error = lua_tostring(test.state, -1);
    Expect(error && std::string(error).find("quota exceeded") !=
        std::string::npos,
        "the quota violation retains its diagnostic message");
    lua_pop(test.state, 1);
    Expect(test.quota.executionExceeded,
        "the quota records the top-level execution violation");
    Expect(lua_gethook(test.state) == nullptr,
        "no quota hook remains after an error");
}
}

int main()
{
    TestStackGuard();
    TestSameStateReentry();
    TestCrossStateReentry();
    TestLoadTransactionPinsStateAcrossHostReentry();
    TestReentryDepthLimit();
    TestRepeatedShortReentryCannotResetQuota();
    TestQuotaErrorIsProtected();

    if (failures == 0)
        std::cout << "Lua runtime tests passed\n";
    return failures == 0 ? 0 : 1;
}
