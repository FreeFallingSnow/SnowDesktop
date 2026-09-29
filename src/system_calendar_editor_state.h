#pragma once
#include "calendar_service.h"
#include <array>
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
