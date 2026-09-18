#include "calendar_display.h"
#include "calendar_service.h"
#include <windows.h>
#include <icu.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <set>

#pragma comment(lib, "icu.lib")

namespace snowdesktop::calendar
{
namespace
{
constexpr auto& calendars = CalendarIds;
constexpr auto& regions = HolidayRegionIds;
struct HolidayEntry { std::string_view region, language, date, name; };
#include "calendar_holidays.inc"

std::string LocaleId(std::string_view language)
{
    std::string result(language);
    std::replace(result.begin(), result.end(), '-', '_');
    return result;
}
std::string Utf8(const UChar* value, int length)
{
    if (length <= 0) return {};
    const auto* wide = reinterpret_cast<const wchar_t*>(value);
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, result.data(), count, nullptr, nullptr);
    return result;
}
using CalendarPtr = std::unique_ptr<UCalendar, decltype(&ucal_close)>;
using FormatPtr = std::unique_ptr<UDateFormat, decltype(&udat_close)>;
std::string Format(UDateFormat* format, UDate date)
{
    if (!format) return {};
    UErrorCode status = U_ZERO_ERROR;
    UChar text[256]{};
    const int count = udat_format(format, date, text, 256, nullptr, &status);
    return U_SUCCESS(status) ? Utf8(text, count) : std::string{};
}
}

