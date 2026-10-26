#include "widget/runtime/widget_lua_lifecycle.h"
#include "widget/runtime/lua_runtime.h"
#include "widget/runtime/widget_runtime_scheduler.h"
#include "widget/api/widget_calendar_query_options.h"
#include "widget/data/widget_calendar_event_query.h"
#include "widget/storage/widget_storage_transaction.h"
#include "widget/storage/widget_storage_value.h"
#include "widget/storage/widget_storage_write_budget.h"
#include "widget/storage/widget_storage_persistence.h"

#include <windows.h>
extern "C" {
#include <lualib.h>
}
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>

namespace
{
using namespace snowdesktop::widget_runtime;
using snowdesktop::calendar::CalendarEvent;
using snowdesktop::calendar::CalendarService;
using Value = InteractionValue;
using Clock = NamedTimerSchedule::Clock;

void Check(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Check(static_cast<bool>(file), "read actual production Lua: " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}
Value String(std::string text) { Value value; value.type=Value::Type::String; value.string=std::move(text); return value; }
Value Integer(long long number) { Value value; value.type=Value::Type::Integer; value.integer=number; return value; }

void PushValue(lua_State* state, const Value& value)
{
    switch (value.type)
    {
    case Value::Type::Null: lua_pushnil(state); break;
    case Value::Type::Boolean: lua_pushboolean(state,value.boolean); break;
    case Value::Type::Integer: lua_pushinteger(state,value.integer); break;
    case Value::Type::Number: lua_pushnumber(state,value.number); break;
    case Value::Type::String: lua_pushlstring(state,value.string.data(),value.string.size()); break;
    case Value::Type::Array:
        lua_createtable(state,static_cast<int>(value.array.size()),0);
        for (std::size_t index=0;index<value.array.size();++index) { PushValue(state,value.array[index]);lua_rawseti(state,-2,static_cast<lua_Integer>(index+1)); }
        break;
    case Value::Type::Object:
        lua_createtable(state,0,static_cast<int>(value.object.size()));
        for (const auto& [key,item]:value.object) { PushValue(state,item);lua_setfield(state,-2,key.c_str()); }
        break;
    }
}

struct SharedStorage
{
    StorageMap values;
    std::filesystem::path path;
    std::size_t writes=0;
    Value Get(const std::string& key) const
    {
        WidgetStorageTransaction transaction(values,"component");std::string error;
        const auto encoded=transaction.Get(key,error);Value value;
        if (encoded) Check(DecodeTypedStorageValue(*encoded,value,error),error);
        return value;
    }
    bool Put(const std::string& key,const Value& value,std::string& error)
    {
        std::string encoded;bool changed=false;
        if (!EncodeTypedStorageValue(value,encoded,error)) return false;
        WidgetStorageTransaction transaction(values,"component");
        if (!transaction.Set(key,std::move(encoded),changed,error) || !transaction.ValidateCommit(error)) return false;
        if (!changed) return true;
        auto candidate=transaction.TakeCandidate();
        if (!storage_persistence::SaveStorageMap(path,candidate)) { error="actual isolated storage replacement failed";return false; }
        values=std::move(candidate);++writes;return true;
    }
    void Seed(const std::string& key,const Value& value) { std::string error;Check(Put(key,value,error),error); }
};

struct Host
{
    struct Subscription { std::string from,to,id;std::optional<CalendarEventQueryOptions> query;bool active=true; };
    lua_State* state=nullptr;
    LuaRuntimeQuota quota;
    WidgetLuaLifecycle lifecycle;
    int definition=LUA_NOREF;
    std::filesystem::path package;
    std::shared_ptr<SharedStorage> storage;
    CalendarService* calendar=nullptr;
    NamedTimerSchedule timers;
    AnimationFrameRequests frames;
    WidgetStorageWriteBudget budget;
    std::map<std::string,int> modules;
    std::map<int,Subscription> subscriptions;
    std::vector<std::string> drawn,translated;
    std::vector<std::string> drawnImages;
    std::map<std::string,bool> lazyImages;
    std::set<std::string> pendingImages;
    std::vector<std::pair<std::string,long long>> scheduled;
    std::string date="2026-10-16",error,failSchedule;
    long long now=1000;
    bool queryCap=true,permission=true,available=true,preview=false,lazyImageCap=true;
    int opens=0,closes=0,notifications=0;

    static Host& Get(lua_State* state)
    {
        lua_getfield(state,LUA_REGISTRYINDEX,"community.host");
        auto* host=static_cast<Host*>(lua_touserdata(state,-1));lua_pop(state,1);return *host;
    }
    static void Context(lua_State* state)
    {
        lua_newtable(state);lua_pushboolean(state,Get(state).preview);lua_setfield(state,-2,"preview");
    }
    static int Noop(lua_State*) { return 0; }
    static int False(lua_State* state) { lua_pushboolean(state,0);return 1; }
    static int Identity(lua_State* state) { lua_pushvalue(state,1);return 1; }
    static int Define(lua_State* state) { return Identity(state); }
    static int Feature(lua_State* state)
    {
        const std::string_view feature=luaL_checkstring(state,1);
        lua_pushboolean(state,feature=="resource.image.lazy" ? Get(state).lazyImageCap :
            feature=="data.calendar.events.query" ? Get(state).queryCap : feature!="ui.durationPicker");return 1;
    }
    static int Permission(lua_State* state) { lua_pushboolean(state,Get(state).permission);return 1; }
    static int Theme(lua_State* state) { lua_newtable(state);lua_pushinteger(state,1);lua_setfield(state,-2,"contentTheme");return 1; }
    static int Open(lua_State* state) { ++Get(state).opens;return 0; }
    static int Close(lua_State* state) { ++Get(state).closes;return 0; }
    static int Now(lua_State* state) { lua_pushinteger(state,Get(state).now);return 1; }
    static int Parts(lua_State* state)
    {
        const auto info=CalendarService::GetDateInfo(Get(state).date);lua_newtable(state);
        for (const auto& [name,value]:{std::pair{"year",info->year},{"month",info->month},{"day",info->day}})
        { lua_pushinteger(state,value);lua_setfield(state,-2,name); }
        return 1;
    }
    static int Width(lua_State* state) { lua_pushnumber(state,500);return 1; }
    static int Height(lua_State* state) { lua_pushnumber(state,350);return 1; }
    static int Metrics(lua_State* state) { lua_newtable(state);lua_pushnumber(state,32);lua_setfield(state,-2,"layoutRowHeight");return 1; }
    static int Language(lua_State* state) { lua_pushliteral(state,"en-US");return 1; }
    static int Translate(lua_State* state)
    {
        auto& host=Get(state);std::string value=luaL_checkstring(state,1);host.translated.push_back(value);
        for (int index=2;index<=lua_gettop(state);++index)
        { std::size_t count=0;const auto* argument=luaL_tolstring(state,index,&count);value.append(":").append(argument,count);lua_pop(state,1); }
        lua_pushlstring(state,value.data(),value.size());return 1;
    }
    static int DrawText(lua_State* state)
    {
        std::size_t count=0;const auto* text=luaL_checklstring(state,3,&count);Get(state).drawn.emplace_back(text,count);return 0;
    }
    static int Measure(lua_State* state)
    {
        std::size_t count=0;(void)luaL_checklstring(state,1,&count);const auto size=luaL_checknumber(state,2);
        lua_newtable(state);lua_pushnumber(state,static_cast<double>(count)*size*.45);lua_setfield(state,-2,"width");
        lua_pushnumber(state,size*1.2);lua_setfield(state,-2,"height");lua_newtable(state);
        lua_pushnumber(state,0);lua_setfield(state,-2,"top");lua_pushnumber(state,size);lua_setfield(state,-2,"height");
        lua_pushnumber(state,static_cast<double>(count)*size*.45);lua_setfield(state,-2,"width");lua_setfield(state,-2,"ink");return 1;
    }
    static int ReadStorage(lua_State* state) { PushValue(state,Get(state).storage->Get(luaL_checkstring(state,1)));return 1; }
    static int WriteStorage(lua_State* state)
    {
        auto& host=Get(state);bool accepted=false;
        {
            const std::string key=luaL_checkstring(state,1);Value value;
            if (lua_isboolean(state,2)) { value.type=Value::Type::Boolean;value.boolean=lua_toboolean(state,2)!=0; }
            else if (lua_isinteger(state,2)) value=Integer(lua_tointeger(state,2));
            else if (lua_type(state,2)==LUA_TSTRING) value=String(lua_tostring(state,2));
            else { host.error="fixture accepts the production primitive storage values used in these scenarios"; }
            if (value.type!=Value::Type::Null)
            {
                const auto decision=host.budget.Consume(Clock::time_point{std::chrono::milliseconds(host.now)});
                accepted=decision.allowed && host.storage->Put(key,value,host.error);
                if (!decision.allowed) host.error="actual production storage write budget exhausted";
            }
        }
        if (!accepted) { lua_pushlstring(state,host.error.data(),host.error.size());return lua_error(state); }
        return 0;
    }
    static int Timer(lua_State* state)
    {
        auto& host=Get(state);const auto* name=luaL_checkstring(state,1);const auto delay=luaL_checkinteger(state,2);
        // The host Lua API validates before NamedTimerSchedule's internal clamp.
        if (delay<1 || delay>NamedTimerSchedule::MaxIntervalMs)
            return luaL_error(state,"schedule milliseconds must be between 1 and 86400000");
        if (host.failSchedule==name) { host.failSchedule.clear();lua_pushboolean(state,0);return 1; }
        auto policy=ScheduleHiddenPolicy::Throttle;
        if (lua_istable(state,3))
        {
            lua_getfield(state,3,"whenHidden");const std::string_view text=lua_tostring(state,-1);
            if (text=="continue") policy=ScheduleHiddenPolicy::Continue;
            else if (text=="pause") policy=ScheduleHiddenPolicy::Pause;
            lua_pop(state,1);
        }
        host.scheduled.emplace_back(name,delay);
        lua_pushboolean(state,host.timers.Set(name,static_cast<int>(delay),lua_toboolean(state,lua_upvalueindex(1))!=0,
            Clock::time_point{std::chrono::milliseconds(host.now)},policy));return 1;
    }
    static int Cancel(lua_State* state) { lua_pushboolean(state,Get(state).timers.Cancel(luaL_checkstring(state,1)));return 1; }
    static int Frame(lua_State* state) { lua_pushboolean(state,Get(state).frames.Request(luaL_checkstring(state,1)));return 1; }
    static int CancelFrame(lua_State* state) { lua_pushboolean(state,Get(state).frames.Cancel(luaL_checkstring(state,1)));return 1; }
    static int Task(lua_State* state)
    {
        if (std::string_view(luaL_checkstring(state,1))=="notification.show") ++Get(state).notifications;
        lua_pushinteger(state,100);return 1;
    }
    static int Image(lua_State* state)
    {
        const std::string name=luaL_checkstring(state,1);bool lazy=false;
        if(lua_istable(state,2)){lua_getfield(state,2,"lazy");lazy=lua_toboolean(state,-1)!=0;lua_pop(state,1);}
        Check(!lazy || Get(state).lazyImageCap,"old hosts must not receive lazy image options");
        Get(state).lazyImages[name]=lazy;lua_pushlstring(state,name.data(),name.size());return 1;
    }
    static int ImageStatus(lua_State* state)
    {
        const std::string name=luaL_checkstring(state,1);lua_newtable(state);
        lua_pushstring(state,Get(state).pendingImages.contains(name)?"pending":"ready");
        lua_setfield(state,-2,"state");return 1;
    }
    static int DrawImage(lua_State* state)
    { Get(state).drawnImages.emplace_back(luaL_checkstring(state,1));return 0; }
    // Storage scenarios keep audio allocation/playback outside the fixture;
    // wooden-fish still resolves its declared sound while loading real Lua.
    static int Audio(lua_State* state) { luaL_checkstring(state,1);lua_pushliteral(state,"fixture-audio");return 1; }
    static int Subscribe(lua_State* state)
    {
        auto& host=Get(state);Subscription subscription;
        if (!snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,2,subscription.query,host.error))
        { lua_pushlstring(state,host.error.data(),host.error.size());return lua_error(state); }
        for (const auto& [key,target]:{std::pair{"fromDate",&subscription.from},{"toDate",&subscription.to},{"eventId",&subscription.id}})
        { lua_getfield(state,2,key);if (lua_isstring(state,-1)) *target=lua_tostring(state,-1);lua_pop(state,1); }
        const int id=static_cast<int>(host.subscriptions.size()+1);host.subscriptions.emplace(id,std::move(subscription));
        lua_newtable(state);lua_pushinteger(state,id);lua_pushcclosure(state,Snapshot,1);lua_setfield(state,-2,"value");
        lua_pushinteger(state,id);lua_pushcclosure(state,Unsubscribe,1);lua_setfield(state,-2,"unsubscribe");return 1;
    }
    static int Unsubscribe(lua_State* state)
    { Get(state).subscriptions.at(static_cast<int>(lua_tointeger(state,lua_upvalueindex(1)))).active=false;return 0; }
    static int Snapshot(lua_State* state)
    {
        auto& host=Get(state);const auto& subscription=host.subscriptions.at(static_cast<int>(lua_tointeger(state,lua_upvalueindex(1))));
        lua_newtable(state);const bool available=subscription.active && host.permission && host.available;
        lua_pushboolean(state,available);lua_setfield(state,-2,"available");
        if (!host.permission) { lua_pushliteral(state,"permissionDenied");lua_setfield(state,-2,"error"); }
        if (!available) return 1;
        std::vector<CalendarEvent> events;
        if (!subscription.id.empty()) { if (auto event=host.calendar->EventById(subscription.id)) events.push_back(*event); }
        else events=host.calendar->Events(subscription.from,subscription.to);
        lua_newtable(state);bool truncated=false;
        if (subscription.query)
        {
            auto page=QueryCalendarEvents(events,*subscription.query);events=std::move(page.events);truncated=events.size()<page.total;
            lua_pushinteger(state,static_cast<lua_Integer>(page.total));lua_setfield(state,-2,"total");
            lua_pushboolean(state,page.hasMore);lua_setfield(state,-2,"hasMore");
            lua_pushinteger(state,static_cast<lua_Integer>(page.nextOffset));lua_setfield(state,-2,"nextOffset");
        }
        else if (events.size()>512) { truncated=true;events.resize(512); }
        lua_pushboolean(state,truncated);lua_setfield(state,-2,"truncated");lua_newtable(state);
        int index=1;for (const auto& event:events)
        {
            lua_newtable(state);for (const auto& [key,value]:{std::pair{"id",&event.id},{"title",&event.title},{"date",&event.date}})
            { lua_pushlstring(state,value->data(),value->size());lua_setfield(state,-2,key); }
            lua_rawseti(state,-2,index++);
        }
        lua_setfield(state,-2,"events");lua_setfield(state,-2,"value");return 1;
    }
    static int RequireModule(lua_State* state)
    {
        auto& host=Get(state);const std::string name=luaL_checkstring(state,1);
        if (auto found=host.modules.find(name);found!=host.modules.end()) { lua_rawgeti(state,LUA_REGISTRYINDEX,found->second);return 1; }
        const auto relative=std::filesystem::path(name);
        if (relative.is_absolute() || name.find("..")!=std::string::npos || name.rfind("modules/",0)!=0)
            return luaL_error(state,"unsafe fixture module path");
        const auto source=Read(host.package/relative);
        if (luaL_loadbuffer(state,source.data(),source.size(),name.c_str())!=LUA_OK ||
            snowdesktop::lua_runtime::ProtectedCall(state,0,1)!=LUA_OK) return lua_error(state);
        lua_pushvalue(state,-1);host.modules[name]=luaL_ref(state,LUA_REGISTRYINDEX);return 1;
    }
    void Library(const char* name,std::initializer_list<std::pair<const char*,lua_CFunction>> entries)
    {
        lua_newtable(state);for (const auto& [key,callback]:entries) { lua_pushcfunction(state,callback);lua_setfield(state,-2,key); }lua_setglobal(state,name);
    }
    Host(const std::filesystem::path& source,std::shared_ptr<SharedStorage> shared,CalendarService* service=nullptr,bool capability=true,bool previewMode=false,bool lazyCapability=true)
        : package(source),storage(std::move(shared)),calendar(service),queryCap(capability),preview(previewMode),lazyImageCap(lazyCapability)
    {
        state=luaL_newstate();Check(state!=nullptr,"create actual component Lua VM");
        for (const auto& [name,open]:{std::pair{"_G",luaopen_base},{LUA_TABLIBNAME,luaopen_table},{LUA_STRLIBNAME,luaopen_string},{LUA_MATHLIBNAME,luaopen_math},{LUA_UTF8LIBNAME,luaopen_utf8}})
        { luaL_requiref(state,name,open,1);lua_pop(state,1); }
        lua_pushlightuserdata(state,&quota);lua_setfield(state,LUA_REGISTRYINDEX,"__quota_ptr");
        lua_pushlightuserdata(state,this);lua_setfield(state,LUA_REGISTRYINDEX,"community.host");
        Library("widget",{{"define",Define},{"hasFeature",Feature},{"hasPermission",Permission},{"theme",Theme},{"invalidate",Noop},{"openPanel",Open},{"closePanel",Close},{"openSettings",Noop}});
        Library("module",{{"require",RequireModule}});Library("storage",{{"get",ReadStorage},{"set",WriteStorage},{"remove",Noop}});
        Library("time",{{"now",Now},{"monotonic",Now},{"parts",Parts}});Library("l10n",{{"tr",Translate},{"formatNumber",Identity},{"language",Language}});
        Library("layout",{{"contentWidth",Width},{"contentHeight",Height}});Library("ui",{{"metrics",Metrics}});
        Library("resource",{{"image",Image},{"status",ImageStatus},{"audio",Audio}});Library("data",{{"subscribe",Subscribe}});Library("task",{{"start",Task},{"cancel",Noop}});
        Library("interaction",{{"region",Noop},{"isPressed",False},{"isHovered",False},{"isFocused",False}});
        Library("animation",{{"requestFrame",Frame},{"cancelFrame",CancelFrame}});
        Library("draw",{{"text",DrawText},{"measureText",Measure},{"circle",Noop},{"line",Noop},{"rect",Noop},{"arc",Noop},{"path",Noop},{"strokeRect",Noop},{"imageFit",DrawImage},{"shadow",Noop},{"gradientRect",Noop}});
        Library("view",{{"text",Identity},{"button",Identity},{"row",Identity},{"column",Identity},{"scroll",Identity},{"searchBox",Identity},{"textInput",Identity},{"spacer",Identity},{"icon",Identity},{"iconButton",Identity}});
        Library("schedule",{{"cancel",Cancel}});lua_getglobal(state,"schedule");
        lua_pushboolean(state,0);lua_pushcclosure(state,Timer,1);lua_setfield(state,-2,"after");
        lua_pushboolean(state,1);lua_pushcclosure(state,Timer,1);lua_setfield(state,-2,"every");lua_pop(state,1);
        const auto main=Read(package/L"main.lua");
        Check(luaL_loadbuffer(state,main.data(),main.size(),"@actual-main.lua")==LUA_OK,"load actual main syntax");
        const auto loaded=snowdesktop::lua_runtime::ProtectedCall(state,0,1);
        Check(loaded==LUA_OK,LuaError());definition=luaL_ref(state,LUA_REGISTRYINDEX);
        const bool setup=lifecycle.Setup(state,definition,Context,error);Check(setup,"actual setup: "+error);
    }
    ~Host() { if (state) { lifecycle.Release(state);lua_close(state); } }
    std::string LuaError() const { const auto* value=lua_tostring(state,-1);return value?value:"unknown Lua failure"; }
    bool Event(const char* kind,const char* id=nullptr,const char* text=nullptr)
    {
        const auto top=lua_gettop(state);lua_newtable(state);lua_pushstring(state,kind);lua_setfield(state,-2,"kind");
        if (id) { lua_pushstring(state,id);lua_setfield(state,-2,"id"); }
        if (text) { lua_pushstring(state,text);lua_setfield(state,-2,"text"); }
        bool invoked=false;const bool accepted=lifecycle.Event(state,definition,Context,lua_gettop(state),invoked,error);
        lua_settop(state,top);return accepted && invoked;
    }
    void Act(const char* id,const char* text=nullptr) { const bool accepted=Event("action",id,text);Check(accepted,"actual action "+std::string(id)+": "+error); }
    int Model()
    { Check(lifecycle.PushRenderArguments(state,Context),"retain actual lifecycle model");lua_remove(state,-2);return lua_absindex(state,-1); }
    long long Number(const char* field,const char* parent=nullptr)
    {
        const auto top=lua_gettop(state);Model();if (parent) { lua_getfield(state,-1,parent);lua_remove(state,-2); }
        lua_getfield(state,-1,field);const auto result=lua_tointeger(state,-1);lua_settop(state,top);return result;
    }
    bool Boolean(const char* field,const char* parent=nullptr)
    {
        const auto top=lua_gettop(state);Model();if (parent) { lua_getfield(state,-1,parent);lua_remove(state,-2); }
        lua_getfield(state,-1,field);const bool result=lua_toboolean(state,-1)!=0;lua_settop(state,top);return result;
    }
    void Render(const char* function="render")
    {
        drawn.clear();translated.clear();lua_rawgeti(state,LUA_REGISTRYINDEX,definition);lua_getfield(state,-1,function);lua_remove(state,-2);
        Check(lifecycle.PushRenderArguments(state,Context),"render arguments from production lifecycle");
        const auto rendered=snowdesktop::lua_runtime::ProtectedCall(state,2,std::string_view(function)=="render"?0:1);
        Check(rendered==LUA_OK,"actual render: "+LuaError());
    }
    void Background()
    {
        drawnImages.clear();lua_rawgeti(state,LUA_REGISTRYINDEX,definition);
        lua_getfield(state,-1,"backgroundLayer");lua_getfield(state,-1,"render");
        lua_remove(state,-2);lua_remove(state,-2);
        Check(lifecycle.PushRenderArguments(state,Context),"background uses the production model");
        Check(snowdesktop::lua_runtime::ProtectedCall(state,2,0)==LUA_OK,"actual background render: "+LuaError());
    }
    static void Keys(lua_State* state,int index,std::set<std::string>& result)
    {
        index=lua_absindex(state,index);lua_getfield(state,index,"key");if (lua_isstring(state,-1)) result.insert(lua_tostring(state,-1));lua_pop(state,1);
        lua_getfield(state,index,"children");if (lua_istable(state,-1))
            for (lua_Integer child=1;child<=static_cast<lua_Integer>(lua_rawlen(state,-1));++child)
            { lua_rawgeti(state,-1,child);if (lua_istable(state,-1)) Keys(state,-1,result);lua_pop(state,1); }
        lua_pop(state,1);
    }
    std::set<std::string> Panel()
    { Render("panel");std::set<std::string> result;Keys(state,-1,result);lua_pop(state,1);return result; }
    void Dispose() { Check(lifecycle.Dispose(state,definition,Context,"hotReload",error),"actual dispose: "+error); }
    void Advance(long long milliseconds)
    {
        now+=milliseconds;const auto point=Clock::time_point{std::chrono::milliseconds(now)};
        for (const auto& name:timers.DueNames(point))
            if (timers.ConsumeDue(name,point)) Check(Event("schedule",name.c_str()),"actual schedule event: "+error);
    }
    void Forecast(const std::string& payload)
    {
        lua_pushliteral(state,"modules/json.lua");RequireModule(state);lua_getfield(state,-1,"decode");lua_remove(state,-2);
        lua_pushlstring(state,payload.data(),payload.size());Check(snowdesktop::lua_runtime::ProtectedCall(state,1,1)==LUA_OK,"decode actual weather payload");
        const int decoded=luaL_ref(state,LUA_REGISTRYINDEX);
        lua_settop(state,0);lua_pushliteral(state,"modules/weather.lua");RequireModule(state);lua_getfield(state,-1,"forecast");lua_remove(state,-2);
        lua_rawgeti(state,LUA_REGISTRYINDEX,decoded);Check(snowdesktop::lua_runtime::ProtectedCall(state,1,1)==LUA_OK,"actual forecast parser");
        Check(lua_istable(state,-1),"weather fixture is valid for actual parser");const auto forecast=luaL_ref(state,LUA_REGISTRYINDEX);
        lua_settop(state,0);Model();lua_getfield(state,-1,"weather");lua_rawgeti(state,LUA_REGISTRYINDEX,forecast);lua_setfield(state,-2,"weather");lua_settop(state,0);
        luaL_unref(state,LUA_REGISTRYINDEX,decoded);luaL_unref(state,LUA_REGISTRYINDEX,forecast);
    }
};

std::shared_ptr<SharedStorage> Storage(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);auto storage=std::make_shared<SharedStorage>();storage->path=root/L"storage.json";return storage;
}

