#include "widget/data/widget_data_broker.h"
#include "widget/data/widget_audio_analysis_provider.h"
#include "widget/api/widget_calendar_query_options.h"
#include "system/calendar/calendar_service.h"

extern "C" {
#include <lauxlib.h>
#include <lualib.h>
}
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>

// Count native owners while the actual Lua C callback runs. Lua's allocator
// uses malloc directly; retained broker state is checked separately below.
namespace allocation_probe {
thread_local bool enabled=false;
std::atomic<long long> live=0;
struct Header { std::size_t counted; std::size_t reserved; };
}
void* operator new(std::size_t size)
{
    auto* header=static_cast<allocation_probe::Header*>(std::malloc(sizeof(allocation_probe::Header)+(size?size:1)));
    if(!header)throw std::bad_alloc();
    header->counted=allocation_probe::enabled;
    if(header->counted)++allocation_probe::live;
    return header+1;
}
void operator delete(void* pointer) noexcept
{
    if(!pointer)return;
    auto* header=static_cast<allocation_probe::Header*>(pointer)-1;
    if(header->counted)--allocation_probe::live;
    std::free(header);
}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete[](void* pointer) noexcept{::operator delete(pointer);}
void operator delete(void* pointer,std::size_t) noexcept{::operator delete(pointer);}
void operator delete[](void* pointer,std::size_t) noexcept{::operator delete(pointer);}

