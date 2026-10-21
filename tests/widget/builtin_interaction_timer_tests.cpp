#include "system/calendar/calendar_service.h"
#include "widget/runtime/widget_lua_lifecycle.h"
#include "widget/runtime/widget_calendar_provider.h"
#include "widget/runtime/widget_timer_dispatch.h"
#include "widget/view/widget_interaction_region.h"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace snowdesktop::widget_runtime;
using Calendar = snowdesktop::calendar::CalendarService;
using Schedule = NamedTimerSchedule;
namespace fs=std::filesystem;
void Require(bool condition,const std::string& message)
{
    if(!condition)throw std::runtime_error(message);
}
struct Directory {
    fs::path path;
    Directory()
    {
        static std::atomic<unsigned> sequence=0;
        path=fs::temp_directory_path()/(L"SnowDesktop-builtin-regression-"+
            std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+
            L"-"+std::to_wstring(sequence++));
        Require(fs::create_directory(path),"create owned component test directory");
    }
    ~Directory()
    {
        std::error_code error;
        const auto resolved=fs::weakly_canonical(path,error);
        const auto expectedParent=fs::weakly_canonical(fs::temp_directory_path(),error);
        if(!error && resolved.parent_path()==expectedParent &&
            resolved.filename().wstring().starts_with(L"SnowDesktop-builtin-regression-"))
            fs::remove_all(resolved,error);
    }
};
struct TimerOwner {
    std::wstring widgetId=L"instance";
    std::uint64_t runtimeToken=0;
    bool preview=false;
    NamedTimerSchedule namedTimers;
};

