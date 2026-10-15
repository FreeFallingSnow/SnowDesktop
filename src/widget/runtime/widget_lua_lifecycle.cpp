#include "widget_lua_lifecycle.h"

#include "lua_runtime.h"
#include "diagnostics/performance_trace.h"

#include <chrono>
#include <cstdint>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::int64_t LifecycleInstructionBudget = 1000000;
constexpr auto LifecycleTimeBudget = std::chrono::milliseconds(100);

bool PushMeasuredContext(lua_State* state,
    WidgetLuaLifecycle::PushContext pushContext, std::string& error)
{
    // This is host argument construction, not the lifetime of the execution
    // context guard or the subsequent Lua callback. Owner is inherited from
    // the surrounding widget scope, as with lua/protectedCall.
    snowdesktop::performance::Scope scope("lua", "context.arguments");
    return snowdesktop::lua_runtime::ProtectedHostCall(state,
        +[](lua_State* inner) {
            const auto* callback = static_cast<WidgetLuaLifecycle::PushContext*>(
                lua_touserdata(inner, 1));
            (*callback)(inner);
            return 1;
        }, &pushContext, 1, error);
}

void PushModel(lua_State* state, int modelRef)
{
    if (modelRef == LUA_NOREF || modelRef == LUA_REFNIL)
        lua_pushnil(state);
    else
        lua_rawgeti(state, LUA_REGISTRYINDEX, modelRef);
}

std::string PopLuaError(lua_State* state, const char* fallback)
{
    const char* message = lua_type(state, -1) == LUA_TSTRING
        ? lua_tostring(state, -1) : nullptr;
    std::string result = message ? message : fallback;
    lua_pop(state, 1);
    return result;
}

template<class Operation>
bool ProtectLifecycleOperation(lua_State* state, Operation& operation,
    int results, std::string& error, int argumentIndex = 0)
{
    return snowdesktop::lua_runtime::ProtectedHostCall(state,
        +[](lua_State* inner) {
            auto* callback = static_cast<Operation*>(lua_touserdata(inner, 1));
            return (*callback)(inner);
        }, &operation, results, error, argumentIndex);
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
    bool accepted = false;
    auto operation = [&](lua_State* inner) {
        accepted = SetupUnprotected(inner, definitionRef, pushContext, error);
        return 0;
    };
    return ProtectLifecycleOperation(state, operation, 0, error) && accepted;
}

bool WidgetLuaLifecycle::PushRenderArguments(lua_State* state,
    PushContext pushContext) const
{
    if (!state) return false;
    const int base = lua_gettop(state);
    bool accepted = false;
    std::string error;
    auto operation = [&](lua_State* inner) {
        accepted = PushRenderArgumentsUnprotected(inner, pushContext, error);
        return accepted ? 2 : 0;
    };
    const bool protectedResult = ProtectLifecycleOperation(state, operation, 2, error);
    if (!protectedResult || !accepted) lua_settop(state, base);
    return protectedResult && accepted;
}

bool WidgetLuaLifecycle::Event(lua_State* state, int definitionRef,
    PushContext pushContext, int eventIndex, bool& invoked, std::string& error) const
{
    invoked = false;
    bool accepted = false;
    auto operation = [&](lua_State* inner) {
        accepted = EventUnprotected(inner, definitionRef, pushContext, 2, invoked, error);
        return 0;
    };
    return ProtectLifecycleOperation(state, operation, 0, error, eventIndex) && accepted;
}

bool WidgetLuaLifecycle::Menu(lua_State* state, int definitionRef,
    PushContext pushContext, int requestIndex, bool& invoked, std::string& error) const
{
    invoked = false;
    if (!state) { error = "invalid widget lifecycle menu context"; return false; }
    const int base = lua_gettop(state);
    bool accepted = false;
    auto operation = [&](lua_State* inner) {
        accepted = MenuUnprotected(inner, definitionRef, pushContext, 2, invoked, error);
        return accepted && invoked ? 1 : 0;
    };
    const bool protectedResult = ProtectLifecycleOperation(
        state, operation, 1, error, requestIndex);
    if (!protectedResult || !accepted || !invoked) lua_settop(state, base);
    return protectedResult && accepted;
}

bool WidgetLuaLifecycle::Dispose(lua_State* state, int definitionRef,
    PushContext pushContext, const char* reason, std::string& error)
{
    error.clear();
    if (disposeInvoked_) return true;
    if (!setupCompleted_) { disposeInvoked_ = true; return true; }
    if (!state)
    {
        disposeInvoked_ = true;
        error = "invalid widget lifecycle dispose context";
        return false;
    }
    bool accepted = false;
    auto operation = [&](lua_State* inner) {
        accepted = DisposeUnprotected(inner, definitionRef, pushContext, reason, error);
        return 0;
    };
    return ProtectLifecycleOperation(state, operation, 0, error) && accepted;
}

bool WidgetLuaLifecycle::SetupUnprotected(lua_State* state, int definitionRef,
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

    if (!PushMeasuredContext(state, pushContext, error))
    {
        lua_settop(state, entryTop);
        return false;
    }
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

bool WidgetLuaLifecycle::PushRenderArgumentsUnprotected(lua_State* state,
    PushContext pushContext, std::string& error) const
{
    if (!state || !pushContext || !setupCompleted_ || disposeInvoked_)
        return false;
    if (!PushMeasuredContext(state, pushContext, error)) return false;
    PushModel(state, modelRef_);
    return true;
}

bool WidgetLuaLifecycle::EventUnprotected(lua_State* state, int definitionRef,
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

    if (!PushMeasuredContext(state, pushContext, error))
    {
        lua_settop(state, entryTop);
        return false;
    }
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

bool WidgetLuaLifecycle::MenuUnprotected(lua_State* state, int definitionRef,
    PushContext pushContext, int requestIndex, bool& invoked,
    std::string& error) const
{
    invoked = false;
    error.clear();
    if (!state || !pushContext || !setupCompleted_ || disposeInvoked_ ||
        definitionRef == LUA_NOREF || definitionRef == LUA_REFNIL ||
        !lua_istable(state, requestIndex))
    {
        error = "invalid widget lifecycle menu context";
        return false;
    }

    requestIndex = lua_absindex(state, requestIndex);
    const int entryTop = lua_gettop(state);
    lua_rawgeti(state, LUA_REGISTRYINDEX, definitionRef);
    if (!lua_istable(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle definition is unavailable";
        return false;
    }
    lua_getfield(state, -1, "menu");
    if (lua_isnil(state, -1))
    {
        lua_settop(state, entryTop);
        return true;
    }
    if (!lua_isfunction(state, -1))
    {
        lua_settop(state, entryTop);
        error = "widget lifecycle menu must be a function";
        return false;
    }

    if (!PushMeasuredContext(state, pushContext, error))
    {
        lua_settop(state, entryTop);
        return false;
    }
    PushModel(state, modelRef_);
    lua_pushvalue(state, requestIndex);
    invoked = true;
    if (snowdesktop::lua_runtime::ProtectedCall(
            state, 3, 1, LifecycleInstructionBudget,
            LifecycleTimeBudget) != LUA_OK)
    {
        error = PopLuaError(state, "widget menu failed");
        lua_settop(state, entryTop);
        return false;
    }
    // Remove the definition table while retaining the callback result.
    lua_remove(state, entryTop + 1);
    return true;
}

bool WidgetLuaLifecycle::DisposeUnprotected(lua_State* state, int definitionRef,
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

    if (!PushMeasuredContext(state, pushContext, error))
    {
        lua_settop(state, entryTop);
        return false;
    }
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
