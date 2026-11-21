#pragma once

#include <windows.h>

struct LuaWidgetSurfaceContext
{
    UINT dpiX = USER_DEFAULT_SCREEN_DPI;
    UINT dpiY = USER_DEFAULT_SCREEN_DPI;
    RECT monitorBounds{};
    RECT workArea{};
    bool monitorAvailable = false;
    bool primaryMonitor = false;
};

struct LuaWidgetContextState
{
    LuaWidgetSurfaceContext surface;
    bool visible = false;
    bool preview = false;
    bool focused = false;
    bool selected = false;
};
