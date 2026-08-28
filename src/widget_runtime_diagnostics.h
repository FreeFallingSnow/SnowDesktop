#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct LogEntry
{
    std::string key;
    std::string level;
    std::string message;
};

class DiagnosticsLog
{
public:
    static constexpr std::size_t MaxEntries = 200;

    void Add(std::string key, std::string level, std::string message);
    std::vector<LogEntry> EntriesFor(std::string_view key) const;
    std::size_t Size() const noexcept;

private:
    std::deque<LogEntry> entries_;
};
}