namespace {
using namespace snowdesktop::widget_runtime;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Host
{
    WidgetDataBroker broker;
    std::string lastHandle;
    DataSubscriptionResult RuntimeSubscribeData(const std::wstring&,
        const std::string& topic,std::chrono::milliseconds age,DataHiddenPolicy policy,
        std::string start,std::string end,std::string handle,
        WidgetAudioAnalysisConfiguration audio,std::string eventId,
        std::optional<CalendarEventQueryOptions> query)
    {
        DataSubscriptionOptions options;
        options.requestedInterval=age;options.whenHidden=policy;options.permissionGranted=true;
        options.rangeStart=std::move(start);options.rangeEnd=std::move(end);
        options.eventId=std::move(eventId);options.calendarQuery=std::move(query);
        options.audioWaveform=audio.waveform;options.audioSpectrum=audio.spectrum;
        options.audioRms=audio.rms;options.audioPeak=audio.peak;
        options.audioWaveformPoints=audio.waveformPoints;options.audioSpectrumBins=audio.spectrumBins;
        lastHandle=std::move(handle);
        return broker.Subscribe("callback-instance",topic,std::move(options),WidgetDataBroker::TimePoint{});
    }
};
struct D2D { Host* engine; } current{};
D2D* GetD2D(lua_State*){return &current;}
std::wstring BoundWidgetId(lua_State*){return L"callback-instance";}
struct LuaDataSubscriptionHandle { Host* engine=nullptr;std::uint64_t id=0;bool includeGpuDetails=false; };
constexpr char kDataSubscriptionHandleMetatable[]="callback-subscription";

// This is the complete production callback, not a copy of its parser/guard.
#include "widget/runtime/widget_data_subscribe_lua.inc"

struct Fixture
{
    Host host;
    lua_State* state=nullptr;
    bool rejectUserdata=false;
    int rejectedUserdata=0;
    Fixture()
    {
        std::string error;
        for(const char* topic:{"calendar.events","audio.output.analysis","filesystem.watch","system.cpu","system.gpu"})
            Require(host.broker.RegisterProvider({topic,"",std::chrono::milliseconds(1),std::chrono::milliseconds(1000),std::chrono::milliseconds(0),false,true},error),"register actual callback broker providers");
        current.engine=&host;
        state=lua_newstate(Allocate,this);Require(state!=nullptr,"create callback Lua state");luaL_openlibs(state);
        luaL_newmetatable(state,kDataSubscriptionHandleMetatable);
        lua_pushcfunction(state,[](lua_State* inner){
            auto* handle=static_cast<LuaDataSubscriptionHandle*>(lua_touserdata(inner,1));
            if(handle&&handle->engine&&handle->id)
                handle->engine->broker.Unsubscribe(handle->id,WidgetDataBroker::TimePoint{});
            if(handle)handle->id=0;
            return 0;
        });lua_setfield(state,-2,"__gc");lua_pop(state,1);
        Require(lua_checkstack(state,128)!=0,"reserve callback driver stack");
    }
    ~Fixture(){if(state)lua_close(state);current.engine=nullptr;}
    static void* Allocate(void* context,void* pointer,std::size_t oldSize,std::size_t newSize)
    {
        auto& self=*static_cast<Fixture*>(context);
        if(!newSize){std::free(pointer);return nullptr;}
        if(self.rejectUserdata&&!pointer&&oldSize==LUA_TUSERDATA)
        {++self.rejectedUserdata;return nullptr;}
        return std::realloc(pointer,newSize);
    }
    int Invoke(const char* topic,const std::string& options,bool track=false)
    {
        lua_settop(state,0);lua_pushcfunction(state,lua_DataSubscribe);lua_pushstring(state,topic);
        const auto expression="return "+options;
        Require(luaL_loadstring(state,expression.c_str())==LUA_OK,"compile callback options");
        Require(lua_pcall(state,0,1,0)==LUA_OK,"construct callback options before native tracking");
        allocation_probe::enabled=track;
        const int status=lua_pcall(state,2,1,0);
        allocation_probe::enabled=false;
        return status;
    }
    const DataSubscriptionSnapshot Snapshot()
    {
        auto* handle=static_cast<LuaDataSubscriptionHandle*>(luaL_testudata(state,-1,kDataSubscriptionHandleMetatable));
        Require(handle&&handle->id,"actual callback returned a live subscription handle");
        const auto snapshot=host.broker.SubscriptionSnapshot(handle->id);
        Require(snapshot.has_value(),"returned ID belongs to actual broker");return *snapshot;
    }
    void Collect(){lua_settop(state,0);lua_gc(state,LUA_GCCOLLECT);}
};
void TestNormalRequests()
{
    Fixture vm;
    Require(vm.Invoke("calendar.events","{fromDate='2026-01-01',toDate='2027-01-01',query='Needle',offset=512,limit=40}")==LUA_OK,"actual calendar callback accepts full 366-day range and query");
    auto calendar=vm.Snapshot();
    Require(calendar.options.calendarQuery&&calendar.options.calendarQuery->query=="Needle"&&calendar.options.calendarQuery->offset==512&&calendar.options.calendarQuery->limit==40,"actual callback forwards query into real broker");vm.Collect();
    Require(vm.Invoke("calendar.events","{fromDate='9999-12-01',toDate='9999-12-31'}")==LUA_OK,"actual callback accepts valid upper-year range when +365 overflows");
    Require(!vm.Snapshot().options.calendarQuery,"legacy request keeps its original mode");vm.Collect();
    Require(vm.Invoke("audio.output.analysis","{features={'waveform','rms'},waveformPoints=32,updateHz=20}")==LUA_OK,"actual audio callback accepts feature selection");
    auto audio=vm.Snapshot();Require(audio.options.audioWaveform&&audio.options.audioRms&&!audio.options.audioSpectrum&&!audio.options.audioPeak&&audio.options.audioWaveformPoints==32&&audio.options.requestedInterval.count()==50,"audio parsing and effective interval survive protected bridge");vm.Collect();
    Require(vm.Invoke("filesystem.watch","{handle='filesystem:test',whenHidden='continue'}")==LUA_OK,"actual filesystem callback preserves scope and policy");
    Require(vm.Snapshot().options.whenHidden==DataHiddenPolicy::Continue&&vm.host.lastHandle=="filesystem:test","filesystem scope reaches host boundary");vm.Collect();
    Require(vm.host.broker.SubscriptionCount()==0,"normal callback handles release actual broker subscriptions");
}
void TestErrorsAndNativeOwnership()
{
    Fixture vm;
    const std::vector<std::pair<const char*,std::string>> requests={
        {"calendar.events","{query=string.rep('q',512),offset=-1}"},
        {"calendar.events","{query='x',eventId='event'}"},
        {"calendar.events","{fromDate='2026-01-01',toDate='2027-01-02'}"},
        {"calendar.events","setmetatable({query='x'},{__index=function()error('hostile options')end})"},
        {"audio.output.analysis","{features={'waveform',string.rep('z',256)}}"},
        {"audio.output.analysis","{features={'spectrum','spectrum'}}"},
        {"filesystem.watch","{}"},
        {"system.cpu","{query='invalid topic'}"}
    };
    for(const auto& [topic,options]:requests)
    {
        const auto before=allocation_probe::live.load();
        Require(vm.Invoke(topic,options,true)!=LUA_OK,"invalid actual callback request must report a Lua error");
        Require(allocation_probe::live.load()==before,"actual callback must destroy all native owners before raising a Lua error");
        Require(vm.host.broker.SubscriptionCount()==0,"invalid callback request never registers a native subscription");
        vm.Collect();
        Require(vm.Invoke("calendar.events","{query='recovered'}")==LUA_OK,"same VM remains usable after the actual callback error");vm.Collect();
    }
}
void TestActualUserdataAllocationFailure()
{
    Fixture vm;vm.rejectUserdata=true;
    const auto before=allocation_probe::live.load();
    const int status=vm.Invoke("calendar.events","{query=string.rep('q',512)}",true);
    std::cout<<"Observed userdata denial="<<vm.rejectedUserdata<<", Lua status="<<status
        <<", broker subscriptions="<<vm.host.broker.SubscriptionCount()<<'\n';
    Require(status==LUA_ERRMEM,"actual userdata allocator failure must propagate as Lua memory error");
    Require(vm.rejectedUserdata>0,"failure injection reached real Lua userdata allocation");
    Require(vm.host.broker.SubscriptionCount()==0,"userdata allocation failure must not leak a broker subscription");
    Require(allocation_probe::live.load()==before,"userdata allocation failure must release the full actual callback context");
    vm.rejectUserdata=false;vm.Collect();
    Require(vm.Invoke("calendar.events","{query='after-oom'}")==LUA_OK,"same VM can subscribe after allocator recovery");
    Require(vm.Snapshot().options.calendarQuery->query=="after-oom","recovered callback still forwards complete query options");vm.Collect();
}
}
int main(int argc,char** argv)
{
    std::cout<<std::unitbuf;std::cerr<<std::unitbuf;
    int failed=0;
    const auto run=[&](const char* name,auto test){
        std::cout<<"RUN actual data.subscribe "<<name<<'\n';
        try{test();std::cout<<"PASS actual data.subscribe "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"FAIL actual data.subscribe "<<name<<": "<<error.what()<<'\n';}
    };
    const std::string selected=argc>1?argv[1]:"all";
    if(selected=="all"||selected=="normal")run("calendar/audio/filesystem requests",TestNormalRequests);
    if(selected=="all"||selected=="errors")run("invalid requests, native ownership and same-VM recovery",TestErrorsAndNativeOwnership);
    if(selected=="all"||selected=="oom")run("userdata allocator failure, broker ownership and same-VM recovery",TestActualUserdataAllocationFailure);
    if(selected!="all"&&selected!="normal"&&selected!="errors"&&selected!="oom")return 2;
    return failed?1:0;
}
