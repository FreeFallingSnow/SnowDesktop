#include "lua_runtime.h"

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::lua_runtime
{
namespace
{
constexpr int kHookInstructionStep = 10000;
thread_local int g_protectedCallDepth = 0;

LuaRuntimeQuota* GetQuota(lua_State* state)
{
    lua_getfield(state, LUA_REGISTRYINDEX, "__quota_ptr");
    auto* quota = static_cast<LuaRuntimeQuota*>(
        lua_touserdata(state, -1));
    lua_pop(state, 1);
    return quota;
}

void QuotaHook(lua_State* state, lua_Debug*)
{
    LuaRuntimeQuota* quota = GetQuota(state);
    if (!quota)
        return;

    quota->instructionsRemaining -= kHookInstructionStep;
    if (quota->instructionsRemaining <= 0 ||
        std::chrono::steady_clock::now() >= quota->deadline)
    {
        quota->executionExceeded = true;
        luaL_error(state, "widget execution quota exceeded");
    }
}

int Traceback(lua_State* state)
{
    const char* message = lua_tostring(state, 1);
    luaL_traceback(state, state, message ? message : "(Lua error)", 1);
    return 1;
}

class HookFrame
{
public:
    HookFrame(lua_State* state, LuaRuntimeQuota* quota,
        std::int64_t instructionBudget,
        std::chrono::milliseconds timeBudget)
        : state_(state), quota_(quota), previousHook_(lua_gethook(state)),
          previousMask_(lua_gethookmask(state)),
          previousCount_(lua_gethookcount(state))
    {
        if (!quota_)
            return;

        nestedQuotaFrame_ = previousHook_ == QuotaHook;
        if (nestedQuotaFrame_)
            return;

        ownsHook_ = true;
        quota_->instructionsRemaining = instructionBudget;
        quota_->deadline = std::chrono::steady_clock::now() + timeBudget;
        quota_->executionExceeded = false;
        lua_sethook(state_, QuotaHook, LUA_MASKCOUNT, kHookInstructionStep);
    }

    ~HookFrame()
    {
        if (ownsHook_)
            lua_sethook(
                state_, previousHook_, previousMask_, previousCount_);
    }

    void Suspend() const
    {
        // Stack cleanup and traceback removal happen outside the protected Lua
        // call. No quota error may escape from that cleanup path.
        if (ownsHook_)
            lua_sethook(state_, nullptr, 0, 0);
    }

private:
    lua_State* state_ = nullptr;
    LuaRuntimeQuota* quota_ = nullptr;
    lua_Hook previousHook_ = nullptr;
    int previousMask_ = 0;
    int previousCount_ = 0;
    bool nestedQuotaFrame_ = false;
    bool ownsHook_ = false;
};

class ProtectedCallDepthFrame
{
public:
    ProtectedCallDepthFrame() noexcept
    {
        ++g_protectedCallDepth;
    }

    ~ProtectedCallDepthFrame()
    {
        --g_protectedCallDepth;
    }

    ProtectedCallDepthFrame(const ProtectedCallDepthFrame&) = delete;
    ProtectedCallDepthFrame& operator=(
        const ProtectedCallDepthFrame&) = delete;
};
}

int ProtectedCall(lua_State* state, int arguments, int results,
    std::int64_t instructionBudget,
    std::chrono::milliseconds timeBudget)
{
    if (!state)
        return LUA_ERRRUN;

    LuaRuntimeQuota* quota = GetQuota(state);
    const int functionIndex = lua_gettop(state) - arguments;
    if (g_protectedCallDepth >= kMaxProtectedCallDepth)
    {
        lua_settop(state, functionIndex - 1);
        lua_pushliteral(
            state, "widget callback nesting limit exceeded");
        if (quota)
            quota->executionExceeded = true;
        return LUA_ERRRUN;
    }

    ProtectedCallDepthFrame depthFrame;
    HookFrame hookFrame(
        state, quota, instructionBudget, timeBudget);
    const auto started = std::chrono::steady_clock::now();
    lua_pushcfunction(state, Traceback);
    lua_insert(state, functionIndex);
    const int status = lua_pcall(
        state, arguments, results, functionIndex);

    hookFrame.Suspend();
    lua_remove(state, functionIndex);
    if (quota)
    {
        quota->lastExecutionMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
    }
    return status;
}
}
