#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace snowdesktop::file_date_category
{
// Both timestamps describe local calendar dates. Week buckets take precedence
// over month buckets, just as they do in the displayed file lists.
inline std::wstring ForLocalDate(SYSTEMTIME file, SYSTEMTIME now,
    bool futureIsToday = false)
{
    file.wHour = file.wMinute = file.wSecond = file.wMilliseconds = 0;
    now.wHour = now.wMinute = now.wSecond = now.wMilliseconds = 0;
    FILETIME fileTicks{}, nowTicks{};
    if (!SystemTimeToFileTime(&file, &fileTicks) || !SystemTimeToFileTime(&now, &nowTicks))
        return L"older";
    const auto ticks = [](FILETIME value) {
        return (static_cast<std::int64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };
    const auto days = (ticks(nowTicks) - ticks(fileTicks)) / 864000000000LL;
    if (days == 0 || (futureIsToday && days < 0)) return L"today";
    if (days == 1) return L"yesterday";
    const int weekDay = now.wDayOfWeek == 0 ? 7 : now.wDayOfWeek;
    if (days >= 1 && days < weekDay) return L"this_week";
    if (days >= weekDay && days < weekDay + 7) return L"last_week";
    if (file.wYear == now.wYear && file.wMonth == now.wMonth) return L"this_month";
    const int previousMonth = now.wMonth == 1 ? 12 : now.wMonth - 1;
    const int previousYear = now.wMonth == 1 ? now.wYear - 1 : now.wYear;
    if (file.wYear == previousYear && file.wMonth == previousMonth) return L"last_month";
    return file.wYear == now.wYear ? L"this_year" : L"older";
}
}
