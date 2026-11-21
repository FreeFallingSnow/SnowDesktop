#pragma once

struct lua_State;

namespace snowdesktop::widget_runtime
{
int lua_ScheduleAt(lua_State* state);
int lua_ScheduleTimeline(lua_State* state);
int lua_ScheduleEvery(lua_State* state);
int lua_ScheduleAfter(lua_State* state);
int lua_ScheduleCancel(lua_State* state);
int lua_AnimationRequestFrame(lua_State* state);
int lua_AnimationCancelFrame(lua_State* state);
}
