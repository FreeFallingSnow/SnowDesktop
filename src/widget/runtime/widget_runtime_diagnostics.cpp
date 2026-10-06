#include "widget_runtime_diagnostics.h"

#include <utility>

namespace snowdesktop::widget_runtime
{
void DiagnosticsLog::Add(
    std::string key,
    std::string level,
    std::string message)
{
    entries_.push_back({
        std::move(key),
        level.empty() ? "info" : std::move(level),
        std::move(message),
    });
    while (entries_.size() > MaxEntries)
        entries_.pop_front();
}

std::vector<LogEntry> DiagnosticsLog::EntriesFor(
    std::string_view key) const
{
    std::vector<LogEntry> result;
    for (const LogEntry& entry : entries_)
    {
        if (entry.key == key)
            result.push_back(entry);
    }
    return result;
}

std::size_t DiagnosticsLog::Size() const noexcept
{
    return entries_.size();
}
}
