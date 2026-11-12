#pragma once

#include "calendar_service.h"

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::calendar
{

// The calendar model exposes single-day events. Expand into that format over
// a bounded window while retaining the original identity of moved instances.
std::optional<std::vector<CalendarEvent>> ParseIcsEvents(
    const std::string& text, std::string_view sourceId, const std::string& today,
    std::stop_token token);

} // namespace snowdesktop::calendar
