#pragma once
#include "calendar_service.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

namespace snowdesktop
{
// Internal editor boundary. Both the native form and isolated regressions use
// these exact mutation rules; no second store or component API is introduced.
struct SystemCalendarEditorActions
{
    std::function<calendar::MutationResult(const calendar::CalendarEvent&)> save;
    std::function<std::optional<calendar::CalendarEvent>(const calendar::CalendarEvent&)> current;
    std::function<calendar::MutationResult(const std::string&)> remove;
    std::function<std::optional<calendar::CalendarSeries>(const std::string&)> seriesById;
    std::function<calendar::MutationResult(calendar::CalendarSeries)> saveSeries;
    std::function<calendar::MutationResult(const std::string&, int)> removeSeries;
};
inline constexpr std::array<int,7> SystemCalendarReminderMinutes{-1,0,5,15,30,60,1440};
// Pick an actual occurrence, not merely the rule's lower date bound. Keep an
// existing original date when returning from a moved occurrence to its series.
inline std::optional<std::string> CalendarEditorOccurrenceDate(
    const calendar::CalendarSeries& series, const std::string& preferred = {})
{
    using calendar::CalendarService;
    const auto& rule = series.rule;
    if (rule.kind != "dates" && (!CalendarService::GetDateInfo(rule.startDate) ||
        rule.interval < 1 || rule.interval > 99)) return {};
    if (rule.kind == "weekly" && (rule.weekdays.empty() ||
        std::any_of(rule.weekdays.begin(), rule.weekdays.end(), [](int day) { return day < 1 || day > 7; }))) return {};
    if (rule.kind == "monthly" && (rule.monthDay < 0 || rule.monthDay > 31)) return {};
    const auto available = [&](const std::string& date) {
        if (!CalendarService::MatchesRule(rule, date)) return false;
        const auto found = series.exceptions.find(date);
        return found == series.exceptions.end() || !found->second.canceled;
    };
    if (!preferred.empty() && available(preferred)) return preferred;
    if (rule.kind == "dates")
    {
        for (const auto& date : rule.dates) if (available(date)) return date;
        return {};
    }
    const auto start = CalendarService::GetDateInfo(rule.startDate);
    if (!start || rule.interval < 1 || rule.interval > 99) return {};
    if (rule.kind == "weekly")
    {
        // Monday-anchored intervals must agree with CalendarService::Matches.
        const int offset = (start->weekday + 5) % 7;
        for (int week = 0;; week += rule.interval)
        {
            for (int day = week == 0 ? offset : 0; day < 7; ++day)
            {
                const auto date = CalendarService::AddDays(rule.startDate, week * 7 + day - offset);
                if (!date || (!rule.endDate.empty() && *date > rule.endDate)) return {};
                if (available(*date)) return date;
            }
        }
    }
    if (rule.kind == "monthly")
    {
        for (int month = (start->year - 1) * 12 + start->month - 1;
             month < 9999 * 12; month += rule.interval)
        {
            char first[16]{};
            std::snprintf(first, sizeof(first), "%04d-%02d-01", month / 12 + 1, month % 12 + 1);
            const auto info = CalendarService::GetDateInfo(first);
            if (!info || (!rule.endDate.empty() && std::string(first) > rule.endDate)) return {};
            const int day = rule.monthDay == 0 ? info->daysInMonth : rule.monthDay;
            if (day < 1 || day > info->daysInMonth) continue;
            const auto date = CalendarService::AddDays(first, day - 1);
            if (date && available(*date)) return date;
        }
    }
    return {};
}
inline std::optional<int> ParseCalendarEditorTime(std::wstring_view value)
{
    if(value.size()!=5||value[2]!=L':')return {};
    for(const auto index:{0,1,3,4})if(value[index]<L'0'||value[index]>L'9')return {};
    const int hour=(value[0]-L'0')*10+value[1]-L'0',minute=(value[3]-L'0')*10+value[4]-L'0';
    if(hour>23||minute>59)return {};
    return hour*60+minute;
}
struct SystemCalendarEditorState
{
    calendar::CalendarEvent original, draft;
    SystemCalendarEditorActions actions;
    std::function<bool()> valid;
    std::string error;
    bool committed=false, deleted=false;

    bool Valid() const {return !valid||valid();}
    bool Save(calendar::CalendarEvent candidate)
    {
        candidate.id=original.id;candidate.revision=original.revision;
        draft=std::move(candidate);error.clear();committed=deleted=false;
        if(!Valid()){error="canceled";return false;}
        if(!actions.save){error="unavailable";return false;}
        const auto result=actions.save(draft);
        if(!result.ok){error=result.error;return false;}
        draft.id=result.id;draft.revision=result.revision;committed=true;return true;
    }
    bool Remove()
    {
        error.clear();committed=deleted=false;
        if(!Valid()){error="canceled";return false;}
        if(original.id.empty()||!actions.current||!actions.remove){error="unavailable";return false;}
        const auto current=actions.current(original);
        if(!current){error="not_found";return false;}
        if(current->id!=original.id||current->revision!=original.revision){error="conflict";return false;}
        // A callback may reenter the host. Revocation between lookup and write
        // must not delete the old model's event after its popup was closed.
        if(!Valid()){error="canceled";return false;}
        const auto result=actions.remove(original.id);
        if(!result.ok){error=result.error;return false;}
        committed=deleted=true;return true;
    }
};
}