void TestQuery(const std::filesystem::path& root)
{
    CalendarService service(root/L"calendar.json",[]{return snowdesktop::calendar::CalendarNow{"2026-10-16",0};});
    Check(service.Load(),"isolated real calendar loads");std::string target;
    for (int index=0;index<600;++index)
    {
        CalendarEvent event;event.date="2026-10-16";event.title=index==599?"ZZ Target [literal].*":"Same title";event.allDay=true;
        const auto result=service.Create(event);Check(result.ok,"actual calendar event is persisted");if (index==599) target=result.id;
    }
    const auto events=service.Events("2026-10-01","2026-10-31");
    Check(events.size()==600,"provider range really exceeds512 on one day");
    const auto filtered=QueryCalendarEvents(events,{"target [literal].*",0,40});
    Check(filtered.total==1 && filtered.events.front().id==target && !filtered.hasMore && filtered.nextOffset==1,"actual provider literal query precedes512 cap");
    std::set<std::string> ids;std::uint64_t offset=0;
    do
    {
        const auto page=QueryCalendarEvents(events,{"same TITLE",offset,40});
        Check(page.total==599 && page.events.size()<=40,"same-title pagination reports complete total");
        for (const auto& event:page.events) Check(ids.insert(event.id).second,"stable pages never duplicate an ID");
        offset=page.nextOffset;if (!page.hasMore) break;
    } while (true);
    Check(ids.size()==599 && offset==599,"all same-day matches remain reachable");
    const auto past=QueryCalendarEvents(events,{"",~std::uint64_t{0},512});
    Check(past.events.empty() && !past.hasMore && past.nextOffset==600,"oversized valid offset cannot overflow pagination");
    snowdesktop::calendar::CalendarSeries series;series.event.title="Boundary series";series.event.allDay=true;
    series.rule.kind="dates";series.rule.dates={"2026-01-01","2027-01-01"};
    Check(service.CreateSeries(series).ok,"actual series fixture persists both ends of closed366day range");
    const auto boundary=QueryCalendarEvents(service.Events("2026-01-01","2027-01-01"),{"Boundary series",0,40});
    Check(boundary.total==2,"real recurring provider includes start+365 before query pagination");
    series.rule.dates={"9999-12-31"};
    Check(service.CreateSeries(series).ok,"actual series fixture persists maximum calendar date");
    Check(QueryCalendarEvents(service.Events("9999-12-31","9999-12-31"),{"Boundary series",0,40}).total==1,
        "short maximum-year range expands actual recurring data without date overflow");
    auto* state=luaL_newstate();std::optional<CalendarEventQueryOptions> options;std::string error;
    lua_newtable(state);Check(snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error) && !options,"legacy parser remains absent");
    lua_pushliteral(state,"");lua_setfield(state,-2,"query");
    Check(snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error) && options && options->limit==512,"explicit empty query selects default pagination");
    for (const auto field:{"offset","limit"})
    { lua_pushinteger(state,-1);lua_setfield(state,-2,field);Check(!snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error) && lua_gettop(state)==1,"negative option rejected with balanced stack");lua_pushnil(state);lua_setfield(state,-2,field); }
    lua_pushliteral(state,"existing");lua_setfield(state,-2,"eventId");
    Check(!snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error),"eventId and pagination cannot be combined");lua_close(state);
    struct AllocationProbe
    {
        bool denyGrowth=false;
        static void* Allocate(void* opaque,void* pointer,std::size_t oldSize,std::size_t size)
        {
            if (!size) { std::free(pointer);return nullptr; }
            if (static_cast<AllocationProbe*>(opaque)->denyGrowth && (!pointer || size>oldSize)) return nullptr;
            return std::realloc(pointer,size);
        }
    } allocation;
    state=lua_newstate(AllocationProbe::Allocate,&allocation);Check(state!=nullptr,"real failing-allocator parser VM starts");
    lua_newtable(state);lua_pushliteral(state,"Needle");lua_setfield(state,-2,"query");Check(lua_checkstack(state,16)!=0,"reserve raw parser stack before allocator failure");
    allocation.denyGrowth=true;
    Check(!snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error) && lua_gettop(state)==1 && !error.empty(),
        "actual Lua allocation failure in argument construction remains contained and preserves caller stack");
    allocation.denyGrowth=false;
    Check(snowdesktop::widget_api::ParseCalendarEventQueryOptions(state,-1,options,error) && options && options->query=="Needle",
        "same Lua state and unchanged input recover after allocator availability returns");lua_close(state);
}

