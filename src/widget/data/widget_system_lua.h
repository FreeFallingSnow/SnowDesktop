#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_SystemCpu(lua_State* state);
int lua_SystemMemory(lua_State* state);
int lua_SystemBattery(lua_State* state);
int lua_SystemNetwork(lua_State* state);
int lua_SystemGpu(lua_State* state);
int lua_MediaCurrent(lua_State* state);
int lua_MediaPlayPause(lua_State* state);
int lua_MediaNext(lua_State* state);
int lua_MediaPrevious(lua_State* state);
int lua_SystemInfoV2(lua_State* state);
int lua_SystemUptime(lua_State* state);
}
