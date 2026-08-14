#include "widget_time.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::int64_t Timestamp(int yearValue, unsigned monthValue,
    unsigned dayValue, int hourValue = 0, int minuteValue = 0,
    int secondValue = 0, int millisecondValue = 0)
{
    using namespace std::chrono;
    const sys_time<milliseconds> value =
        sys_days{ year{ yearValue } / monthValue / dayValue } +
        hours{ hourValue } + minutes{ minuteValue } +
        seconds{ secondValue } + milliseconds{ millisecondValue };
    return value.time_since_epoch().count();
}

void TestParts()
{
    snowdesktop::widget_time::DateTimeParts parts;
    const auto error = snowdesktop::widget_time::Parts(
        Timestamp(2024, 2, 29, 13, 5, 7, 321), "utc", parts);
    Check(error == snowdesktop::widget_time::TimeError::None &&
            parts.year == 2024 && parts.month == 2 && parts.day == 29 &&
            parts.hour == 13 && parts.minute == 5 && parts.second == 7 &&
            parts.millisecond == 321,
        "UTC parts must preserve calendar and millisecond fields");
    Check(snowdesktop::widget_time::Parts(0, "invalid", parts) ==
            snowdesktop::widget_time::TimeError::InvalidTimeZone,
        "parts must reject unsupported time zones");
}

void TestFormatting()
{
    using snowdesktop::widget_time::DateStyle;
    using snowdesktop::widget_time::TimeError;
    using snowdesktop::widget_time::TimeStyle;
    const auto timestamp = Timestamp(2024, 2, 29, 13, 5, 7);
    std::string english;
    std::string german;
    Check(snowdesktop::widget_time::Format(timestamp, "en-US", "utc",
            DateStyle::Short, TimeStyle::Short, english) == TimeError::None &&
            english.find("2024") != std::string::npos,
        "English date-time formatting must produce a localized value");
    Check(snowdesktop::widget_time::Format(timestamp, "de-DE", "utc",
            DateStyle::Short, TimeStyle::Short, german) == TimeError::None &&
            german.find("2024") != std::string::npos && german != english,
        "explicit locales must produce distinct regional formatting");
    Check(snowdesktop::widget_time::Format(timestamp, "en-US", "utc",
            DateStyle::None, TimeStyle::None, english) ==
            TimeError::FormattingFailed,
        "formatting must reject an empty date and time selection");
    Check(snowdesktop::widget_time::Format(timestamp, "en-US", "invalid",
            DateStyle::Short, TimeStyle::Short, english) ==
            TimeError::InvalidTimeZone,
        "formatting must reject unsupported time zones");
}

void TestCalendarAddition()
{
    using snowdesktop::widget_time::Add;
    using snowdesktop::widget_time::AddDelta;
    using snowdesktop::widget_time::TimeError;
    std::int64_t result = 0;

    AddDelta oneYear;
    oneYear.years = 1;
    Check(Add(Timestamp(2024, 2, 29, 13, 5), oneYear, "utc", result) ==
            TimeError::None && result == Timestamp(2025, 2, 28, 13, 5),
        "calendar year addition must clamp leap day safely");

    AddDelta oneMonth;
    oneMonth.months = 1;
    Check(Add(Timestamp(2024, 1, 31, 8), oneMonth, "utc", result) ==
            TimeError::None && result == Timestamp(2024, 2, 29, 8),
        "calendar month addition must clamp to the target month");

    AddDelta mixed;
    mixed.days = 1;
    mixed.hours = 2;
    mixed.minutes = 3;
    Check(Add(Timestamp(2024, 3, 1, 1), mixed, "utc", result) ==
            TimeError::None && result == Timestamp(2024, 3, 2, 3, 3),
        "calendar days and fixed time units must compose predictably");

    Check(Add(0, {}, "invalid", result) == TimeError::InvalidTimeZone,
        "calendar addition must reject unsupported time zones");
}
}

int main()
{
    TestParts();
    TestFormatting();
    TestCalendarAddition();
    std::cout << "widget time tests passed\n";
    return 0;
}