void TestCountdown(const std::filesystem::path& packages,const std::filesystem::path& root)
{
    CalendarService service(root/L"calendar.json",[]{return snowdesktop::calendar::CalendarNow{"2026-10-16",0};});Check(service.Load(),"countdown calendar loads");
    std::string target;
    for (int index=0;index<600;++index)
    { CalendarEvent event;event.date="2026-10-16";event.title=index==599?"ZZ Recoverable":"Repeated title";event.allDay=true;const auto saved=service.Create(event);Check(saved.ok,"countdown persisted fixture");if(index==599)target=saved.id; }
    auto shared=Storage(root);Host host(packages/L"days-countdown",shared,&service);
    host.Act("choose");host.Act("filter","recoverable");
    Check(host.Panel().contains("bind:"+target),"actual countdown main finds and renders event beyond original512");
    host.Act(("bind:"+target).c_str());Check(shared->Get("eventId").string==target,"actual result action binds the retrieved ID");
    host.Act("choose");host.Act("filter","Repeated title");std::set<std::string> seen;
    for (int page=0;page<15;++page)
    {
        const auto keys=host.Panel();for (const auto& key:keys) if (key.starts_with("bind:")) seen.insert(key);
        host.Act("page.next");
    }
    Check(seen.size()==599,"actual component page actions retrieve all repeated same-day events");
    host.permission=false;Check(host.Panel().contains("permission"),"permission revoke removes selectable results");host.permission=true;
    host.available=false;Check(host.Panel().contains("unavailable"),"unavailable provider is not an empty successful query");host.available=true;
    host.Act("filter","not present");Check(host.Panel().contains("no.events"),"no-match query has actual empty-state node");
    Host legacy(packages/L"days-countdown",Storage(root/L"legacy"),&service,false);legacy.Act("choose");
    Check(legacy.Panel().contains("truncated"),"older host keeps a truthful truncation hint");
    for (const auto& [id,subscription]:legacy.subscriptions) { (void)id;Check(!subscription.query,"legacy fallback sends no unsupported query options"); }
    legacy.Act("filter","recoverable");Check(!legacy.Panel().contains("bind:"+target),"legacy fallback does not claim to recover omitted provider records");
}

