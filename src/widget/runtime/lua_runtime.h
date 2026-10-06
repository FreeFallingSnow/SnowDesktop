#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

extern "C" {
#include <lua.h>
}

struct LuaRuntimeQuota
{
    std::size_t memoryBytes = 0;
    std::size_t memoryLimit = 16u * 1024u * 1024u;
    std::int64_t instructionsRemaining = 0;
    std::chrono::steady_clock::time_point deadline{};
    bool memoryExceeded = false;
    bool executionExceeded = false;
    double lastExecutionMs = 0.0;
};

namespace snowdesktop::lua_runtime
{
inline constexpr std::int64_t kDefaultInstructionBudget = 500000;
inline constexpr auto kDefaultTimeBudget = std::chrono::milliseconds(50);
inline constexpr int kMaxProtectedCallDepth = 16;

/**
 * Restores the current Lua API stack frame to its entry height. This is safe
 * for nested calls on the same lua_State because lua_gettop/lua_settop are
 * relative to the currently active C API frame.
 */
class StackGuard
{
public:
    explicit StackGuard(lua_State* state) noexcept
        : state_(state), top_(state ? lua_gettop(state) : 0)
    {
    }

    ~StackGuard()
    {
        if (state_)
            lua_settop(state_, top_);
    }

    StackGuard(const StackGuard&) = delete;
    StackGuard& operator=(const StackGuard&) = delete;

private:
    lua_State* state_ = nullptr;
    int top_ = 0;
};

/**
 * Calls the function already present below `arguments` on the Lua stack.
 * The quota hook is installed only while lua_pcall is active and any previous
 * hook/quota frame is restored, allowing synchronous nested Lua callbacks.
 */
int ProtectedCall(lua_State* state, int arguments, int results,
    std::int64_t instructionBudget = kDefaultInstructionBudget,
    std::chrono::milliseconds timeBudget = kDefaultTimeBudget);
}
