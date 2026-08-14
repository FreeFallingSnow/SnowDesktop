#include "widget_time.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string>

namespace snowdesktop::widget_time
{
namespace
{
constexpr std::int64_t kUnixFileTimeOffsetMilliseconds = 11644473600000LL;
constexpr std::uint64_t kFileTimeTicksPerMillisecond = 10000ULL;
constexpr std::uint64_t kFileTimeTicksPerDay = 864000000000ULL;
constexpr std::int64_t kMaximumUnixMilliseconds =
    static_cast<std::int64_t>(
        (std::numeric_limits<std::uint64_t>::max)() /
        kFileTimeTicksPerMillisecond) - kUnixFileTimeOffsetMilliseconds;

bool EqualsAsciiInsensitive(std::string_view left, std::string_view right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(left[index])) !=
            std::tolower(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()),
            result.data(), length) <= 0)
        return {};
    return result;
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(length, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0,
            value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::wstring ResolveLocale(std::string_view locale)
{
    const std::wstring requested = Utf8ToWide(locale);
    std::array<wchar_t, LOCALE_NAME_MAX_LENGTH> resolved{};
    if (!requested.empty() && ResolveLocaleName(requested.c_str(),
            resolved.data(), static_cast<int>(resolved.size())) > 0)
        return resolved.data();
    return L"en-US";
}

struct ResolvedTimeZone
{
    bool utc = false;
    DYNAMIC_TIME_ZONE_INFORMATION value{};
};

bool ResolveTimeZone(std::string_view name, ResolvedTimeZone& result)
{
    if (EqualsAsciiInsensitive(name, "utc"))
    {
        result.utc = true;
        return true;
    }
    if (!EqualsAsciiInsensitive(name, "local")) return false;
    result.utc = false;
    return GetDynamicTimeZoneInformation(&result.value) !=
        TIME_ZONE_ID_INVALID;
}

std::uint64_t FileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER integer{};
    integer.LowPart = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

FILETIME FileTimeFromValue(std::uint64_t value)
{
    ULARGE_INTEGER integer{};
    integer.QuadPart = value;
    return { integer.LowPart, integer.HighPart };
}

bool UnixMillisecondsToUtcSystemTime(std::int64_t milliseconds,
    SYSTEMTIME& result)
{
    if (milliseconds < -kUnixFileTimeOffsetMilliseconds ||
        milliseconds > kMaximumUnixMilliseconds)
        return false;
    const auto shifted = static_cast<std::uint64_t>(
        milliseconds + kUnixFileTimeOffsetMilliseconds);
    const FILETIME fileTime = FileTimeFromValue(
        shifted * kFileTimeTicksPerMillisecond);
    return FileTimeToSystemTime(&fileTime, &result) != FALSE;
}

bool UtcSystemTimeToUnixMilliseconds(const SYSTEMTIME& value,
    std::int64_t& result)
{
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&value, &fileTime)) return false;
    const std::uint64_t milliseconds =
        FileTimeValue(fileTime) / kFileTimeTicksPerMillisecond;
    if (milliseconds > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)()))
        return false;
    result = static_cast<std::int64_t>(milliseconds) -
        kUnixFileTimeOffsetMilliseconds;
    return true;
}

TimeError ToZonedSystemTime(std::int64_t milliseconds,
    std::string_view timeZone, SYSTEMTIME& result)
{
    SYSTEMTIME utc{};
    if (!UnixMillisecondsToUtcSystemTime(milliseconds, utc))
        return TimeError::OutOfRange;
    ResolvedTimeZone zone;
    if (!ResolveTimeZone(timeZone, zone))
        return TimeError::InvalidTimeZone;
    if (zone.utc)
    {
        result = utc;
        return TimeError::None;
    }
    return SystemTimeToTzSpecificLocalTimeEx(&zone.value, &utc, &result)
        ? TimeError::None : TimeError::OutOfRange;
}