void TestWeather(const std::filesystem::path& packages,const std::filesystem::path& root)
{
    Host host(packages/L"sky-weather",Storage(root),nullptr,true,true);
    const auto forecast=[](const std::string& current,const std::string& dates,const std::string& highs,const std::string& lows)
    { return "{\"current\":{\"temperature_2m\":21,\"apparent_temperature\":20,\"weather_code\":0,\"time\":\""+current+"T14:30\"},\"daily\":{\"time\":"+dates+",\"temperature_2m_max\":"+highs+",\"temperature_2m_min\":"+lows+"}}"; };
    host.Forecast(forecast("2026-10-02","[\"2026-10-02\",\"2026-10-03\",\"2026-10-04\"]","[23,null,18]","[11,12,10]"));host.Render();
    Check(std::find(host.translated.begin(),host.translated.end(),"lua_widget.sky_weather.cooler")==host.translated.end(),"actual render never calls +2days tomorrow");
    Check(std::find(host.drawn.begin(),host.drawn.end(),"23° / 11°")!=host.drawn.end(),"today range belongs to exact current date");
    host.Forecast(forecast("2026-10-03","[\"2026-10-04\",\"2026-10-05\"]","[20,15]","[11,10]"));host.Render();
    Check(std::find(host.drawn.begin(),host.drawn.end(),"— / —")!=host.drawn.end(),"missing today does not borrow tomorrow high/low");
    for (const auto& [today,tomorrow]:{std::pair{"2024-02-28","2024-02-29"},{"2026-12-31","2027-01-01"}})
    {
        host.Forecast(forecast(today,"[\""+std::string(today)+"\",\""+tomorrow+"\"]","[23,18]","[11,10]"));host.Render();
        Check(std::find(host.translated.begin(),host.translated.end(),"lua_widget.sky_weather.cooler")!=host.translated.end(),"real tomorrow across leap/year boundary still shows cooling");
    }
    host.storage->Seed("unit",String("f"));host.Render();
    Check(std::find(host.drawn.begin(),host.drawn.end(),"lua_widget.sky_weather.cooler:9°")!=host.drawn.end(),"Fahrenheit cooling uses a9degree delta, without an absolute32degree offset");

    Check(host.lazyImages.size()==6 && std::all_of(host.lazyImages.begin(),host.lazyImages.end(),
        [](const auto& image){return image.second;}),"new hosts register weather scenes as lazy resources");
    const auto sunny=forecast("2026-10-26","[\"2026-10-26\"]","[23]","[11]");
    host.Forecast(sunny);host.Background();
    Check(host.drawnImages==std::vector<std::string>{"sky"},"sunny weather requests only its selected background");
    auto rainy=sunny;const auto code=rainy.find("\"weather_code\":0");
    Check(code!=std::string::npos,"rain fixture changes the actual weather code");
    rainy.replace(code,std::string("\"weather_code\":0").size(),"\"weather_code\":61");
    host.pendingImages.insert("rain");host.Forecast(rainy);host.Background();
    Check(host.drawnImages==std::vector<std::string>({"rain","sky"}),
        "a pending replacement keeps the last ready scene visible");
    host.pendingImages.clear();host.Background();
    Check(host.drawnImages==std::vector<std::string>{"rain"},"ready rain retires the sunny scene demand");
    Host legacy(packages/L"sky-weather",Storage(root/L"legacy-images"),nullptr,true,true,false);
    Check(legacy.lazyImages.size()==6 && std::none_of(legacy.lazyImages.begin(),legacy.lazyImages.end(),
        [](const auto& image){return image.second;}),"old hosts retain the original image API without new options");
    legacy.Forecast(sunny);legacy.Background();
    Check(legacy.drawnImages==std::vector<std::string>{"sky"},"the legacy fallback still draws its weather scene");
}

