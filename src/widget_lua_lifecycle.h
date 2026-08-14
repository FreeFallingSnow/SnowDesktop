#pragma once

#include <string>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace snowdesktop::widget_runtime
{
class WidgetLuaLifecycle
{
public:
    using PushContext = void (*)(lua_State* state);

    WidgetLuaLifecycle() = default;
    WidgetLuaLifecycle(const WidgetLuaLifecycle&) = delete;
    WidgetLuaLifecycle& operator=(const WidgetLuaLifecycle&) = delete;
    WidgetLuaLifecycle(WidgetLuaLifecycle&& other) noexcept;
    WidgetLuaLifecycle& operator=(WidgetLuaLifecycle&& other) noexcept;

    bool Setup(lua_State* state, int definitionRef,
        PushContext pushContext, std::string& error);
    bool PushRenderArguments(lua_State* state,
        PushContext pushContext) const;
    bool Event(lua_State* state, int definitionRef,
        PushContext pushContext, int eventIndex,
        bool& invoked, std::string& error) const;
    bool Menu(lua_State* state, int definitionRef,
        PushContext pushContext, int requestIndex,
        bool& invoked, std::string& error) const;
    bool Dispose(lua_State* state, int definitionRef,
        PushContext pushContext, const char* reason,
        std::string& error);
    void Release(lua_State* state) noexcept;

    bool SetupCompleted() const noexcept;
    bool DisposeInvoked() const noexcept;

private:
    int modelRef_ = LUA_NOREF;
    bool setupCompleted_ = false;
    bool disposeInvoked_ = false;
};
}