TimeError FromZonedSystemTime(const SYSTEMTIME& value,
    std::string_view timeZone, std::int64_t& result)
{
    ResolvedTimeZone zone;
    if (!ResolveTimeZone(timeZone, zone))
        return TimeError::InvalidTimeZone;
    SYSTEMTIME utc{};
    if (zone.utc)
        utc = value;
    else if (!TzSpecificLocalTimeToSystemTimeEx(&zone.value, &value, &utc))
        return TimeError::InvalidLocalTime;
    return UtcSystemTimeToUnixMilliseconds(utc, result)
        ? TimeError::None : TimeError::OutOfRange;
}

bool IsLeapYear(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int DaysInMonth(int year, int month)
{
    static constexpr std::array days = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    return month == 2 && IsLeapYear(year)
        ? 29 : days[static_cast<std::size_t>(month - 1)];
}

bool ShiftLocalDays(SYSTEMTIME& value, std::int64_t days)
{
    if (days == 0) return true;
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&value, &fileTime)) return false;
    std::uint64_t ticks = FileTimeValue(fileTime);
    const std::uint64_t magnitude = days < 0
        ? static_cast<std::uint64_t>(-(days + 1)) + 1
        : static_cast<std::uint64_t>(days);
    if (magnitude >
        (std::numeric_limits<std::uint64_t>::max)() /
            kFileTimeTicksPerDay)
        return false;
    const std::uint64_t delta = magnitude * kFileTimeTicksPerDay;
    if (days < 0)
    {
        if (ticks < delta) return false;
        ticks -= delta;
    }
    else
    {
        if (ticks > (std::numeric_limits<std::uint64_t>::max)() - delta)
            return false;
        ticks += delta;
    }
    const FILETIME shifted = FileTimeFromValue(ticks);
    return FileTimeToSystemTime(&shifted, &value) != FALSE;
}

bool CheckedAdd(std::int64_t& value, std::int64_t addend)
{
    if ((addend > 0 && value >
            (std::numeric_limits<std::int64_t>::max)() - addend) ||
        (addend < 0 && value <
            (std::numeric_limits<std::int64_t>::min)() - addend))
        return false;
    value += addend;
    return true;
}

bool CheckedAddProduct(std::int64_t& value,
    std::int64_t amount, std::int64_t factor)
{
    if (amount > (std::numeric_limits<std::int64_t>::max)() / factor ||
        amount < (std::numeric_limits<std::int64_t>::min)() / factor)
        return false;
    return CheckedAdd(value, amount * factor);
}

TimeError FormatDate(const std::wstring& locale,
    const SYSTEMTIME& value, DateStyle style, std::wstring& result)
{
    if (style == DateStyle::None)
    {
        result.clear();
        return TimeError::None;
    }
    const DWORD flags = style == DateStyle::Long
        ? DATE_LONGDATE : DATE_SHORTDATE;
    const int required = GetDateFormatEx(locale.c_str(), flags, &value,
        nullptr, nullptr, 0, nullptr);
    if (required <= 1) return TimeError::FormattingFailed;
    result.assign(static_cast<std::size_t>(required), L'\0');
    if (GetDateFormatEx(locale.c_str(), flags, &value, nullptr,
            result.data(), required, nullptr) <= 0)
        return TimeError::FormattingFailed;
    result.resize(static_cast<std::size_t>(required - 1));
    return TimeError::None;
}

TimeError FormatTime(const std::wstring& locale,
    const SYSTEMTIME& value, TimeStyle style, std::wstring& result)
{
    if (style == TimeStyle::None)
    {
        result.clear();
        return TimeError::None;
    }
    const DWORD flags = style == TimeStyle::Short ? TIME_NOSECONDS : 0;
    const int required = GetTimeFormatEx(locale.c_str(), flags, &value,
        nullptr, nullptr, 0);
    if (required <= 1) return TimeError::FormattingFailed;
    result.assign(static_cast<std::size_t>(required), L'\0');
    if (GetTimeFormatEx(locale.c_str(), flags, &value, nullptr,
            result.data(), required) <= 0)
        return TimeError::FormattingFailed;
    result.resize(static_cast<std::size_t>(required - 1));
    return TimeError::None;
}
}

