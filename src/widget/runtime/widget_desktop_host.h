#pragma once

struct lua_State;

int lua_DesktopItems(lua_State* state);
int lua_DesktopSelection(lua_State* state);
int lua_DesktopFind(lua_State* state);
int lua_DesktopFindApplications(lua_State* state);
int lua_EverythingSearch(lua_State* state);
int lua_DesktopOpen(lua_State* state);
int lua_DesktopReveal(lua_State* state);
int lua_DesktopRefresh(lua_State* state);
