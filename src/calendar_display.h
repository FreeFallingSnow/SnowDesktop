#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::calendar
{
struct DisplayPreferences
{
    bool enabled = false;
    std::string calendar = "chinese";
    bool holidaysEnabled = false;
    std::string region = "CN";
    friend bool operator==(const DisplayPreferences&, const DisplayPreferences&) = default;
};
struct DisplayOption { std::string id; std::wstring label; };
struct DayAnnotation
{
    std::string date;
    std::string secondary;
    std::string fullDate;
    int year = 0, month = 0, day = 0, era = 0;
    bool leapMonth = false;
    bool calendarAvailable = false;
    bool holidaysAvailable = false;
    std::vector<std::string> holidays;
};
std::vector<DisplayOption> CalendarOptions(std::string_view language);
std::vector<DisplayOption> HolidayRegions(std::string_view language);
inline constexpr std::array CalendarIds = {"chinese", "dangi", "islamic-civil", "islamic-umalqura", "hebrew", "persian", "indian", "coptic", "ethiopic", "japanese", "buddhist", "roc"};
inline constexpr std::array HolidayRegionIds = {"CN", "TW", "HK", "JP", "KR", "US", "GB", "DE", "FR", "ES", "MX", "BR", "TH", "IL", "IR", "SA", "IN"};
inline void Normalize(DisplayPreferences& p)
{
    if (std::find(CalendarIds.begin(), CalendarIds.end(), p.calendar) == CalendarIds.end())
    { p.calendar = "chinese"; p.enabled = false; }
    if (std::find(HolidayRegionIds.begin(), HolidayRegionIds.end(), p.region) == HolidayRegionIds.end())
    { p.region = "CN"; p.holidaysEnabled = false; }
}
// Civil dates are converted at UTC noon, independently of the machine zone.
// Returns empty for invalid/reversed ranges or more than 62 days.
std::vector<DayAnnotation> Annotate(const std::string& from, const std::string& to,
    const DisplayPreferences& preferences, std::string_view language);
}