void TestTimer(const std::filesystem::path& packages,const std::filesystem::path& root)
{
    for (const auto* draft:{"1500","5999:59"})
    {
        auto shared=Storage(root/std::filesystem::path(std::string(draft).find(':')==std::string::npos?"25h":"maximum"));
        Host host(packages/L"timer-stopwatch",shared);host.Act("custom");host.Act("duration",draft);host.Act("custom.start");
        const long long duration=std::string_view(draft)=="1500"?90000000:359999000;
        Check(host.Boolean("running","countdown") && host.closes==1,"actual >24h start succeeds and closes panel");
        host.timers.SetVisible(false,Clock::time_point{std::chrono::milliseconds(host.now)});
        long long remaining=duration;
        while (remaining>0) { const auto part=std::min(remaining,86400000LL);host.Advance(part);remaining-=part; }
        Check(host.notifications==1 && host.Boolean("done","countdown") && !host.Boolean("running","countdown"),"hidden segmented countdown completes exactly once at original deadline");
        host.Advance(1000);Check(host.notifications==1,"later callbacks cannot duplicate completion");
        for (const auto& [name,delay]:host.scheduled) Check(name!="deadline" || (delay>=1 && delay<=86400000),"all actual deadline registrations obey host24h bound");
    }
    Host host(packages/L"timer-stopwatch",Storage(root/L"failure"));host.Act("custom");host.Act("duration","1500");host.failSchedule="deadline";
    host.Act("custom.start");Check(!host.Boolean("running","countdown") && host.closes==0,"failed actual registration preserves previous model and open input");
    host.Act("custom.start");Check(host.Boolean("running","countdown"),"same retained duration can be retried");
    host.Advance(1000);host.Act("toggle");const auto paused=host.Number("value","countdown");host.Advance(10000);host.Act("toggle");
    Check(host.Boolean("running","countdown") && host.Number("value","countdown")==paused,"pause time is excluded on resume");
    host.Act("reset");Check(!host.Boolean("running","countdown") && host.Number("value","countdown")==0,"reset cancels long countdown");
    Host interrupted(packages/L"timer-stopwatch",Storage(root/L"interrupted"));
    interrupted.Act("custom");interrupted.Act("duration","1500");interrupted.Act("custom.start");interrupted.failSchedule="deadline";
    interrupted.timers.SetVisible(false,Clock::time_point{std::chrono::milliseconds(interrupted.now)});interrupted.Advance(86400000);
    Check(!interrupted.Boolean("running","countdown") && interrupted.Number("value","countdown")==3600000 && interrupted.notifications==0 && interrupted.opens==2,
        "a failed intermediate wakeup pauses with the actual remaining hour and visible retry feedback");
    interrupted.Act("toggle");interrupted.Advance(3600000);
    Check(interrupted.notifications==1,"retry after intermediate failure completes retained progress");
}

