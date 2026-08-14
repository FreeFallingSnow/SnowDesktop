#include "widget_lua_lifecycle.h"

#include "lua_runtime.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::int64_t LifecycleInstructionBudget = 1000000;
constexpr auto LifecycleTimeBudget = std::chrono::milliseconds(100);

void PushModel(lua_State* state, int modelRef)
{
    if (modelRef == LUA_NOREF || modelRef == LUA_REFNIL)
        lua_pushnil(state);
    else
        lua_rawgeti(state, LUA_REGISTRYINDEX, modelRef);
}

std::string PopLuaError(lua_State* state, const char* fallback)
{
    const char* message = lua_tostring(state, -1);
    std::string result = message ? message : fallback;
    lua_pop(state, 1);
    return result;
}
}

WidgetLuaLifecycle::WidgetLuaLifecycle(
    WidgetLuaLifecycle&& other) noexcept
    : modelRef_(std::exchange(other.modelRef_, LUA_NOREF)),
      setupCompleted_(std::exchange(other.setupCompleted_, false)),
      disposeInvoked_(std::exchange(other.disposeInvoked_, false))
{
}

WidgetLuaLifecycle& WidgetLuaLifecycle::operator=(
    WidgetLuaLifecycle&& other) noexcept
{
    if (this == &other) return *this;
    modelRef_ = std::exchange(other.modelRef_, LUA_NOREF);
    setupCompleted_ = std::exchange(other.setupCompleted_, false);
    disposeInvoked_ = std::exchange(other.disposeInvoked_, false);
    return *this;
}

bool WidgetLuaLifecycle::Setup(lua_State* state, int definitionRef,
    PushContext pushContext, std::string& error)
{
    error.clear();
    if (!state || !pushContext || definitionRef == LUA_NOREF ||
        definitionRef == LUA_REFNIL)
    {
        error = "invalid widget lifecycle setup context";
        return false;
    }
    if (setupCompleted_)
    {
        error = "widget lifecycle setup has already completed";
        return false;
    }

    const int entryTop = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, definitionRef);
    if (!lua_istable(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle definition is unavailable";
        return false;
    }
    lua_getfield(state, -1, "setup");
    if (lua_isnil(state, -1))
    {
        lua_pop(state, 1);
        lua_pushnil(state);
        modelRef_ = luaL_ref(state, LUA_REGISTRYINDEX);
        setupCompleted_ = true;
        lua_settop(state, entryTop);
        return true;
    }
    if (!lua_isfunction(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle setup must be a function";
        return false;
    }

    pushContext(state);
    if (snowdesktop::lua_runtime::ProtectedCall(
            state, 1, 1, LifecycleInstructionBudget,
            LifecycleTimeBudget) != LUA_OK)
    {
        error = PopLuaError(state, "widget setup failed");
        lua_settop(state, entryTop);
        return false;
    }
    modelRef_ = luaL_ref(state, LUA_REGISTRYINDEX);
    setupCompleted_ = true;
    lua_settop(state, entryTop);
    return true;
}

bool WidgetLuaLifecycle::PushRenderArguments(lua_State* state,
    PushContext pushContext) const
{
    if (!state || !pushContext || !setupCompleted_ || disposeInvoked_)
        return false;
    pushContext(state);
    PushModel(state, modelRef_);
    return true;
}

bool WidgetLuaLifecycle::Event(lua_State* state, int definitionRef,
    PushContext pushContext, int eventIndex, bool& invoked,
    std::string& error) const
{
    invoked = false;
    error.clear();
    if (!state || !pushContext || !setupCompleted_ || disposeInvoked_ ||
        definitionRef == LUA_NOREF || definitionRef == LUA_REFNIL ||
        !lua_istable(state, eventIndex))
    {
        error = "invalid widget lifecycle event context";
        return false;
    }

    eventIndex = lua_absindex(state, eventIndex);
    const int entryTop = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, definitionRef);
    if (!lua_istable(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle definition is unavailable";
        return false;
    }
    lua_getfield(state, -1, "event");
    if (lua_isnil(state, -1))
    {
        lua_settop(state, entryTop);
        return true;
    }
    if (!lua_isfunction(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle event must be a function";
        return false;
    }

    pushContext(state);
    PushModel(state, modelRef_);
    lua_pushvalue(state, eventIndex);
    invoked = true;
    if (snowdesktop::lua_runtime::ProtectedCall(
            state, 3, 0, LifecycleInstructionBudget,
            LifecycleTimeBudget) != LUA_OK)
    {
        error = PopLuaError(state, "widget event failed");
        lua_settop(state, entryTop);
        return false;
    }
    lua_settop(state, entryTop);
    return true;
}

bool WidgetLuaLifecycle::Dispose(lua_State* state, int definitionRef,
    PushContext pushContext, const char* reason, std::string& error)
{
    error.clear();
    if (disposeInvoked_) return true;
    disposeInvoked_ = true;
    if (!setupCompleted_) return true;
    if (!state || !pushContext || definitionRef == LUA_NOREF ||
        definitionRef == LUA_REFNIL)
    {
        error = "invalid widget lifecycle dispose context";
        return false;
    }

    const int entryTop = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, definitionRef);
    if (!lua_istable(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle definition is unavailable";
        return false;
    }
    lua_getfield(state, -1, "dispose");
    if (lua_isnil(state, -1))
    {
        lua_settop(state, entryTop);
        return true;
    }
    if (!lua_isfunction(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle dispose must be a function";
        return false;
    }

    pushContext(state);
    PushModel(state, modelRef_);
    lua_pushstring(state, reason ? reason : "unknown");
    if (snowdesktop::lua_runtime::ProtectedCall(
            state, 3, 0, LifecycleInstructionBudget,
            LifecycleTimeBudget) != LUA_OK)
    {
        error = PopLuaError(state, "widget dispose failed");
        lua_settop(state, entryTop);
        return false;
    }
    lua_settop(state, entryTop);
    return true;
}

void WidgetLuaLifecycle::Release(lua_State* state) noexcept
{
    if (state && modelRef_ != LUA_NOREF && modelRef_ != LUA_REFNIL)
        luaL_unref(state, LUA_REGISTRYINDEX, modelRef_);
    modelRef_ = LUA_NOREF;
    setupCompleted_ = false;
    disposeInvoked_ = false;
}

bool WidgetLuaLifecycle::SetupCompleted() const noexcept
{
    return setupCompleted_;
}

bool WidgetLuaLifecycle::DisposeInvoked() const noexcept
{
    return disposeInvoked_;
}
}
