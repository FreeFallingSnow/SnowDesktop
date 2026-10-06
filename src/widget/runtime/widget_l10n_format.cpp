#include "widget_l10n_format.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <sstream>

namespace snowdesktop::widget_l10n
{
namespace
{
std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), result.data(), length);
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
    WideCharToMultiByte(CP_UTF8, 0,
        value.data(), static_cast<int>(value.size()), result.data(), length,
        nullptr, nullptr);
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

std::wstring LocaleValue(const std::wstring& locale, LCTYPE type,
    const wchar_t* fallback)
{
    std::array<wchar_t, 32> value{};
    if (GetLocaleInfoEx(locale.c_str(), type, value.data(),
            static_cast<int>(value.size())) > 0)
        return value.data();
    return fallback;
}

int LocaleInteger(const std::wstring& locale, LCTYPE type, int fallback)
{
    const std::wstring value = LocaleValue(locale, type, L"");
    if (value.empty()) return fallback;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    return end && *end == L'\0' ? static_cast<int>(parsed) : fallback;
}

std::string Language(std::string_view locale)
{
    std::string result;
    for (const char ch : locale)
    {
        if (ch == '-' || ch == '_') break;
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    return result.empty() ? "en" : result;
}

std::string Join(const std::vector<std::string>& values,
    std::string_view separator)
{
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index) result.append(separator);
        result += values[index];
    }
    return result;
}

struct DurationUnits
{
    const char* hour;
    const char* minute;
    const char* second;
};

DurationUnits UnitsFor(std::string_view locale)
{
    const std::string language = Language(locale);
    if (language == "zh")
    {
        const bool traditional = locale.find("TW") != std::string_view::npos ||
            locale.find("tw") != std::string_view::npos;
        return traditional
            ? DurationUnits{ "小時", "分鐘", "秒" } // l10n-allow: API unit table
            : DurationUnits{ "小时", "分钟", "秒" }; // l10n-allow: API unit table
    }
    if (language == "ja")
        return { "時間", "分", "秒" }; // l10n-allow: API unit table
    if (language == "ko")
        return { "시간", "분", "초" }; // l10n-allow: API unit table
    if (language == "de")
        return { "Std.", "Min.", "Sek." }; // l10n-allow: API unit table
    if (language == "fr") return { "h", "min", "s" };
    if (language == "es") return { "h", "min", "s" };
    if (language == "pt") return { "h", "min", "s" };
    return { "h", "min", "s" };
}

enum class RelativeUnit
{
    Second,
    Minute,
    Hour,
    Day,
    Week,
    Month,
    Year,
};

std::uint64_t Magnitude(std::int64_t value)
{
    return value < 0
        ? static_cast<std::uint64_t>(-(value + 1)) + 1
        : static_cast<std::uint64_t>(value);
}

std::uint64_t RoundedUnits(std::uint64_t milliseconds,
    std::uint64_t unitMilliseconds)
{
    const std::uint64_t whole = milliseconds / unitMilliseconds;
    const std::uint64_t remainder = milliseconds % unitMilliseconds;
    return whole + (remainder >= (unitMilliseconds + 1) / 2 ? 1 : 0);
}

bool ParseRelativeUnit(std::string_view value, RelativeUnit& unit)
{
    if (value == "second") unit = RelativeUnit::Second;
    else if (value == "minute") unit = RelativeUnit::Minute;
    else if (value == "hour") unit = RelativeUnit::Hour;
    else if (value == "day") unit = RelativeUnit::Day;
    else if (value == "week") unit = RelativeUnit::Week;
    else if (value == "month") unit = RelativeUnit::Month;
    else if (value == "year") unit = RelativeUnit::Year;
    else return false;
    return true;
}

RelativeUnit SelectRelativeUnit(std::uint64_t milliseconds)
{
    constexpr std::uint64_t second = 1000;
    constexpr std::uint64_t minute = 60 * second;
    constexpr std::uint64_t hour = 60 * minute;
    constexpr std::uint64_t day = 24 * hour;
    if (milliseconds < minute) return RelativeUnit::Second;
    if (milliseconds < hour) return RelativeUnit::Minute;
    if (milliseconds < day) return RelativeUnit::Hour;
    if (milliseconds < 7 * day) return RelativeUnit::Day;
    if (milliseconds < 30 * day) return RelativeUnit::Week;
    if (milliseconds < 365 * day) return RelativeUnit::Month;
    return RelativeUnit::Year;
}

