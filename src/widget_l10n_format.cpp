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