void TestFish(const std::filesystem::path& packages,const std::filesystem::path& root)
{
    auto shared=Storage(root);shared->Seed("count",Integer(0));shared->Seed("todayCount",Integer(0));shared->Seed("todayDate",String("2026-10-16"));
    Host old(packages/L"wooden-fish",shared);old.Act("wooden-fish.strike");
    Check(shared->Get("count").integer==0,"real350ms batching remains before flush");
    Host replacement(packages/L"wooden-fish",shared);old.Dispose();replacement.Render();
    Check(replacement.Number("count")==1 && replacement.Number("todayCount")==1,"new actual model adopts old dispose flush before display");
    replacement.Act("wooden-fish.strike");replacement.Advance(350);
    Check(replacement.Number("count")==2 && shared->Get("count").integer==2,"next actual strike becomes2 rather than overwriting1");
    const auto writes=shared->writes;
    for (int index=0;index<50;++index) replacement.Act("wooden-fish.strike");
    Check(shared->writes==writes && replacement.Number("count")==52,"fast strikes keep local deltas and do not exhaust real write budget");
    replacement.Advance(350);Check(shared->Get("count").integer==52,"batched burst persists all strikes");
    { Host alreadySaved(packages/L"wooden-fish",shared);alreadySaved.Render();Check(alreadySaved.Number("count")==52,"already-saved reload preserves existing total");alreadySaved.Dispose(); }
    replacement.Act("wooden-fish.reset");replacement.Render();Check(replacement.Number("count")==0 && shared->Get("count").integer==0,"explicit reset never max-merges old counts back");
    replacement.date="2026-10-17";replacement.Act("wooden-fish.strike");replacement.Advance(350);
    Check(shared->Get("count").integer==1 && shared->Get("todayCount").integer==1 && shared->Get("todayDate").string=="2026-10-17","day rollover retains total and starts new day correctly");
    replacement.Act("wooden-fish.strike");
    const auto lock=CreateFileW(shared->path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    Check(lock!=INVALID_HANDLE_VALUE,"actual storage failure fixture holds replacement lock");
    const bool accepted=replacement.Event("schedule","wooden-fish.persist");CloseHandle(lock);
    Check(!accepted && replacement.Number("count")==2 && replacement.Boolean("countsDirty") && shared->Get("count").integer==1,"failed real save retains dirty unflushed counts");
    Check(replacement.Event("schedule","wooden-fish.persist") && shared->Get("count").integer==2,"unlocked retry persists retained count");
}
}

