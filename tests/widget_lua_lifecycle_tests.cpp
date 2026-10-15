#include "widget/runtime/widget_lua_lifecycle.h"
#include "widget/runtime/lua_runtime.h"

#include <windows.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

extern "C" {
#include <lualib.h>
}

namespace
{
using snowdesktop::widget_runtime::WidgetLuaLifecycle;

int setupCalls = 0;
int disposeCalls = 0;
int eventCalls = 0;
int menuCalls = 0;

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

int BuildMenu(lua_State* state)
{
    lua_getfield(state, 1, "token");
    const bool contextMatches = lua_tointeger(state, -1) == 42;
    lua_pop(state, 1);
    lua_getfield(state, 2, "status");
    const bool modelMatches = lua_isstring(state, -1) &&
        std::string(lua_tostring(state, -1)) == "ready";
    lua_pop(state, 1);
    lua_getfield(state, 3, "id");
    const bool requestMatches = lua_isstring(state, -1) &&
        std::string(lua_tostring(state, -1)) == "item.menu";
    lua_pop(state, 1);
    Check(contextMatches && modelMatches && requestMatches,
        "menu must receive context, the retained model, and request");
    ++menuCalls;
    lua_createtable(state, 1, 0);
    lua_createtable(state, 0, 2);
    lua_pushliteral(state, "item.open");
    lua_setfield(state, -2, "id");
    lua_pushliteral(state, "Open");
    lua_setfield(state, -2, "label");
    lua_rawseti(state, -2, 1);
    return 1;
}

int SetupFailure(lua_State* state)
{
    return luaL_error(state, "setup exploded");
}

int StoreDefinition(lua_State* state, lua_CFunction setup,
    lua_CFunction dispose, lua_CFunction event = nullptr,
    lua_CFunction menu = nullptr)
{
    lua_createtable(state, 0, 4);
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
    if (menu)
    {
        lua_pushcfunction(state, menu);
        lua_setfield(state, -2, "menu");
    }
    return luaL_ref(state, LUA_REGISTRYINDEX);
}

void TestSetupModelRenderAndDispose()
{
    lua_State* state = luaL_newstate();
    Check(state != nullptr, "Lua state must be created");
    const int definition = StoreDefinition(
        state, SetupModel, DisposeModel, HandleEvent, BuildMenu);
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

    lua_createtable(state, 0, 1);
    lua_pushliteral(state, "item.menu");
    lua_setfield(state, -2, "id");
    bool menuInvoked = false;
    Check(lifecycle.Menu(state, definition, PushContext, -1,
            menuInvoked, error) && menuInvoked && menuCalls == 1 &&
            lua_istable(state, -1) && lua_rawlen(state, -1) == 1,
        "menu must return its synchronous menu model on the Lua stack");
    lua_pop(state, 2);

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

struct AllocationBudget
{
    std::size_t used = 0;
    std::size_t limit = 1024 * 1024;
};

AllocationBudget* failingBudget = nullptr;
int failingContextCalls = 0;

void PushFailingContext(lua_State* state)
{
    ++failingContextCalls;
    // Allow the protected-call frames to initialize, then inject the real
    // allocator failure at host payload construction in both implementations.
    failingBudget->limit = failingBudget->used;
    PushContext(state);
}

void* Allocate(void* context, void* pointer, std::size_t oldSize, std::size_t newSize)
{
    auto& budget = *static_cast<AllocationBudget*>(context);
    if (!pointer) oldSize = 0;
    if (!newSize)
    {
        budget.used -= oldSize;
        std::free(pointer);
        return nullptr;
    }
    if (newSize > oldSize && newSize - oldSize > budget.limit - budget.used)
        return nullptr;
    void* replacement = std::realloc(pointer, newSize);
    if (replacement) budget.used = budget.used - oldSize + newSize;
    return replacement;
}

void TestAllocationFailure(std::string_view operation)
{
    AllocationBudget budget;
    lua_State* state = lua_newstate(Allocate, &budget);
    Check(state && lua_checkstack(state, 128), "fault fixture initializes its real Lua VM");
    lua_pushlightuserdata(state, nullptr);
    lua_setfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    lua_pushcfunction(state, +[](lua_State*) { return 0; });
    Check(snowdesktop::lua_runtime::ProtectedCall(state, 0, 0) == LUA_OK,
        "fault fixture warms the existing protected callback path");
    const int definition = StoreDefinition(state, SetupModel, DisposeModel, HandleEvent, BuildMenu);
    WidgetLuaLifecycle lifecycle;
    std::string error;
    if (operation != "setup")
        Check(lifecycle.Setup(state, definition, PushContext, error), "fault fixture retains a real setup model");
    lua_pushinteger(state, 12345);
    lua_createtable(state, 0, 2);
    lua_pushliteral(state, "timer");
    lua_setfield(state, -2, "kind");
    lua_pushliteral(state, "item.menu");
    lua_setfield(state, -2, "id");
    lua_gc(state, LUA_GCCOLLECT);
    const int base = lua_gettop(state);
    failingBudget = &budget;
    bool invoked = false;
    bool accepted = true;
    if (operation == "setup") accepted = lifecycle.Setup(state, definition, PushFailingContext, error);
    else if (operation == "render") accepted = lifecycle.PushRenderArguments(state, PushFailingContext);
    else if (operation == "event") accepted = lifecycle.Event(state, definition, PushFailingContext, -1, invoked, error);
    else if (operation == "menu") accepted = lifecycle.Menu(state, definition, PushFailingContext, -1, invoked, error);
    else if (operation == "dispose") accepted = lifecycle.Dispose(state, definition, PushFailingContext, "unload", error);
    else Check(false, "known allocation failure scenario");
    Check(failingContextCalls == 1 && !accepted && !invoked && lua_gettop(state) == base,
        "host argument allocation failure is contained and restores the caller stack");
    Check(operation == "render" || !error.empty(), "allocation failure remains observable");
    budget.limit = 1024 * 1024;
    if (operation == "setup")
        Check(lifecycle.Setup(state, definition, PushContext, error), "failed setup can be retried");
    else if (operation == "render")
    {
        Check(lifecycle.PushRenderArguments(state, PushContext), "failed render argument construction can be retried");
        lua_settop(state, base);
    }
    else if (operation == "event")
        Check(lifecycle.Event(state, definition, PushContext, -1, invoked, error) && invoked,
            "failed event argument construction can be retried with the retained model");
    else if (operation == "menu")
    {
        Check(lifecycle.Menu(state, definition, PushContext, -1, invoked, error) && invoked,
            "failed menu argument construction can be retried with the retained model");
        lua_settop(state, base);
    }
    else
    {
        const bool started = lifecycle.DisposeInvoked();
        Check(lifecycle.Dispose(state, definition, PushContext, "unload", error)
                && disposeCalls == (started ? 0 : 1),
            "dispose retries only when protection failed before disposal began");
        Check(lifecycle.Dispose(state, definition, PushContext, "unload", error)
                && disposeCalls == (started ? 0 : 1),
            "repeated disposal never executes its callback twice");
    }
    lifecycle.Release(state);
    luaL_unref(state, LUA_REGISTRYINDEX, definition);
    lua_close(state);
    Check(budget.used == 0, "closing the recovered VM frees every tracked allocation");
}

void TestIsolatedAllocationFailures()
{
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    Check(length && length < executable.size(), "allocation fixture executable path is available");
    bool allPassed = true;
    for (const wchar_t* operation : {L"setup", L"render", L"event", L"menu", L"dispose"})
    {
        std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" --oom-" + operation;
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        Check(job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "allocation child has a bounded owned process lifetime");
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        Check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process),
            "allocation child starts without opening a desktop window");
        const bool assigned = AssignProcessToJobObject(job, process.hProcess) != FALSE;
        if (!assigned) TerminateProcess(process.hProcess, 3);
        Check(assigned, "allocation child belongs to its cleanup job");
        ResumeThread(process.hThread);
        const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
        DWORD exit = 0;
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 4);
        Check(GetExitCodeProcess(process.hProcess, &exit), "allocation child status is observable");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        if (wait != WAIT_OBJECT_0 || exit != 0)
        {
            std::wcerr << L"allocation scenario " << operation << L": wait=" << wait << L", exit=" << exit << L'\n';
            allPassed = false;
        }
    }
    Check(allPassed, "all five real lifecycle entry points contain allocation failure and recover");
}
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 2 && std::string_view(argv[1]).starts_with("--oom-"))
    {
        TestAllocationFailure(std::string_view(argv[1]).substr(6));
        return 0;
    }
    TestSetupModelRenderAndDispose();
    TestNoSetupAndSetupFailure();
    TestIsolatedAllocationFailures();
    std::cout << "widget Lua lifecycle tests passed\n";
    return 0;
}