std::uint64_t RelativeUnitMilliseconds(RelativeUnit unit)
{
    constexpr std::uint64_t second = 1000;
    constexpr std::uint64_t minute = 60 * second;
    constexpr std::uint64_t hour = 60 * minute;
    constexpr std::uint64_t day = 24 * hour;
    switch (unit)
    {
    case RelativeUnit::Second: return second;
    case RelativeUnit::Minute: return minute;
    case RelativeUnit::Hour: return hour;
    case RelativeUnit::Day: return day;
    case RelativeUnit::Week: return 7 * day;
    case RelativeUnit::Month: return 30 * day;
    case RelativeUnit::Year: return 365 * day;
    }
    return second;
}

std::string RelativeSpecial(std::string_view locale, RelativeUnit unit,
    std::int64_t direction, std::uint64_t count)
{
    const std::string language = Language(locale);
    if (unit == RelativeUnit::Second && count == 0)
    {
        if (language == "zh")
        {
            const bool traditional = locale.find("TW") != std::string_view::npos ||
                locale.find("tw") != std::string_view::npos;
            return traditional ? "現在" : "现在"; // l10n-allow: relative-time term
        }
        if (language == "ja") return "今"; // l10n-allow: relative-time term
        if (language == "ko") return "지금"; // l10n-allow: relative-time term
        if (language == "de") return "jetzt"; // l10n-allow: relative-time term
        if (language == "fr") return "maintenant"; // l10n-allow: relative-time term
        if (language == "es") return "ahora"; // l10n-allow: relative-time term
        if (language == "pt") return "agora"; // l10n-allow: relative-time term
        return "now";
    }
    if (unit != RelativeUnit::Day || count > 1) return {};

    const int offset = count == 0 ? 0 : (direction < 0 ? -1 : 1);
    if (language == "zh")
    {
        static constexpr std::array terms = {
            "昨天", "今天", "明天" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "ja")
    {
        static constexpr std::array terms = {
            "昨日", "今日", "明日" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "ko")
    {
        static constexpr std::array terms = {
            "어제", "오늘", "내일" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "de")
    {
        static constexpr std::array terms = {
            "gestern", "heute", "morgen" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "fr")
    {
        static constexpr std::array terms = {
            "hier", "aujourd'hui", "demain" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "es")
    {
        static constexpr std::array terms = {
            "ayer", "hoy", "mañana" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    if (language == "pt")
    {
        static constexpr std::array terms = {
            "ontem", "hoje", "amanhã" // l10n-allow: relative-time terms
        };
        return terms[static_cast<std::size_t>(offset + 1)];
    }
    static constexpr std::array terms = {
        "yesterday", "today", "tomorrow"
    };
    return terms[static_cast<std::size_t>(offset + 1)];
}

std::string RelativeUnitName(std::string_view locale, RelativeUnit unit,
    bool plural)
{
    const std::string language = Language(locale);
    const std::size_t index = static_cast<std::size_t>(unit);
    if (language == "zh")
    {
        const bool traditional = locale.find("TW") != std::string_view::npos ||
            locale.find("tw") != std::string_view::npos;
        static constexpr std::array simplified = {
            "秒", "分钟", "小时", "天", "周", "个月", "年" // l10n-allow: relative-time units
        };
        static constexpr std::array traditionalUnits = {
            "秒", "分鐘", "小時", "天", "週", "個月", "年" // l10n-allow: relative-time units
        };
        return traditional ? traditionalUnits[index] : simplified[index];
    }
    if (language == "ja")
    {
        static constexpr std::array units = {
            "秒", "分", "時間", "日", "週間", "か月", "年" // l10n-allow: relative-time units
        };
        return units[index];
    }
    if (language == "ko")
    {
        static constexpr std::array units = {
            "초", "분", "시간", "일", "주", "개월", "년" // l10n-allow: relative-time units
        };
        return units[index];
    }
    if (language == "de")
    {
        static constexpr std::array singular = {
            "Sekunde", "Minute", "Stunde", "Tag", "Woche", "Monat", "Jahr" // l10n-allow: relative-time units
        };
        static constexpr std::array pluralUnits = {
            "Sekunden", "Minuten", "Stunden", "Tagen", "Wochen", "Monaten", "Jahren" // l10n-allow: relative-time units
        };
        return plural ? pluralUnits[index] : singular[index];
    }
    if (language == "fr")
    {
        static constexpr std::array singular = {
            "seconde", "minute", "heure", "jour", "semaine", "mois", "an" // l10n-allow: relative-time units
        };
        static constexpr std::array pluralUnits = {
            "secondes", "minutes", "heures", "jours", "semaines", "mois", "ans" // l10n-allow: relative-time units
        };
        return plural ? pluralUnits[index] : singular[index];
    }
    if (language == "es")
    {
        static constexpr std::array singular = {
            "segundo", "minuto", "hora", "día", "semana", "mes", "año" // l10n-allow: relative-time units
        };
        static constexpr std::array pluralUnits = {
            "segundos", "minutos", "horas", "días", "semanas", "meses", "años" // l10n-allow: relative-time units
        };
        return plural ? pluralUnits[index] : singular[index];
    }
    if (language == "pt")
    {
        static constexpr std::array singular = {
            "segundo", "minuto", "hora", "dia", "semana", "mês", "ano" // l10n-allow: relative-time units
        };
        static constexpr std::array pluralUnits = {
            "segundos", "minutos", "horas", "dias", "semanas", "meses", "anos" // l10n-allow: relative-time units
        };
        return plural ? pluralUnits[index] : singular[index];
    }
    static constexpr std::array singular = {
        "second", "minute", "hour", "day", "week", "month", "year"
    };
    static constexpr std::array pluralUnits = {
        "seconds", "minutes", "hours", "days", "weeks", "months", "years"
    };
    return plural ? pluralUnits[index] : singular[index];
}
}

std::string FormatNumber(double value, std::string_view locale,
    NumberOptions options)
{
    if (!std::isfinite(value)) return {};
    options.minimumFractionDigits = std::clamp(
        options.minimumFractionDigits, 0, 6);
    options.maximumFractionDigits = std::clamp(
        options.maximumFractionDigits,
        options.minimumFractionDigits, 6);

    const std::wstring resolved = ResolveLocale(locale);
    std::wstring decimal = LocaleValue(resolved, LOCALE_SDECIMAL, L".");
    std::wstring thousand = LocaleValue(resolved, LOCALE_STHOUSAND, L",");
    const std::wstring groupingText = LocaleValue(
        resolved, LOCALE_SGROUPING, L"3;0");
    int grouping = 3;
    if (!groupingText.empty() && std::iswdigit(groupingText.front()))
        grouping = groupingText.front() - L'0';

    NUMBERFMTW format{};
    format.NumDigits = static_cast<UINT>(options.maximumFractionDigits);
    format.LeadingZero = 1;
    format.Grouping = options.grouping ? static_cast<UINT>(grouping) : 0;
    format.lpDecimalSep = decimal.data();
    format.lpThousandSep = thousand.data();
    format.NegativeOrder = static_cast<UINT>(std::clamp(
        LocaleInteger(resolved, LOCALE_INEGNUMBER, 1), 0, 4));

    std::wostringstream input;
    input.imbue(std::locale::classic());
    input << std::fixed << std::setprecision(options.maximumFractionDigits)
        << value;
    const std::wstring plain = input.str();
    const int required = GetNumberFormatEx(resolved.c_str(), 0,
        plain.c_str(), &format, nullptr, 0);
    if (required <= 1) return {};
    std::wstring formatted(static_cast<std::size_t>(required), L'\0');
    if (GetNumberFormatEx(resolved.c_str(), 0, plain.c_str(), &format,
            formatted.data(), required) <= 0)
        return {};
    formatted.resize(static_cast<std::size_t>(required - 1));

    if (options.minimumFractionDigits < options.maximumFractionDigits)
    {
        const auto decimalAt = formatted.rfind(decimal);
        if (decimalAt != std::wstring::npos)
        {
            int digits = options.maximumFractionDigits;
            while (digits > options.minimumFractionDigits &&
                !formatted.empty() && formatted.back() == L'0')
            {
                formatted.pop_back();
                --digits;
            }
            if (digits == 0 && formatted.ends_with(decimal))
                formatted.resize(formatted.size() - decimal.size());
        }
    }
    return WideToUtf8(formatted);
}

std::string FormatBytes(double bytes, std::string_view locale,
    int base, int maximumFractionDigits)
{
    if (!std::isfinite(bytes) || bytes < 0) return {};
    if (base != 1000 && base != 1024) return {};
    maximumFractionDigits = std::clamp(maximumFractionDigits, 0, 3);
    static constexpr std::array decimalUnits = {
        "B", "KB", "MB", "GB", "TB", "PB"
    };
    static constexpr std::array binaryUnits = {
        "B", "KiB", "MiB", "GiB", "TiB", "PiB"
    };
    const auto& units = base == 1024 ? binaryUnits : decimalUnits;
    std::size_t unit = 0;
    double value = bytes;
    while (value >= static_cast<double>(base) && unit + 1 < units.size())
    {
        value /= static_cast<double>(base);
        ++unit;
    }
    NumberOptions options;
    options.maximumFractionDigits = unit == 0 ? 0 : maximumFractionDigits;
    const std::string number = FormatNumber(value, locale, options);
    return number.empty() ? std::string{} :
        number + "\xE2\x80\xAF" + units[unit];
}

std::string FormatDuration(std::int64_t milliseconds,
    std::string_view locale, std::string_view style)
{
    if (milliseconds < 0) return {};
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    if (style == "clock")
    {
        std::ostringstream result;
        if (hours > 0)
            result << hours << ':' << std::setw(2) << std::setfill('0');
        result << (hours > 0 ? minutes : totalSeconds / 60) << ':'
            << std::setw(2) << std::setfill('0') << seconds;
        return result.str();
    }
    if (style != "short") return {};

    const DurationUnits units = UnitsFor(locale);
    std::vector<std::string> parts;
    const NumberOptions integers{ 0, 0, true };
    if (hours > 0)
        parts.push_back(FormatNumber(static_cast<double>(hours), locale,
            integers) + " " + units.hour);
    if (minutes > 0)
        parts.push_back(FormatNumber(static_cast<double>(minutes), locale,
            integers) + " " + units.minute);
    if (seconds > 0 || parts.empty())
        parts.push_back(FormatNumber(static_cast<double>(seconds), locale,
            integers) + " " + units.second);
    return Join(parts, " ");
}

std::string FormatRelativeTime(std::int64_t deltaMilliseconds,
    std::string_view locale, std::string_view unit,
    std::string_view numeric)
{
    if (numeric != "auto" && numeric != "always") return {};
    const std::uint64_t magnitude = Magnitude(deltaMilliseconds);
    RelativeUnit resolvedUnit{};
    if (unit == "auto")
        resolvedUnit = SelectRelativeUnit(magnitude);
    else if (!ParseRelativeUnit(unit, resolvedUnit))
        return {};

    const std::uint64_t count = RoundedUnits(
        magnitude, RelativeUnitMilliseconds(resolvedUnit));
    if (numeric == "auto")
    {
        const std::string special = RelativeSpecial(locale, resolvedUnit,
            deltaMilliseconds < 0 ? -1 : (deltaMilliseconds > 0 ? 1 : 0),
            count);
        if (!special.empty()) return special;
    }

    const std::string number = FormatNumber(static_cast<double>(count),
        locale, { 0, 0, true });
    const std::string unitName = RelativeUnitName(
        locale, resolvedUnit, count != 1);
    const std::string language = Language(locale);
    const bool future = deltaMilliseconds >= 0;
    if (language == "zh")
    {
        const bool traditional = locale.find("TW") != std::string_view::npos ||
            locale.find("tw") != std::string_view::npos;
        return number + unitName +
            (future ? (traditional ? "後" : "后") : "前"); // l10n-allow: relative-time suffix
    }
    if (language == "ja")
        return number + unitName + (future ? "後" : "前"); // l10n-allow: relative-time suffix
    if (language == "ko")
        return number + unitName + (future ? " 후" : " 전"); // l10n-allow: relative-time suffix
    const std::string value = number + " " + unitName;
    if (language == "de") return future ? "in " + value : "vor " + value;
    if (language == "fr")
        return future ? "dans " + value : "il y a " + value;
    if (language == "es")
        return future ? "dentro de " + value : "hace " + value;
    if (language == "pt") return future ? "em " + value : "há " + value;
    return future ? "in " + value : value + " ago";
}

std::string FormatList(const std::vector<std::string>& values,
    std::string_view locale)
{
    if (values.empty()) return {};
    if (values.size() == 1) return values.front();
    const std::string language = Language(locale);
    if (language == "ja") return Join(values, "、"); // l10n-allow: list separator

    std::string conjunction = "and";
    std::string separator = ", ";
    if (language == "zh")
    {
        conjunction = "和"; // l10n-allow: list conjunction
        separator = "、"; // l10n-allow: list separator
    }
    else if (language == "ko") conjunction = "및"; // l10n-allow: list conjunction
    else if (language == "de") conjunction = "und"; // l10n-allow: list conjunction
    else if (language == "fr") conjunction = "et"; // l10n-allow: list conjunction
    else if (language == "es") conjunction = "y"; // l10n-allow: list conjunction
    else if (language == "pt") conjunction = "e"; // l10n-allow: list conjunction

    if (values.size() == 2)
    {
        if (language == "zh")
            return values[0] + conjunction + values[1];
        return values[0] + " " + conjunction + " " + values[1];
    }
    std::vector<std::string> leading(values.begin(), values.end() - 1);
    if (language == "zh")
        return Join(leading, separator) + conjunction + values.back();
    const bool oxfordComma = language == "en";
    return Join(leading, separator) +
        (oxfordComma ? ", " : " ") + conjunction + " " + values.back();
}
}
