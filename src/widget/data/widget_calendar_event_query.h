#pragma once

#include "system/calendar/calendar_service.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct CalendarEventQueryOptions
{
    std::string query;
    std::uint64_t offset = 0;
    std::size_t limit = 512;
};

struct CalendarEventPage
{
    std::vector<snowdesktop::calendar::CalendarEvent> events;
    std::uint64_t total = 0;
    bool hasMore = false;
    std::uint64_t nextOffset = 0;
};

bool ValidateCalendarEventQuery(const CalendarEventQueryOptions& options,
    std::string& error);
// Input order is the CalendarService's stable order. Matching is a literal,
// case-insensitive Unicode substring; filtering always precedes the page bound.
CalendarEventPage QueryCalendarEvents(
    const std::vector<snowdesktop::calendar::CalendarEvent>& events,
    const CalendarEventQueryOptions& options);
}
