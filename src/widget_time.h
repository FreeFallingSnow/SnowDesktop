#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop::widget_time
{
enum class TimeError
{
    None,
    InvalidTimeZone,
    OutOfRange,
    InvalidLocalTime,
    FormattingFailed,
};

enum class DateStyle
{
    None,
    Short,
    Long,
};

enum class TimeStyle
{
    None,
    Short,
    Long,
};

struct DateTimeParts
{
    int year = 0;
    int month = 0;
    int day = 0;
    int weekday = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int millisecond = 0;
};

struct AddDelta
{
    std::int64_t years = 0;
    std::int64_t months = 0;
    std::int64_t days = 0;
    std::int64_t hours = 0;
    std::int64_t minutes = 0;
    std::int64_t seconds = 0;
    std::int64_t milliseconds = 0;
};

TimeError Parts(std::int64_t timestampMilliseconds,
    std::string_view timeZone, DateTimeParts& result) noexcept;

TimeError Format(std::int64_t timestampMilliseconds,
    std::string_view locale, std::string_view timeZone,
    DateStyle dateStyle, TimeStyle timeStyle, std::string& result);

TimeError Add(std::int64_t timestampMilliseconds,
    const AddDelta& delta, std::string_view timeZone,
    std::int64_t& result) noexcept;

const char* DescribeError(TimeError error) noexcept;
}
