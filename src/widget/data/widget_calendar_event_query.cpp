#include "widget_calendar_event_query.h"

#include <windows.h>
#include <algorithm>
#include <optional>
#include <string_view>

namespace snowdesktop::widget_runtime
{
namespace
{
std::optional<std::wstring> Decode(std::string_view text)
{
    if (text.empty()) return std::wstring{};
    if (text.size() > 512) return std::nullopt;
    const auto length = static_cast<int>(text.size());
    const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), length, nullptr, 0);
    if (count <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), length,
        result.data(), count) != count) return std::nullopt;
    return result;
}
}

bool ValidateCalendarEventQuery(const CalendarEventQueryOptions& options,
    std::string& error)
{
    error.clear();
    if (options.query.find('\0') != std::string::npos || !Decode(options.query))
    {
        error = "calendar query must be valid UTF-8, at most 512 bytes, without NUL";
        return false;
    }
    if (options.limit < 1 || options.limit > 512)
    {
        error = "calendar query limit must be from 1 to 512";
        return false;
    }
    return true;
}

CalendarEventPage QueryCalendarEvents(
    const std::vector<snowdesktop::calendar::CalendarEvent>& events,
    const CalendarEventQueryOptions& options)
{
    CalendarEventPage page;
    std::string error;
    if (!ValidateCalendarEventQuery(options, error)) return page;
    const auto query = Decode(options.query);
    for (const auto& event : events)
    {
        if (!options.query.empty())
        {
            const auto title = Decode(event.title);
            if (!title || FindStringOrdinal(FIND_FROMSTART, title->data(),
                static_cast<int>(title->size()), query->data(),
                static_cast<int>(query->size()), TRUE) < 0) continue;
        }
        const auto index = page.total++;
        if (index >= options.offset && page.events.size() < options.limit)
            page.events.push_back(event);
    }
    // Avoid adding a caller-controlled offset to the limit before bounding it.
    const auto start = std::min(options.offset, page.total);
    page.nextOffset = start + page.events.size();
    page.hasMore = page.nextOffset < page.total;
    return page;
}
}