int main(int argc,char** argv)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    if (argc!=2) { std::cerr<<"source root argument required\n";return 2; }
    const auto source=std::filesystem::absolute(argv[1]);const auto packages=source/L"developer_assets/workshop_widgets";
    wchar_t temporary[MAX_PATH]{};if (!GetTempPathW(MAX_PATH,temporary)) return 2;
    const auto rootTag=L"SnowDesktopCommunityRuntime-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    const auto root=std::filesystem::path(temporary)/rootTag;
    std::filesystem::create_directories(root);int failed=0;
    for (const auto& [name,test]:std::initializer_list<std::pair<const char*,std::function<void()>>>{
        {"query",[&]{TestQuery(root/L"query");}}, {"countdown",[&]{TestCountdown(packages,root/L"countdown");}},
        {"weather",[&]{TestWeather(packages,root/L"weather");}}, {"timer",[&]{TestTimer(packages,root/L"timer");}},
        {"fish",[&]{TestFish(packages,root/L"fish");}}})
    { try { std::filesystem::create_directories(root/name);test();std::cout<<name<<" passed\n"; } catch(const std::exception& error) { ++failed;std::cerr<<"FAILED "<<name<<": "<<error.what()<<'\n'; } }
    const auto absolute=std::filesystem::absolute(root).lexically_normal();
    if (std::filesystem::equivalent(absolute.parent_path(),std::filesystem::weakly_canonical(temporary)) &&
        absolute.filename()==rootTag &&
        (GetFileAttributesW(absolute.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)==0)
    { std::error_code error;std::filesystem::remove_all(absolute,error);if (error) { ++failed;std::cerr<<"owned temporary cleanup failed\n"; } }
    else { ++failed;std::cerr<<"refusing cleanup outside exact owned temporary root\n"; }
    return failed?1:0;
}
