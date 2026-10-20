#pragma once

#include "widget/data/widget_calendar_event_query.h"

#include <optional>
#include <string>

extern "C" { struct lua_State; }

namespace snowdesktop::widget_api
{
// Reads only raw option fields and preserves the caller's Lua stack. Absence
// stays nullopt so old requests keep the existing 512-event behavior.
bool ParseCalendarEventQueryOptions(lua_State* state, int optionsIndex,
    std::optional<snowdesktop::widget_runtime::CalendarEventQueryOptions>& result,
    std::string& error);
}