std::vector<DisplayOption> CalendarOptions(std::string_view language)
{
    std::vector<DisplayOption> result;
    const auto locale = LocaleId(language);
    for (auto id : calendars)
    {
        UErrorCode status = U_ZERO_ERROR;
        const std::string source = "en@calendar=" + std::string(id);
        UChar label[128]{};
        const auto count = uloc_getDisplayKeywordValue(source.c_str(), "calendar", locale.c_str(), label, 128, &status);
        if (U_SUCCESS(status)) result.push_back({id, std::wstring(reinterpret_cast<wchar_t*>(label), count)});
    }
    return result;
}
std::vector<DisplayOption> HolidayRegions(std::string_view language)
{
    // Product region labels are independent of ICU's country naming conventions.
    struct Names { std::string_view language; const wchar_t *mainland, *hongKong, *taiwan; };
    static constexpr Names names[] = {
        {"zh-CN", L"中国大陆", L"香港特别行政区", L"台湾"}, // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
        {"zh-TW", L"中國大陸", L"香港特別行政區", L"台灣"}, // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
        {"en-US", L"Mainland China", L"Hong Kong Special Administrative Region", L"Taiwan"},
        {"ja-JP", L"中国本土", L"香港特別行政区", L"台湾"}, // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
        {"ko-KR", L"중국 본토", L"홍콩 특별행정구", L"대만"},
        {"de-DE", L"Festlandchina", L"Sonderverwaltungsregion Hongkong", L"Taiwan"},
        {"fr-FR", L"Chine continentale", L"Région administrative spéciale de Hong Kong", L"Taïwan"},
        {"es-ES", L"China continental", L"Región Administrativa Especial de Hong Kong", L"Taiwán"},
        {"es-419", L"China continental", L"Región Administrativa Especial de Hong Kong", L"Taiwán"},
        {"pt-BR", L"China continental", L"Região Administrativa Especial de Hong Kong", L"Taiwan"},
    };
    const Names* selected = &names[2];
    for (const auto& entry : names) if (entry.language == language) selected = &entry;
    std::vector<DisplayOption> result;
    const auto locale = LocaleId(language);
    for (auto id : regions)
    {
        if (std::string_view(id) == "CN") { result.push_back({id, selected->mainland}); continue; }
        if (std::string_view(id) == "HK") { result.push_back({id, selected->hongKong}); continue; }
        if (std::string_view(id) == "TW") { result.push_back({id, selected->taiwan}); continue; }
        UErrorCode status = U_ZERO_ERROR;
        UChar label[128]{};
        const std::string source = "und_" + std::string(id);
        const auto count = uloc_getDisplayCountry(source.c_str(), locale.c_str(), label, 128, &status);
        if (U_SUCCESS(status)) result.push_back({id, std::wstring(reinterpret_cast<wchar_t*>(label), count)});
    }
    return result;
}
std::vector<DayAnnotation> Annotate(const std::string& from, const std::string& to,
    const DisplayPreferences& requested, std::string_view language)
{
    const auto first = CalendarService::GetDateInfo(from);
    if (!first || !CalendarService::GetDateInfo(to) || to < from) return {};
    const auto limit = CalendarService::AddDays(from, 61);
    if (limit && to > *limit) return {};
    auto p = requested;
    Normalize(p);
    const auto languageId = LocaleId(language);
    const auto locale = languageId + "@calendar=" + p.calendar;
    UErrorCode status = U_ZERO_ERROR;
    CalendarPtr calendar(p.enabled ? ucal_open(u"UTC", -1, locale.c_str(), UCAL_DEFAULT, &status) : nullptr, ucal_close);
    const bool usable = calendar && U_SUCCESS(status) && p.calendar == ucal_getType(calendar.get(), &status);
    const bool yearOnly = p.calendar == "japanese" || p.calendar == "buddhist" || p.calendar == "roc";
    status = U_ZERO_ERROR;
    FormatPtr shortFormat(usable ? udat_open(UDAT_PATTERN, UDAT_PATTERN, locale.c_str(), u"UTC", -1,
        yearOnly ? u"Gy" : u"MMMd", -1, &status) : nullptr, udat_close);
    status = U_ZERO_ERROR;
    FormatPtr fullFormat(usable ? udat_open(UDAT_NONE, UDAT_FULL, locale.c_str(), u"UTC", -1,
        nullptr, 0, &status) : nullptr, udat_close);
    // Choose one available language for the whole region, never per holiday.
    const auto start = std::lower_bound(std::begin(kHolidayEntries), std::end(kHolidayEntries), p.region,
        [](const HolidayEntry& entry, const std::string& region) { return entry.region < region; });
    const auto end = std::find_if(start, std::end(kHolidayEntries), [&](const auto& entry) { return entry.region != p.region; });
    std::set<std::string_view> available;
    for (auto it = start; it != end; ++it) available.insert(it->language);
    std::string holidayLanguage = languageId;
    if (!available.contains(holidayLanguage)) holidayLanguage = languageId.substr(0, languageId.find('_'));
    if (!available.contains(holidayLanguage)) holidayLanguage = "en_US";
    if (!available.contains(holidayLanguage) && !available.empty()) holidayLanguage = *available.begin();

    std::vector<DayAnnotation> result;
    for (std::string date = from; date <= to;)
    {
        DayAnnotation item;
        item.date = date;
        const auto info = *CalendarService::GetDateInfo(date);
        if (usable)
        {
            using namespace std::chrono;
            const auto days = sys_days{year{info.year}/month{static_cast<unsigned>(info.month)}/day{static_cast<unsigned>(info.day)}};
            const UDate instant = static_cast<double>(duration_cast<milliseconds>(days.time_since_epoch()).count()) + 43200000.0;
            status = U_ZERO_ERROR;
            ucal_setMillis(calendar.get(), instant, &status);
            item.year = ucal_get(calendar.get(), UCAL_EXTENDED_YEAR, &status);
            item.month = ucal_get(calendar.get(), UCAL_MONTH, &status) + 1;
            item.day = ucal_get(calendar.get(), UCAL_DATE, &status);
            item.era = ucal_get(calendar.get(), UCAL_ERA, &status);
            item.leapMonth = ucal_get(calendar.get(), UCAL_IS_LEAP_MONTH, &status) != 0;
            item.secondary = Format(shortFormat.get(), instant);
            if (p.calendar == "chinese")
            {
                static constexpr const char* lunarDays[] = {"", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十", "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十", "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"}; // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
                static constexpr const char* months[] = {"", "正月", "二月", "三月", "四月", "五月", "六月", "七月", "八月", "九月", "十月", "冬月", "腊月"}; // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
                const bool traditional = language == "zh-TW" || language == "zh-HK";
                if (item.day >= 1 && item.day <= 30 && item.month >= 1 && item.month <= 12)
                    item.secondary = item.day == 1
                        ? std::string(item.leapMonth ? (traditional ? "閏" : "闰") : "") + // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
                            (traditional && item.month == 12 ? "臘月" : months[item.month]) // l10n-allow: locale-specific region table or intrinsic Chinese lunar notation
                        : lunarDays[item.day];
            }
            item.fullDate = Format(fullFormat.get(), instant);
            item.calendarAvailable = U_SUCCESS(status) && !item.secondary.empty();
        }
        item.holidaysAvailable = p.holidaysEnabled && info.year >= 2020 && info.year <= 2035;
        if (item.holidaysAvailable)
            for (auto it = start; it != end; ++it)
                if (it->language == holidayLanguage && it->date == date) item.holidays.emplace_back(it->name);
        result.push_back(std::move(item));
        const auto next = CalendarService::AddDays(date, 1);
        if (!next) break;
        date = *next;
    }
    return result;
}
}