TimeError Parts(std::int64_t timestampMilliseconds,
    std::string_view timeZone, DateTimeParts& result) noexcept
{
    SYSTEMTIME value{};
    const TimeError error = ToZonedSystemTime(
        timestampMilliseconds, timeZone, value);
    if (error != TimeError::None) return error;
    result = {
        value.wYear,
        value.wMonth,
        value.wDay,
        value.wDayOfWeek + 1,
        value.wHour,
        value.wMinute,
        value.wSecond,
        value.wMilliseconds,
    };
    return TimeError::None;
}

TimeError Format(std::int64_t timestampMilliseconds,
    std::string_view locale, std::string_view timeZone,
    DateStyle dateStyle, TimeStyle timeStyle, std::string& result)
{
    result.clear();
    if (dateStyle == DateStyle::None && timeStyle == TimeStyle::None)
        return TimeError::FormattingFailed;
    SYSTEMTIME value{};
    TimeError error = ToZonedSystemTime(
        timestampMilliseconds, timeZone, value);
    if (error != TimeError::None) return error;
    const std::wstring resolvedLocale = ResolveLocale(locale);
    std::wstring date;
    std::wstring time;
    error = FormatDate(resolvedLocale, value, dateStyle, date);
    if (error != TimeError::None) return error;
    error = FormatTime(resolvedLocale, value, timeStyle, time);
    if (error != TimeError::None) return error;
    std::wstring combined = date;
    if (!date.empty() && !time.empty()) combined += L' ';
    combined += time;
    result = WideToUtf8(combined);
    return result.empty() ? TimeError::FormattingFailed : TimeError::None;
}

TimeError Add(std::int64_t timestampMilliseconds,
    const AddDelta& delta, std::string_view timeZone,
    std::int64_t& result) noexcept
{
    SYSTEMTIME local{};
    TimeError error = ToZonedSystemTime(
        timestampMilliseconds, timeZone, local);
    if (error != TimeError::None) return error;

    if (delta.years != 0 || delta.months != 0)
    {
        if (delta.years > 1000000 || delta.years < -1000000 ||
            delta.months > 1000000 || delta.months < -1000000)
            return TimeError::OutOfRange;
        const std::int64_t currentMonth =
            static_cast<std::int64_t>(local.wYear) * 12 + local.wMonth - 1;
        const std::int64_t targetMonth = currentMonth +
            delta.years * 12 + delta.months;
        constexpr std::int64_t minimumMonth = 1601 * 12;
        constexpr std::int64_t maximumMonth = 9999 * 12 + 11;
        if (targetMonth < minimumMonth || targetMonth > maximumMonth)
            return TimeError::OutOfRange;
        const int targetYear = static_cast<int>(targetMonth / 12);
        const int targetMonthOfYear = static_cast<int>(targetMonth % 12) + 1;
        local.wYear = static_cast<WORD>(targetYear);
        local.wMonth = static_cast<WORD>(targetMonthOfYear);
        local.wDay = static_cast<WORD>(std::min<int>(
            local.wDay, DaysInMonth(targetYear, targetMonthOfYear)));
    }
    if (!ShiftLocalDays(local, delta.days)) return TimeError::OutOfRange;

    std::int64_t calendarResult = 0;
    error = FromZonedSystemTime(local, timeZone, calendarResult);
    if (error != TimeError::None) return error;
    std::int64_t fixedDelta = delta.milliseconds;
    if (!CheckedAddProduct(fixedDelta, delta.seconds, 1000) ||
        !CheckedAddProduct(fixedDelta, delta.minutes, 60000) ||
        !CheckedAddProduct(fixedDelta, delta.hours, 3600000) ||
        !CheckedAdd(calendarResult, fixedDelta))
        return TimeError::OutOfRange;
    SYSTEMTIME validated{};
    if (!UnixMillisecondsToUtcSystemTime(calendarResult, validated))
        return TimeError::OutOfRange;
    result = calendarResult;
    return TimeError::None;
}

const char* DescribeError(TimeError error) noexcept
{
    switch (error)
    {
    case TimeError::None: return "none";
    case TimeError::InvalidTimeZone: return "invalid time zone";
    case TimeError::OutOfRange: return "timestamp is out of range";
    case TimeError::InvalidLocalTime: return "local time does not exist";
    case TimeError::FormattingFailed: return "formatting failed";
    }
    return "unknown error";
}
}
