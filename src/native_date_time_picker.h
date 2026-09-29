#pragma once
#include "calendar_service.h"
#include "native_ui_scene.h"
#include <d2d1helper.h>
#include "l10n.h"
#include <algorithm>
#include <array>
#include <cmath>
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
    {return kind_==Kind::Date?"picker.day:"+date_:TimeTarget(activeMinutes_);}
    std::string TimeTabTarget(std::string_view focused,bool shift) const
    {
        if(kind_!=Kind::Time)return {};
        const std::array<std::string,5> order{"picker.back",TimeTarget(false),TimeTarget(true),"picker.cancel","picker.confirm"};
        const auto part=TimePart(focused);const auto current=part?TimeTarget(*part):std::string(focused);
        const auto found=std::find(order.begin(),order.end(),current);
        const auto index=found==order.end()?(shift?order.size()-1:0):
            (static_cast<std::size_t>(found-order.begin())+(shift?order.size()-1:1))%order.size();
        return order[index];
    }
    bool TimeKey(std::string_view focused,unsigned key)
    {
        if(kind_!=Kind::Time)return false;const auto part=TimePart(focused);if(!part)return false;
        activeMinutes_=*part;
        if(key==VK_LEFT||key==VK_RIGHT){activeMinutes_=key==VK_RIGHT;return true;}
        if(key==VK_HOME||key==VK_END){SetPart(*part,key==VK_HOME?0:(*part?59:23));return true;}
        if(key!=VK_UP&&key!=VK_DOWN&&key!=VK_PRIOR&&key!=VK_NEXT)return false;
        const int step=key==VK_PRIOR?-5:key==VK_NEXT?5:key==VK_UP?-1:1;
        SetPart(*part,PartValue(*part)+step);return true;
    }
    bool TimeWheel(std::string_view target,float notches)
    {
        if(kind_!=Kind::Time||!std::isfinite(notches))return false;
        const auto part=TimePart(target);if(!part)return false;
        activeMinutes_=*part;auto& remainder=wheel_[*part?1:0];remainder+=std::clamp(notches,-120.f,120.f);
        const int steps=static_cast<int>(remainder);remainder-=static_cast<float>(steps);
        if(steps)SetPart(*part,PartValue(*part)-steps);return true;
    }
    Scene Build(float width,std::wstring title,const std::string& today) const
    {
        Scene scene;scene.width=width;
        const auto rect=[](float x,float y,float w,float h){return D2D1::RectF(x,y,x+w,y+h);};
        const auto add=[&](std::string id,Role role,D2D1_RECT_F bounds,std::wstring text={}) -> Node& {
            Node n;n.id=std::move(id);n.role=role;n.bounds=n.clip=bounds;n.text=std::move(text);
            scene.nodes.push_back(std::move(n));return scene.nodes.back();
        };
        auto& back=add("picker.back",Role::Icon,rect(16,8,32,32));back.glyph=L"\uE76B";back.tooltip=_LW("settings.shell.back");
        auto& heading=add("picker.heading",Role::Text,rect(52,8,width-68,32),std::move(title));heading.fontSize=17;heading.bold=true;
        const float content=(std::min)(408.f,width-32),left=(width-content)/2;
        float bottom=0;
        if(kind_==Kind::Date)
        {
            const auto month=calendar::CalendarService::GetDateInfo(month_);
            const bool narrow=content<280;const float controls=narrow?72.f:44.f,grid=controls+38.f;
            auto& label=add("picker.month",Role::Text,rect(left,44,narrow?content:content-150,28),
                std::to_wstring(month->year)+L" / "+std::to_wstring(month->month));label.bold=true;label.fontSize=16;
            auto& now=add("picker.today",Role::Button,rect(left+content-144,controls,68,30),_LW("app.widget.date_picker.today"));now.centered=true;now.fontSize=12;now.enabled=calendar::CalendarService::GetDateInfo(today).has_value();
            for(int i=0;i<2;++i)
            {
                auto& arrow=add(i?"picker.next":"picker.previous",Role::Icon,rect(left+content-70+i*36.f,controls,32,30));
                arrow.glyph=i?L"\uE76C":L"\uE76B";arrow.tooltip=_LW(i?"app.widget.date_picker.next":"app.widget.date_picker.previous");
                arrow.enabled=i?month->year<9999||month->month<12:month->year>1||month->month>1;
            }
            const float cell=content/7;
            for(int i=0;i<7;++i)
            {
                const auto key="app.widget.date_picker.weekday"+std::to_string((i+1)%7+1);
                auto& day=add("picker.weekday:"+std::to_string(i),Role::Text,rect(left+i*cell,grid,cell-4,20),_LW(key.c_str()));
                day.centered=day.secondary=true;day.fontSize=12;
            }
            const auto dates=MonthDates(month_);
            for(int i=0;i<42;++i)if(dates[i])
            {
                const auto info=calendar::CalendarService::GetDateInfo(*dates[i]);
                const float stride=narrow?28.f:30.f;
                auto& day=add("picker.day:"+*dates[i],Role::ListItem,rect(left+(i%7)*cell,grid+24+(i/7)*stride,cell-4,stride-2),std::to_wstring(info->day));
                day.centered=true;day.selected=day.accent=*dates[i]==date_;day.outlined=*dates[i]==today;day.secondary=info->month!=month->month;day.fontSize=13;
                day.accessibilityLabel=day.tooltip=std::wstring(dates[i]->begin(),dates[i]->end());
            }
            bottom=grid+24+6*(narrow?28.f:30.f)+6;
        }
        else
        {
            auto& value=add("picker.time",Role::Text,rect(left,50,content,40),TimeText(minutes_));value.centered=value.bold=true;value.fontSize=24;
            const float column=(content-12)/2;
            for(int part=0;part<2;++part)
            {
                const bool minute=part!=0;const float x=left+part*(column+12);const std::string prefix=minute?"picker.minute:":"picker.hour:";
                const auto label=_LW(minute?"app.widget.time_picker.minute":"app.widget.time_picker.hour");
                auto& columnLabel=add(prefix+"label",Role::Text,rect(x,104,column,26),label);columnLabel.centered=columnLabel.secondary=true;
                add(prefix+"column",Role::Card,rect(x,134,column,166));
                const int selected=PartValue(minute);
                for(int offset=-2;offset<=2;++offset)
                {
                    const int choice=selected+offset;if(choice<0||choice>(minute?59:23))continue;
                    const auto text=choice<10?L"0"+std::to_wstring(choice):std::to_wstring(choice);
                    auto& valueNode=add(prefix+(offset==0?std::string("current"):std::to_string(choice)),Role::ListItem,rect(x+4,136+(offset+2)*32.f,column-8,30),text);
                    valueNode.centered=true;valueNode.selected=valueNode.accent=offset==0;valueNode.secondary=offset!=0;
                    valueNode.fontSize=offset==0?22.f:17.f;valueNode.accessibilityLabel=std::wstring(label)+L" "+text;
                }
            }
            bottom=308;
        }
        add("picker.cancel",Role::Button,rect(left,bottom,(content-12)/2,36),_LW("settings.dialog.cancel")).centered=true;
        auto& confirm=add("picker.confirm",Role::Button,rect(left+(content+12)/2,bottom,(content-12)/2,36),_LW("app.widget.date_picker.confirm"));confirm.centered=confirm.accent=true;
        scene.height=bottom+40;return scene;
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
        const bool hour=id.starts_with("picker.hour:"),minute=id.starts_with("picker.minute:");if(!hour&&!minute)return Result::None;
        const auto digits=id.substr(hour?12:14);if(digits=="current"){activeMinutes_=minute;return Result::Changed;}
        if(digits.empty()||digits.size()>2)return Result::None;
        int value=0;for(const auto digit:digits){if(digit<'0'||digit>'9')return Result::None;value=value*10+digit-'0';}
        if(value>=(hour?24:60))return Result::None;
        SetPart(minute,value);return Result::Changed;
    }
private:
    Kind kind_;
    std::string date_,month_;
    int minutes_=0;
    bool activeMinutes_=false;
    std::array<float,2> wheel_{};
    static std::optional<bool> TimePart(std::string_view id)
    {if(id.starts_with("picker.hour:"))return false;if(id.starts_with("picker.minute:"))return true;return {};}
    int PartValue(bool minute)const{return minute?minutes_%60:minutes_/60;}
    std::string TimeTarget(bool minute)const
    {return minute?"picker.minute:current":"picker.hour:current";}
    void SetPart(bool minute,int value)
    {
        activeMinutes_=minute;value=std::clamp(value,0,minute?59:23);
        minutes_=minute?(minutes_/60)*60+value:value*60+minutes_%60;
    }
};
}
