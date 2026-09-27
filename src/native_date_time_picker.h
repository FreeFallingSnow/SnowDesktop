#pragma once
#include "calendar_service.h"
#include "native_ui_scene.h"
#include <d2d1helper.h>
#include "l10n.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace snowdesktop::native_ui
{
// Internal scene controls, shared by native form hosts. Selection is a draft;
// only Confirm returns a value to the containing form. They create no HWND.
class DateTimePicker
{
public:
    enum class Kind { Date, Time };
    enum class Result { None, Changed, Confirmed, Cancelled };
    explicit DateTimePicker(std::string date):kind_(Kind::Date),date_(std::move(date))
    {if(!calendar::CalendarService::GetDateInfo(date_))date_="2000-01-01";month_=date_.substr(0,7)+"-01";}
    explicit DateTimePicker(int minutes):kind_(Kind::Time),minutes_((std::clamp)(minutes,0,1439)){}
    Kind Type() const{return kind_;}
    const std::string& Date() const{return date_;}
    int Minutes() const{return minutes_;}
    static std::wstring TimeText(int minutes)
    {wchar_t value[16]{};swprintf_s(value,L"%02d:%02d",minutes/60,minutes%60);return value;}
    static std::array<std::optional<std::string>,42> MonthDates(const std::string& month)
    {
        std::array<std::optional<std::string>,42> result;
        if(const auto info=calendar::CalendarService::GetDateInfo(month))
            for(int i=0;i<42;++i)result[i]=calendar::CalendarService::AddDays(month,i-(info->weekday+5)%7);
        return result;
    }
    std::string FocusTarget() const
    {return kind_==Kind::Date?"picker.day:"+date_:std::string(minutesOpen_?"picker.minute:":"picker.hour:")+std::to_string(minutesOpen_?minutes_%60:minutes_/60);}
    Scene Build(float width,std::wstring title,const std::string& today) const
    {
        Scene scene;scene.width=width;
        const auto rect=[](float x,float y,float w,float h){return D2D1::RectF(x,y,x+w,y+h);};
        const auto add=[&](std::string id,Role role,D2D1_RECT_F bounds,std::wstring text={}) -> Node& {
            Node n;n.id=std::move(id);n.role=role;n.bounds=n.clip=bounds;n.text=std::move(text);
            scene.nodes.push_back(std::move(n));return scene.nodes.back();
        };
        auto& back=add("picker.back",Role::Icon,rect(12,10,36,36));back.glyph=L"\uE76B";back.tooltip=_LW("settings.shell.back");
        auto& heading=add("picker.heading",Role::Text,rect(56,10,width-72,36),std::move(title));heading.fontSize=17;heading.bold=true;
        const float content=(std::min)(408.f,width-32),left=(width-content)/2;
        float bottom=0;
        if(kind_==Kind::Date)
        {
            const auto month=calendar::CalendarService::GetDateInfo(month_);
            const bool compact=content<280;const float controls=compact?104.f:64.f,grid=controls+44;
            auto& label=add("picker.month",Role::Text,rect(left,64,compact?content:content-150,32),
                std::to_wstring(month->year)+L" / "+std::to_wstring(month->month));label.bold=true;label.fontSize=16;
            auto& now=add("picker.today",Role::Button,rect(left+content-144,controls,68,32),_LW("app.widget.date_picker.today"));now.fontSize=12;now.enabled=calendar::CalendarService::GetDateInfo(today).has_value();
            for(int i=0;i<2;++i)
            {
                auto& arrow=add(i?"picker.next":"picker.previous",Role::Icon,rect(left+content-70+i*36.f,controls,32,32));
                arrow.glyph=i?L"\uE76C":L"\uE76B";arrow.tooltip=_LW(i?"app.widget.date_picker.next":"app.widget.date_picker.previous");
                arrow.enabled=i?month->year<9999||month->month<12:month->year>1||month->month>1;
            }
            const float cell=content/7;
            for(int i=0;i<7;++i)
            {
                const auto key="app.widget.date_picker.weekday"+std::to_string((i+1)%7+1);
                auto& day=add("picker.weekday:"+std::to_string(i),Role::Text,rect(left+i*cell,grid,cell-4,24),_LW(key.c_str()));
                day.centered=day.secondary=true;day.fontSize=12;
            }
            const auto dates=MonthDates(month_);
            for(int i=0;i<42;++i)if(dates[i])
            {
                const auto info=calendar::CalendarService::GetDateInfo(*dates[i]);
                auto& day=add("picker.day:"+*dates[i],Role::ListItem,rect(left+(i%7)*cell,grid+28+(i/7)*36.f,cell-4,32),std::to_wstring(info->day));
                day.centered=true;day.selected=day.accent=*dates[i]==date_;day.outlined=*dates[i]==today;day.secondary=info->month!=month->month;day.fontSize=13;
                day.accessibilityLabel=day.tooltip=std::wstring(dates[i]->begin(),dates[i]->end());
            }
            bottom=grid+28+6*36+12;
        }
        else
        {
            auto& value=add("picker.time",Role::Text,rect(left,64,content,44),TimeText(minutes_));value.centered=value.bold=true;value.fontSize=24;
            for(int part=0;part<2;++part)
            {
                auto& tab=add(part?"picker.minutes":"picker.hours",Role::Button,rect(left+part*(content+12)/2,120,(content-12)/2,36),_LW(part?"app.widget.time_picker.minute":"app.widget.time_picker.hour"));
                tab.selected=tab.accent=(part!=0)==minutesOpen_;
            }
            const int count=minutesOpen_?60:24;const float cell=content/6;
            for(int i=0;i<count;++i)
            {
                const auto label=i<10?L"0"+std::to_wstring(i):std::to_wstring(i);
                auto& valueNode=add(std::string(minutesOpen_?"picker.minute:":"picker.hour:")+std::to_string(i),Role::Button,
                    rect(left+(i%6)*cell,168+(i/6)*36.f,cell-4,32),label);
                valueNode.centered=true;valueNode.selected=valueNode.accent=i==(minutesOpen_?minutes_%60:minutes_/60);
                valueNode.accessibilityLabel=std::wstring(_LW(minutesOpen_?"app.widget.time_picker.minute":"app.widget.time_picker.hour"))+L" "+label;
            }
            bottom=168+((count+5)/6)*36.f+12;
        }
        add("picker.cancel",Role::Button,rect(left,bottom,(content-12)/2,36),_LW("settings.dialog.cancel"));
        add("picker.confirm",Role::Button,rect(left+(content+12)/2,bottom,(content-12)/2,36),_LW("app.widget.date_picker.confirm")).accent=true;
        scene.height=bottom+44;return scene;
    }
    Result Invoke(std::string_view id,const std::string& today)
    {
        if(id=="picker.back"||id=="picker.cancel")return Result::Cancelled;
        if(id=="picker.confirm")return Result::Confirmed;
        if(kind_==Kind::Date)
        {
            if(id=="picker.previous"||id=="picker.next")
            {
                const auto info=calendar::CalendarService::GetDateInfo(month_);int year=info->year,month=info->month+(id=="picker.next"?1:-1);
                if(month<1){month=12;--year;}if(month>12){month=1;++year;}if(year<1||year>9999)return Result::None;
                char text[16]{};sprintf_s(text,"%04d-%02d-01",year,month);month_=text;return Result::Changed;
            }
            const auto value=id=="picker.today"?today:id.starts_with("picker.day:")?std::string(id.substr(11)):std::string{};
            if(!calendar::CalendarService::GetDateInfo(value))return Result::None;
            date_=value;month_=date_.substr(0,7)+"-01";return Result::Changed;
        }
        if(id=="picker.hours"||id=="picker.minutes"){minutesOpen_=id=="picker.minutes";return Result::Changed;}
        const bool hour=id.starts_with("picker.hour:"),minute=id.starts_with("picker.minute:");if(!hour&&!minute)return Result::None;
        const auto digits=id.substr(hour?12:14);if(digits.empty()||digits.size()>2)return Result::None;
        int value=0;for(const auto digit:digits){if(digit<'0'||digit>'9')return Result::None;value=value*10+digit-'0';}
        if(value>=(hour?24:60))return Result::None;
        minutes_=hour?value*60+minutes_%60:(minutes_/60)*60+value;minutesOpen_=true;return Result::Changed;
    }
private:
    Kind kind_;
    std::string date_,month_;
    int minutes_=0;
    bool minutesOpen_=false;
};
}