// Host services are boundaries: data comes from the real CalendarService;
// schedule calls mutate the real timer model through the production dispatcher.
// Shipped Lua setup/event/render code and native interaction hit testing execute.
class Vm {
public:
    Vm(Calendar& calendar,std::vector<TimerOwner>& owners,std::uint64_t token)
        : calendar_(calendar),owners_(owners),token_(token)
    {
        state_=luaL_newstate(); Require(state_!=nullptr,"create component Lua VM");
        luaL_openlibs(state_);
        lua_pushinteger(state_,static_cast<lua_Integer>(token));
        lua_setfield(state_,LUA_REGISTRYINDEX,"__widget_runtime_token");
        Bind("_now",Now); Bind("_parts",Parts); Bind("_dateInfo",DateInfo);
        Bind("_addDays",AddDays); Bind("_selectDate",SelectDate);
        Bind("_storageGet",StorageGet); Bind("_storageSet",StorageSet);
        Bind("_dataValue",DataValue); Bind("_subscribe",Subscribe);
        Bind("_region",Region); Bind("_scroll",Scroll); Bind("_drawText",DrawText);
        Bind("_every",Every); Bind("_after",After); Bind("_at",At);
        Bind("_timeline",Timeline); Bind("_cancel",Cancel);
        Run(R"lua(
widget={define=function(d)return d end,setTitle=function()end,invalidate=function()end,
    theme=function()return {contentTheme=0}end,hasFeature=function(name)
        return name=="calendar.annotations" end,hasPermission=function()return true end,
    log=function()end}
l10n={language=function()return "en-US"end,tr=function(key,...)
    local result=key;for _,v in ipairs({...})do result=result.."|"..tostring(v)end;return result end}
ui={metrics=function()return {layoutRowHeight=28}end}
layout={contentWidth=function()return _width end,contentHeight=function()return _height end}
time={now=_now,parts=_parts,monotonic=function()return 0 end}
calendar={dateInfo=_dateInfo,addDays=_addDays,selectDate=_selectDate,
    preferences=function()return {enabled=true,calendar="gregorian",weekStart=1}end,
    annotations=function(first,last) _annotationRange={first,last};return {}end}
storage={get=_storageGet,set=_storageSet,remove=function(key)return _storageSet(key,nil)end}
resource={audio=function(name)return {name=name}end}
data={subscribe=_subscribe}
schedule={every=_every,after=_after,at=_at,timeline=_timeline,cancel=_cancel}
interaction={region=_region,scroll=_scroll,setScrollOffset=function()end,
    isHovered=function()return false end,isPressed=function()return false end}
draw=setmetatable({text=_drawText,measureText=function(text,size)
    return {width=#tostring(text)*size/2,height=size}end},{__index=function()return function()end end})
control={textInput=function()return ""end}
task={start=function()return 1 end,cancel=function()return true end}
_width=420;_height=280
)lua");
    }
    ~Vm(){if(state_)lua_close(state_);}
    Vm(const Vm&)=delete;Vm& operator=(const Vm&)=delete;
    void Load(const fs::path& entry)
    {
        Require(luaL_loadfile(state_,entry.string().c_str())==LUA_OK,"load shipped entry "+entry.string());
        Call(0,1);lua_setglobal(state_,"descriptor");
        Run("model=descriptor.setup({preview=false})");
    }
    void Run(const std::string& source)
    {
        if(luaL_dostring(state_,source.c_str())!=LUA_OK)FailLua();
    }
    void Render(int width,int height)
    {
        lua_pushinteger(state_,width);lua_setglobal(state_,"_width");
        lua_pushinteger(state_,height);lua_setglobal(state_,"_height");
        drawn.clear();submitted.clear();regions.BeginFrame();
        Run("descriptor.render({preview=false,selected=true},model)");
        regions.CommitFrame();
    }
    void Action(const char* id)
    {
        lua_getglobal(state_,"descriptor");lua_getfield(state_,-1,"event");lua_remove(state_,-2);
        lua_newtable(state_);lua_getglobal(state_,"model");lua_newtable(state_);
        lua_pushliteral(state_,"action");lua_setfield(state_,-2,"kind");
        lua_pushstring(state_,id);lua_setfield(state_,-2,"id");Call(3,0);
    }
    void FireDue(std::int64_t steadyMs,std::int64_t wallMs)
    {
        steady=steadyMs;wall=wallMs;
        auto& timers=CurrentOwner().namedTimers;
        const auto now=Schedule::TimePoint{std::chrono::milliseconds(steady)};
        const auto wallNow=Schedule::WallTimePoint{std::chrono::milliseconds(wall)};
        for(const auto& name:timers.DueNames(now,wallNow))
        {
            if(!timers.ConsumeDueInfo(name,now,wallNow))continue;
            lua_getglobal(state_,"descriptor");lua_getfield(state_,-1,"event");lua_remove(state_,-2);
            lua_newtable(state_);lua_getglobal(state_,"model");lua_newtable(state_);
            lua_pushliteral(state_,"schedule");lua_setfield(state_,-2,"kind");
            lua_pushlstring(state_,name.data(),name.size());lua_setfield(state_,-2,"id");Call(3,0);
        }
    }
    lua_State* State(){return state_;}
    void Call(int arguments,int results)
    {
        if(lua_pcall(state_,arguments,results,0)!=LUA_OK)FailLua();
    }
    std::unordered_map<std::string,std::string> storage;
    std::vector<InteractionRegion> submitted;
    WidgetInteractionRegions regions;
    InteractionShape viewport;
    std::vector<std::string> drawn;
    double scrollOffset=13;
    std::int64_t steady=0,wall=1'000'000;
    bool invalidRange=false;
private:
    static Vm& Self(lua_State* state){return *static_cast<Vm*>(lua_touserdata(state,lua_upvalueindex(1)));}
    void Bind(const char* name,lua_CFunction function)
    {lua_pushlightuserdata(state_,this);lua_pushcclosure(state_,function,1);lua_setglobal(state_,name);}
    [[noreturn]]void FailLua()
    {
        const char* text=lua_tostring(state_,-1);std::string error=text?text:"unknown Lua error";
        lua_pop(state_,1);throw std::runtime_error(error);
    }
    TimerOwner& CurrentOwner()
    {
        const auto owner=std::find_if(owners_.begin(),owners_.end(),[&](const auto& value){return value.runtimeToken==token_;});
        Require(owner!=owners_.end(),"VM timer owner exists");return *owner;
    }
    static void Text(lua_State* state,const char* key,const std::string& value)
    {lua_pushlstring(state,value.data(),value.size());lua_setfield(state,-2,key);}
    static void Number(lua_State* state,const char* key,int value)
    {lua_pushinteger(state,value);lua_setfield(state,-2,key);}
    static int Now(lua_State* state){lua_pushinteger(state,Self(state).wall);return 1;}
    static int Parts(lua_State* state)
    {
        lua_newtable(state);Number(state,"year",2026);Number(state,"month",10);Number(state,"day",15);
        Number(state,"hour",0);Number(state,"min",0);Number(state,"sec",0);return 1;
    }
    static int DateInfo(lua_State* state)
    {
        const char* date=lua_tostring(state,1);const auto value=date?Calendar::GetDateInfo(date):std::nullopt;
        if(!value){lua_pushnil(state);return 1;}
        lua_newtable(state);Number(state,"year",value->year);Number(state,"month",value->month);
        Number(state,"day",value->day);Number(state,"weekday",value->weekday);Number(state,"daysInMonth",value->daysInMonth);return 1;
    }
    static int AddDays(lua_State* state)
    {
        const char* date=lua_tostring(state,1);
        const auto result=date?Calendar::AddDays(date,static_cast<int>(lua_tointeger(state,2))):std::nullopt;
        if(result)lua_pushlstring(state,result->data(),result->size());else lua_pushnil(state);return 1;
    }
    static int SelectDate(lua_State* state)
    {const char* date=lua_tostring(state,1);lua_pushboolean(state,date&&Self(state).calendar_.SetSelectedDate(date));return 1;}
    static int StorageGet(lua_State* state)
    {
        const char* key=lua_tostring(state,1);const auto& values=Self(state).storage;
        const auto found=values.find(key?key:"");
        if(found==values.end())lua_pushnil(state);else lua_pushlstring(state,found->second.data(),found->second.size());return 1;
    }
    static int StorageSet(lua_State* state)
    {
        const char* key=lua_tostring(state,1);const char* value=lua_tostring(state,2);
        if(key){if(value)Self(state).storage[key]=value;else Self(state).storage.erase(key);}lua_pushboolean(state,key!=nullptr);return 1;
    }
    static int Subscribe(lua_State* state)
    {
        lua_newtable(state);lua_pushvalue(state,1);lua_setfield(state,-2,"topic");
        for(const auto* key:{"fromDate","toDate"})
        {
            if(lua_istable(state,2)){lua_getfield(state,2,key);lua_setfield(state,-2,key);}
        }
        lua_pushlightuserdata(state,&Self(state));lua_pushcclosure(state,DataValue,1);lua_setfield(state,-2,"value");
        lua_pushcfunction(state,[](lua_State* s){lua_pushboolean(s,1);return 1;});lua_setfield(state,-2,"unsubscribe");return 1;
    }
    static int DataValue(lua_State* state)
    {
        auto& vm=Self(state);lua_getfield(state,1,"topic");const char* raw=lua_tostring(state,-1);
        const std::string topic=raw?raw:"";lua_pop(state,1);
        lua_newtable(state);lua_pushboolean(state,1);lua_setfield(state,-2,"available");lua_newtable(state);
        if(topic=="calendar.selectedDate")Text(state,"date",vm.calendar_.SelectedDate());
        else if(topic=="calendar.events")
        {
            lua_getfield(state,1,"fromDate");const char* from=lua_tostring(state,-1);const std::string first=from?from:"0001-01-01";lua_pop(state,1);
            lua_getfield(state,1,"toDate");const char* to=lua_tostring(state,-1);const std::string last=to?to:"9999-12-31";lua_pop(state,1);
            vm.invalidRange=vm.invalidRange||!Calendar::GetDateInfo(first)||!Calendar::GetDateInfo(last)||first>last;
            lua_newtable(state);int index=1;
            for(const auto& event:vm.calendar_.Events(first,last))
            {
                lua_newtable(state);Text(state,"id",event.id);Text(state,"date",event.date);Text(state,"title",event.title);
                Number(state,"revision",event.revision);Number(state,"startMinutes",event.startMinutes);Number(state,"endMinutes",event.endMinutes);
                lua_pushboolean(state,event.allDay);lua_setfield(state,-2,"allDay");lua_rawseti(state,-2,index++);
            }
            lua_setfield(state,-2,"events");
        }
        lua_setfield(state,-2,"value");return 1;
    }
    static float Field(lua_State* state,int table,const char* name)
    {lua_getfield(state,table,name);const auto value=static_cast<float>(lua_tonumber(state,-1));lua_pop(state,1);return value;}
    static InteractionShape Shape(lua_State* state,int table)
    {
        table=lua_absindex(state,table);InteractionShape shape;
        shape.x=Field(state,table,"x");shape.y=Field(state,table,"y");shape.width=Field(state,table,"width");shape.height=Field(state,table,"height");shape.radius=Field(state,table,"radius");
        lua_getfield(state,table,"type");const char* type=lua_tostring(state,-1);
        if(type&&std::string_view(type)=="roundedRect")shape.type=InteractionShapeType::RoundedRect;
        else if(type&&std::string_view(type)=="circle")shape.type=InteractionShapeType::Circle;
        lua_pop(state,1);return shape;
    }
    static int Region(lua_State* state)
    {
        auto& vm=Self(state);InteractionRegion region;
        lua_getfield(state,1,"key");const char* key=lua_tostring(state,-1);region.key=key?key:"";lua_pop(state,1);
        lua_getfield(state,1,"shape");region.shape=Shape(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"enabled");region.enabled=lua_isnil(state,-1)||lua_toboolean(state,-1);lua_pop(state,1);
        lua_getfield(state,1,"events");
        if(lua_istable(state,-1))
        {
            for(const char* event:{"click","doubleClick","contextMenu"})
            {
                lua_getfield(state,-1,event);
                if(lua_istable(state,-1))
                {
                    lua_getfield(state,-1,"id");const char* action=lua_tostring(state,-1);
                    if(action)region.events[event].id=action;
                    lua_pop(state,1);
                }
                lua_pop(state,1);
            }
        }
        lua_pop(state,1);
        std::string error;vm.submitted.push_back(region);
        if(!vm.regions.Submit(std::move(region),error))vm.invalidRange=true;
        return 0;
    }
    static int Scroll(lua_State* state)
    {auto& vm=Self(state);lua_getfield(state,1,"shape");vm.viewport=Shape(state,-1);lua_pop(state,1);lua_newtable(state);lua_pushnumber(state,vm.scrollOffset);lua_setfield(state,-2,"offset");return 1;}
    static int DrawText(lua_State* state)
    {const char* text=lua_tostring(state,3);if(text)Self(state).drawn.emplace_back(text);return 0;}
    static int Mutate(lua_State* state,WidgetTimerMutation::Kind kind,bool repeat=false)
    {
        auto& vm=Self(state);const char* name=lua_tostring(state,1);
        WidgetTimerMutation mutation;mutation.kind=kind;mutation.name=name?name:"";mutation.repeat=repeat;
        if(kind==WidgetTimerMutation::Kind::Set)mutation.intervalMs=static_cast<int>(lua_tointeger(state,2));
        if(kind==WidgetTimerMutation::Kind::At)mutation.epochMilliseconds=lua_tointeger(state,2);
        if(kind==WidgetTimerMutation::Kind::Timeline&&lua_istable(state,2))
        {
            for(lua_Integer index=1;index<=static_cast<lua_Integer>(lua_rawlen(state,2));++index)
            {
                lua_rawgeti(state,2,index);lua_getfield(state,-1,"at");
                mutation.entries.push_back({lua_tointeger(state,-1),{}});lua_pop(state,2);
            }
        }
        if(lua_istable(state,3)){lua_getfield(state,3,"whenHidden");const char* policy=lua_tostring(state,-1);
            if(policy&&std::string_view(policy)=="pause")mutation.hiddenPolicy=ScheduleHiddenPolicy::Pause;
            lua_pop(state,1);}
        lua_getfield(state,LUA_REGISTRYINDEX,"__widget_runtime_token");const auto token=static_cast<std::uint64_t>(lua_tointeger(state,-1));lua_pop(state,1);
        const bool result=DispatchWidgetTimerMutation(vm.owners_,L"instance",token,std::move(mutation),
            Schedule::TimePoint{std::chrono::milliseconds(vm.steady)},Schedule::WallTimePoint{std::chrono::milliseconds(vm.wall)},[](auto&){});
        lua_pushboolean(state,result);return 1;
    }
    static int Every(lua_State* state){return Mutate(state,WidgetTimerMutation::Kind::Set,true);}
    static int After(lua_State* state){return Mutate(state,WidgetTimerMutation::Kind::Set);}
    static int At(lua_State* state){return Mutate(state,WidgetTimerMutation::Kind::At);}
    static int Timeline(lua_State* state){return Mutate(state,WidgetTimerMutation::Kind::Timeline);}
    static int Cancel(lua_State* state){return Mutate(state,WidgetTimerMutation::Kind::Cancel);}
    Calendar& calendar_;std::vector<TimerOwner>& owners_;std::uint64_t token_;lua_State* state_=nullptr;
};

long long ModelInteger(Vm& vm,const char* key)
{
    lua_getglobal(vm.State(),"model");lua_getfield(vm.State(),-1,key);
    const auto result=lua_tointeger(vm.State(),-1);lua_pop(vm.State(),2);return result;
}
long long GlobalInteger(Vm& vm,const char* key)
{
    lua_getglobal(vm.State(),key);const auto value=lua_tointeger(vm.State(),-1);
    lua_pop(vm.State(),1);return value;
}
bool GlobalBoolean(Vm& vm,const char* key)
{
    lua_getglobal(vm.State(),key);const bool result=lua_toboolean(vm.State(),-1)!=0;
    lua_pop(vm.State(),1);return result;
}
std::vector<std::string> DateRegions(const Vm& vm)
{
    std::vector<std::string> dates;
    for(const auto& region:vm.submitted)
        if(region.key.starts_with("calendar.date."))dates.push_back(region.key.substr(14));
    return dates;
}
void TestPartialRows(const fs::path& root,const std::string& component)
{
    Directory directory;
    Calendar calendar(directory.path/L"calendar.json",[]{return snowdesktop::calendar::CalendarNow{"2026-10-15",0};});
    Require(calendar.Load(),"load isolated row calendar");
    for(int index=0;index<30;++index)
    {
        snowdesktop::calendar::CalendarEvent event;
        event.title="Appointment "+std::to_string(index);event.date="2026-10-15";event.allDay=true;
        Require(calendar.Create(std::move(event)).ok,"populate real agenda events");
    }
    {
        std::vector<TimerOwner> owners(1);owners.front().runtimeToken=1;
        Vm vm(calendar,owners,1);vm.storage["query"]="fixture";
        vm.Load(root/"widgets"/component/"main.lua");
        if(component=="quick-launcher")vm.Run(R"lua(
model.query="fixture";model.rows={};for i=1,30 do
model.rows[i]={kind="item",item={ref="app:"..i,title="Item "..i,kind="app.reference",preview=true}}end
)lua");
        if(component=="rss-reader")vm.Run(R"lua(
model.loading=false;model.articles={};for i=1,30 do
model.articles[i]={title="Article "..i,link="https://example.invalid/"..i,date="",displayDate=""}end
)lua");
        const std::string prefix=component=="agenda"?"agenda.event.":component=="quick-launcher"?"quick.item.":"rss.article.";
        const std::string eventName=component=="rss-reader"?"doubleClick":"click";
        for(const int height:{240,280,340})
        {
            vm.Render(420,height);
            Require(!vm.invalidRange,component+" submits valid native regions");
            const auto& viewport=vm.viewport;
            Require(viewport.height>0,component+" creates actual scroll viewport");
            const float center=viewport.x+viewport.width/2;
            std::string actionTarget;
            vm.regions.ActionAt(center,viewport.y-1,eventName,&actionTarget);
            Require(!actionTarget.starts_with(prefix),component+" row cannot receive header clicks");
            actionTarget.clear();
            vm.regions.ActionAt(center,viewport.y+viewport.height+1,eventName,&actionTarget);
            Require(!actionTarget.starts_with(prefix),component+" row cannot receive footer clicks");
            int rows=0;bool clippedTop=false,visibleHit=false;
            for(const auto& region:vm.submitted)
            {
                if(!region.key.starts_with(prefix))continue;
                ++rows;
                const auto& shape=region.shape;
                Require(shape.y>=viewport.y-0.01f && shape.y+shape.height<=viewport.y+viewport.height+0.01f,
                    component+" partially scrolled row must stay inside the visible viewport");
                clippedTop=clippedTop||std::abs(shape.y-viewport.y)<0.01f;
                const auto* action=vm.regions.ActionAt(shape.x+shape.width/2,shape.y+shape.height/2,eventName,&actionTarget);
                visibleHit=visibleHit||(action&&!action->id.empty()&&actionTarget==region.key);
            }
            Require(rows>=2 && clippedTop && visibleHit,component+" tests clipped leading row and real visible row hits");
        }
    }
}
void TestMonthNavigation(const fs::path& root)
{
    Directory directory;Calendar calendar(directory.path/L"calendar.json",[]{return snowdesktop::calendar::CalendarNow{"2026-10-15",0};});
    Require(calendar.Load()&&calendar.SetSelectedDate("2026-10-15"),"select real calendar date");
    std::vector<TimerOwner> owners(1);owners.front().runtimeToken=1;
    Vm vm(calendar,owners,1);vm.Load(root/"widgets/month-calendar/main.lua");vm.Render(420,140);
    Require(DateRegions(vm).size()<=7,"exercise compact calendar layout");
    vm.Action("calendar.next");vm.Render(420,140);
    const auto dates=DateRegions(vm);
    Require(ModelInteger(vm,"viewYear")==2026&&ModelInteger(vm,"viewMonth")==11,"next advances calendar model");
    Require(std::any_of(dates.begin(),dates.end(),[](const auto& date){return date.starts_with("2026-11-");}),
        "compact next-month navigation must display the viewed month rather than the previous selection's week");
    Require(calendar.SelectedDate()=="2026-10-15","month browsing preserves selected date");
    vm.Action("calendar.previous");vm.Render(420,700);
    Require(DateRegions(vm).size()>=28 && ModelInteger(vm,"viewMonth")==10,"full layout returns to selected month");
}
void TestMonthBoundaries(const fs::path& root)
{
    for(const std::string boundary:{"0001-01-01","9999-12-31"})
    for(const std::string weekStart:{"sunday","monday"})
    for(const std::string adjacent:{"0","1"})
    {
        Directory directory;Calendar calendar(directory.path/L"calendar.json",[boundary]{return snowdesktop::calendar::CalendarNow{boundary,0};});
        Require(calendar.Load()&&calendar.SetSelectedDate(boundary),"select supported boundary date");
        std::vector<TimerOwner> owners(1);owners.front().runtimeToken=1;
        Vm vm(calendar,owners,1);vm.storage["weekStart"]=weekStart;vm.storage["showAdjacent"]=adjacent;
        vm.Load(root/"widgets/month-calendar/main.lua");
        for(const int height:{140,700})
        {
            vm.Render(420,height);const auto dates=DateRegions(vm);
            Require(!dates.empty()&&!vm.invalidRange,"boundary month renders and subscribes using valid dates");
            for(const auto& date:dates)Require(Calendar::GetDateInfo(date).has_value(),"render never submits out-of-range day");
            lua_getglobal(vm.State(),"_annotationRange");
            Require(lua_istable(vm.State(),-1),"annotation provider received the actual month range");
            for(const int index:{1,2})
            {
                lua_rawgeti(vm.State(),-1,index);const char* value=lua_tostring(vm.State(),-1);
                Require(value&&Calendar::GetDateInfo(value).has_value(),"annotation range is clamped at supported year boundaries");
                lua_pop(vm.State(),1);
            }
            lua_pop(vm.State(),1);
        }
        const auto year=ModelInteger(vm,"viewYear"),month=ModelInteger(vm,"viewMonth");
        vm.Action(boundary.starts_with("0001")?"calendar.previous":"calendar.next");vm.Render(420,140);
        Require(ModelInteger(vm,"viewYear")==year&&ModelInteger(vm,"viewMonth")==month,
            "month navigation stops at years 0001 and 9999");
    }
}
void TestHiddenDeadline(const fs::path& root)
{
    Directory directory;Calendar calendar(directory.path/L"calendar.json");
    std::vector<TimerOwner> owners(1);owners.front().runtimeToken=1;
    Vm vm(calendar,owners,1);vm.storage["workMin"]="1";vm.storage["breakMin"]="1";
    vm.Run(R"lua(
_audioPlays=0
widget.hasFeature=function(name)return name=="task.audio.playback" end
task={start=function(name) if name=="audio.play" then _audioPlays=_audioPlays+1 end;return 1 end}
)lua");
    vm.Load(root/"widgets/pomodoro/main.lua");vm.Action("pomodoro.start");
    Require(vm.storage["state"]=="work","start actual pomodoro phase");
    owners.front().namedTimers.SetVisible(false,Schedule::TimePoint{});
    // Sixty steady seconds pass, while a wall-clock rollback leaves only thirty
    // elapsed seconds. Consume the real one-shot deadline while visual is paused.
    vm.FireDue(60000,1'030'000);
    Require(vm.storage["state"]=="work","wall rollback must not finish the work phase early");
    const auto delay=owners.front().namedTimers.NextDelay(Schedule::TimePoint{std::chrono::milliseconds(60000)},
        Schedule::WallTimePoint{std::chrono::milliseconds(1'030'000)});
    Require(delay&&delay->count()==30000,"consumed hidden deadline must rearm the actual remaining wall time");
    vm.FireDue(90000,1'060'000);
    Require(vm.storage["state"]=="break"&&vm.storage["sessions"]=="1","hidden pomodoro completes after the replacement deadline fires");
    Require(GlobalInteger(vm,"_audioPlays")==0,"completion sound is off by default even with playback permission");
    vm.storage["soundEnabled"]="true";
    vm.Action("pomodoro.start");vm.FireDue(150000,1'120'000);
    Require(GlobalInteger(vm,"_audioPlays")==1,"an enabled sound plays once at a hidden completion deadline");
}
void PushContext(lua_State* state){lua_newtable(state);}
void TestRetiringVmTimers()
{
    Directory directory;Calendar calendar(directory.path/L"calendar.json");std::vector<TimerOwner> owners;
    owners.reserve(2);owners.emplace_back();owners.back().runtimeToken=1;
    Vm oldVm(calendar,owners,1);
    const std::string definition=R"lua(
_ticks=0;descriptor={setup=function()schedule.every("tick",100,{whenHidden="continue"});return {}end,
event=function()_ticks=_ticks+1 end,dispose=function()
_cancelled=schedule.cancel("tick")
_setEvery=schedule.every("tick",1000)
_setAfter=schedule.after("tick",1000)
_setAt=schedule.at("tick",1001000)
_setTimeline=schedule.timeline("tick",{{at=1001000}})
end}
)lua";
    auto setup=[&](Vm& vm,WidgetLuaLifecycle& lifecycle)
    {
        vm.Run(definition);lua_getglobal(vm.State(),"descriptor");const int ref=luaL_ref(vm.State(),LUA_REGISTRYINDEX);
        std::string error;Require(lifecycle.Setup(vm.State(),ref,PushContext,error),"actual lifecycle setup: "+error);return ref;
    };
    WidgetLuaLifecycle oldLifecycle,newLifecycle;const int oldRef=setup(oldVm,oldLifecycle);
    owners.emplace_back();owners.back().runtimeToken=2;Vm newVm(calendar,owners,2);
    const int newRef=setup(newVm,newLifecycle);
    std::string error;
    Require(oldLifecycle.Dispose(oldVm.State(),oldRef,PushContext,"replaced",error),"actual retiring VM disposal: "+error);
    for(const char* result:{"_cancelled","_setEvery","_setAfter","_setAt","_setTimeline"})
        Require(!GlobalBoolean(oldVm,result),std::string("retiring VM cannot mutate replacement timer via ")+result);
    Vm unbound(calendar,owners,0);
    unbound.Run("_blocked=schedule.cancel('tick');_blockedSet=schedule.every('tick',1000);"
        "_blockedAfter=schedule.after('tick',1000);_blockedAt=schedule.at('tick',1001000);"
        "_blockedTimeline=schedule.timeline('tick',{{at=1001000}})");
    for(const char* result:{"_blocked","_blockedSet","_blockedAfter","_blockedAt","_blockedTimeline"})
        Require(!GlobalBoolean(unbound,result),"module without runtime generation cannot target an existing VM");
    for(const int elapsed:{100,200})
    {
        auto& timers=owners.back().namedTimers;
        const auto now=Schedule::TimePoint{std::chrono::milliseconds(elapsed)};
        const auto wall=Schedule::WallTimePoint{std::chrono::milliseconds(1'000'000+elapsed)};
        const auto due=timers.DueNames(now,wall);
        Require(due==std::vector<std::string>{"tick"}&&timers.ConsumeDueInfo("tick",now,wall).has_value(),
            "new VM's same-name repeating timer still fires at its original cadence");
        lua_newtable(newVm.State());lua_pushliteral(newVm.State(),"schedule");lua_setfield(newVm.State(),-2,"kind");
        lua_pushliteral(newVm.State(),"tick");lua_setfield(newVm.State(),-2,"id");bool invoked=false;
        Require(newLifecycle.Event(newVm.State(),newRef,PushContext,-1,invoked,error)&&invoked,"dispatch actual replacement VM lifecycle event");
        lua_pop(newVm.State(),1);
    }
    Require(GlobalInteger(newVm,"_ticks")==2,"replacement lifecycle receives both timer events");
    oldLifecycle.Release(oldVm.State());newLifecycle.Release(newVm.State());
    luaL_unref(oldVm.State(),LUA_REGISTRYINDEX,oldRef);luaL_unref(newVm.State(),LUA_REGISTRYINDEX,newRef);
}
void TestCalendarProviderRecovery()
{
    Directory directory;
    const auto blocked=directory.path/L"blocked.json";
    Require(fs::create_directory(blocked),"create actual calendar-store directory obstruction");
    Calendar blockedService(blocked);
    Require(!LoadWidgetCalendarProvider(blockedService,blocked),"existing calendar directory cannot become an available empty provider");
    Require(fs::is_directory(blocked),"invalid calendar entry is preserved");
    Require(fs::remove(blocked),"remove only owned empty calendar obstruction");
    Require(LoadWidgetCalendarProvider(blockedService,blocked),"explicit reload recovers after directory obstruction is removed");
    const auto path=directory.path/L"calendar.json";
    const auto seriesPath=directory.path/L"calendar-series.json";
    const auto write=[](const fs::path& file,const std::string& text)
    {
        std::ofstream stream(file,std::ios::binary|std::ios::trunc);stream<<text;stream.close();
        Require(stream.good(),"write owned calendar fixture");
    };
    const auto read=[](const fs::path& file)
    {
        std::ifstream stream(file,std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream),{});
    };
    Calendar service(path,[]{return snowdesktop::calendar::CalendarNow{"2026-10-15",0};});
    Require(LoadWidgetCalendarProvider(service,path),"genuinely absent stores initialize an empty calendar");
    snowdesktop::calendar::CalendarEvent event;event.title="Retained event";event.date="2026-10-15";event.allDay=true;
    const auto created=service.Create(event);Require(created.ok,"persist real calendar event before provider failure");
    const auto original=read(path);
    write(path,"{ broken calendar");
    Require(!LoadWidgetCalendarProvider(service,path),"real parser load failure cannot become an available empty provider");
    Require(read(path)=="{ broken calendar","failed guarded loading preserves the unreadable original entry");
    write(path,original);
    Require(LoadWidgetCalendarProvider(service,path)&&service.EventById(created.id).has_value(),
        "explicit reload after repair restores the existing event");
    {
        struct Handle{HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} lease;
        lease.value=CreateFileW(path.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(lease.value!=INVALID_HANDLE_VALUE,"hold exclusive read fixture");
        Require(!LoadWidgetCalendarProvider(service,path),"existing inaccessible file is unavailable rather than an empty result");
        Require(service.EventById(created.id).has_value(),"failed access preflight does not erase the last loaded model");
    }
    Require(LoadWidgetCalendarProvider(service,path)&&read(path)==original,"unlocked provider can explicitly reload without rewriting data");
    Require(fs::create_directory(seriesPath),"create actual series-store directory obstruction");
    Require(!LoadWidgetCalendarProvider(service,path),"existing series path directory cannot be mistaken for a missing store");
    Require(fs::remove(seriesPath),"remove only owned empty series directory");
    write(seriesPath,"{ broken series");
    Require(!LoadWidgetCalendarProvider(service,path),"series parser failure keeps the whole calendar provider unavailable");
    Require(read(path)==original&&read(seriesPath)=="{ broken series","failed series loading preserves both store entries");
    Require(fs::remove(seriesPath),"remove only owned corrupt series fixture");
    Require(LoadWidgetCalendarProvider(service,path)&&service.EventById(created.id).has_value(),"explicit reload recovers after the series obstruction is repaired");
}
}

void TestBuiltinInteractionTimers(const std::filesystem::path& repository)
{
    int failed=0;
    const auto run=[&](const char* name,auto test)
    {
        try{test();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"FAIL "<<name<<": "<<error.what()<<'\n';}
    };
    for(const std::string component:{"agenda","quick-launcher","rss-reader"})
        run((component+" actual Lua rows with native clipped hit testing").c_str(),[&]{TestPartialRows(repository,component);});
    run("actual compact month navigation",[&]{TestMonthNavigation(repository);});
    run("actual CalendarService boundary rendering",[&]{TestMonthBoundaries(repository);});
    run("actual hidden pomodoro deadline after wall rollback",[&]{TestHiddenDeadline(repository);});
    run("actual two-VM lifecycle timer ownership",TestRetiringVmTimers);
    run("actual CalendarService failed load and explicit recovery",TestCalendarProviderRecovery);
    if(failed)std::exit(1);
}
