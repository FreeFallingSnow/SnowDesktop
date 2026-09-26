#include "system_panel_model.h"
#include "system_control_wifi_presentation.h"
#include "widget_gpu_presentation.h"
#include "status_bar_presentation.h"
#include "tray_order.h"
#include "l10n.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace snowdesktop
{
namespace j = system_control::json;
namespace ui = native_ui;
namespace wr = widget_runtime;
namespace
{
std::wstring Wide(std::string_view s)
{
    if(s.empty()) return {};
    const auto size=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring result(size,L'\0'); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),result.data(),size); return result;
}
const std::vector<JsonValue>& Items(const JsonValue& v,const char* field)
{ static const std::vector<JsonValue> empty;const auto* p=v.Find(field);return p&&p->IsArray()?p->array:empty; }
std::wstring Percent(double v) { return std::isfinite(v)&&v>=0&&v<=100?std::to_wstring(static_cast<int>(std::lround(v)))+L"%":L"—"; }
double Number(const JsonValue& value, std::string_view field)
{ return j::Numeric(value,field,std::numeric_limits<double>::quiet_NaN()); }
bool InRange(double value,double maximum=1)
{ return std::isfinite(value)&&value>=0&&value<=maximum; }
std::wstring Bytes(std::uint64_t v)
{ constexpr const wchar_t* units[]{L"B",L"KiB",L"MiB",L"GiB",L"TiB"}; double d=static_cast<double>(v);int i=0;while(d>=1024&&i<4){d/=1024;++i;}wchar_t s[80]{};swprintf_s(s,L"%.1f %s",d,units[i]);return s; }
const char* Topic(StatusBarAction a)
{ return a==StatusBarAction::Cpu?"system.cpu":a==StatusBarAction::Memory?"system.memory":a==StatusBarAction::Gpu?"system.gpu":"system.network.traffic"; }
D2D1_RECT_F Rect(float x,float y,float w,float h) { return {x,y,x+w,y+h}; }
std::string Date(int year,int month,int day)
{ char s[16]{};sprintf_s(s,"%04d-%02d-%02d",year,month,day);return s; }
}
bool IsSystemResourceAction(StatusBarAction a)
{ return a==StatusBarAction::Cpu||a==StatusBarAction::Memory||a==StatusBarAction::Gpu||a==StatusBarAction::Traffic; }
ui::Palette SystemPanelPalette(const PersonalizationSettings& appearance, bool highContrast)
{
    if(highContrast)
    {
        const auto color=[](int index){const auto c=GetSysColor(index);return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);};
        return {color(COLOR_WINDOWTEXT),color(COLOR_WINDOWTEXT),color(COLOR_HIGHLIGHT),color(COLOR_HIGHLIGHTTEXT),color(COLOR_BTNFACE),color(COLOR_WINDOW),color(COLOR_WINDOWTEXT)};
    }
    const bool light=appearance.contentTheme==1;
    return {D2D1::ColorF(light?0x202020:0xffffff),D2D1::ColorF(light?0x626262:0xceced2),D2D1::ColorF(0x4cc2ff),D2D1::ColorF(0x001c2b),D2D1::ColorF(light?0:0xffffff,light?.10f:.13f),D2D1::ColorF(0xffffff,light?.46f:.065f),D2D1::ColorF(light?0:0xffffff,.20f)};
}
SystemPanelSource LiveSystemPanelSource(std::shared_ptr<wr::WidgetSystemDataProvider> data)
{
    SystemPanelSource s;auto service=data->Controls();
    s.current=[service](auto topic){return service->Current(topic);};
    s.start=[service](auto request){return service->Start("nativeSystemPanel",std::move(request));};
    s.completions=[service]{return service->DrainCompletions("nativeSystemPanel");};
    s.subscribe=[data](auto topic,auto period){data->StartTopic("nativeSystemPanel",topic,period);};
    s.unsubscribe=[data](auto topic){data->StopTopic("nativeSystemPanel",topic);};
    s.close=[data,service]{data->RemoveConsumer("nativeSystemPanel");service->RemoveConsumer("nativeSystemPanel");};
    s.settings=[](const wchar_t* uri){ShellExecuteW(nullptr,L"open",uri,nullptr,nullptr,SW_SHOWNORMAL);};
    s.media=[data]{return data->MediaSessions();};s.artwork=[data]{return data->MediaArtwork();};
    s.cpu=[data]{return data->Cpu();};s.memory=[data]{return data->Memory();};s.gpu=[data]{return data->Gpu(true);};s.traffic=[data]{return data->NetworkTraffic();};
    s.history=[data](auto topic,auto id){return data->ResourceHistory(topic,id);};
    s.now=[]{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();};return s;
}
SystemPanelModel::SystemPanelModel(SystemPanelSource source,StatusBarSettings settings,StatusBarAction action)
    :source_(std::move(source)),settings_(std::move(settings)),action_(action)
{
    date_=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(date_))date_=calendar::CalendarService::CurrentLocalNow().date;
    month_=date_.substr(0,7)+"-01";
    Refresh();
}
SystemPanelModel::~SystemPanelModel(){Close();}
void SystemPanelModel::Close(){if(closed_)return;closed_=true;actions_.clear();feedback_.Clear();pendingValues_.clear();sliderTargets_.clear();valueControls_.clear();subscriptions_.clear();if(source_.close)source_.close();source_={};}
void SystemPanelModel::SyncSubscriptions()
{
    std::set<std::string> needed;
    if(IsSystemResourceAction(action_))needed.insert(Topic(action_));
    else if(action_!=StatusBarAction::Tray&&action_!=StatusBarAction::Calendar)
    {
        if(settings_.mediaControls){needed.insert("media.sessions");needed.insert("media.artwork");}
        if(settings_.wifiControls&&(page_.empty()||page_.starts_with("wifi")))needed.insert("network.wifi");
        if(settings_.bluetoothControls&&(page_.empty()||page_=="bluetooth"))needed.insert("bluetooth.devices");
        if(settings_.audioControls&&(page_.empty()||page_=="audio"))needed.insert("audio.output.volume");
        if(settings_.audioControls&&page_=="audio"){needed.insert("audio.devices");needed.insert("audio.input.volume");}
        if(settings_.brightnessControls&&(page_.empty()||page_=="brightness"))needed.insert("system.display.brightness");
        if(settings_.powerControls&&(page_.empty()||page_=="power"))needed.insert("system.power.plans");
    }
    for(const auto& topic:subscriptions_)if(!needed.contains(topic)&&source_.unsubscribe)source_.unsubscribe(topic);
    for(const auto& topic:needed)if(!subscriptions_.contains(topic)&&source_.subscribe)
        source_.subscribe(topic,std::chrono::milliseconds(1000));
    subscriptions_=std::move(needed);
}
JsonValue SystemPanelModel::Current(const char* topic)const
{ if(source_.current)if(auto s=source_.current(topic);s&&s->available)return s->value;return j::Object(); }
JsonValue SystemPanelModel::Wifi()const
{ const auto v=Current("network.wifi");for(const auto& a:Items(v,"interfaces"))if(j::String(a,"id")==interface_)return a;return j::Object(); }
void SystemPanelModel::Start(std::string task,system_control::Arguments args)
{
    if(closed_||!source_.start)return;
    system_control::Request request;request.name=std::move(task);request.arguments=std::move(args);
    const bool hiddenNetwork = request.name == "network.wifi.connect" && request.arguments.contains("hidden");
    if((hiddenNetwork||system_control::RequiresConfirmation(request.name)||system_control::RequiresPasswordPrompt(request))&&(!source_.prompt||!source_.prompt(request)))return;
    const auto key=system_control::ControlFeedback::Key(request);const auto id=source_.start(std::move(request));feedback_.Track(key,id);lastStarted_=id;
    error_=id?L"":_LW("controlCenter.failed");
}
ui::Node& SystemPanelModel::Add(std::string id,ui::Role role,D2D1_RECT_F rect,std::wstring text,std::wstring glyph)
{ ui::Node node;node.id=std::move(id);node.role=role;node.bounds=rect;node.text=std::move(text);node.glyph=std::move(glyph);node.tooltip=node.text;scene_.nodes.push_back(std::move(node));return scene_.nodes.back(); }
void SystemPanelModel::Command(std::string id,std::function<void()> fn)
{ actions_[std::move(id)]=[fn=std::move(fn)](auto){fn();}; }
bool SystemPanelModel::Invoke(std::string_view id,std::optional<float> value)
{
    if(closed_)return false;
    const std::string key(id);const auto* node=scene_.Find(key);
    if(!node||!node->Interactive()||(value&&(node->role!=ui::Role::Slider||!std::isfinite(*value))))return false;
    if(value)*value=std::clamp(*value,0.f,1.f);
    const auto it=actions_.find(key);if(it==actions_.end())return false;
    const auto target=sliderTargets_.contains(key)?sliderTargets_.at(key):std::string{};
    const auto fn=it->second;lastStarted_=0;fn(value);
    if(value){if(lastStarted_)pendingValues_[key]={lastStarted_,*value,target};else pendingValues_.erase(key);}
    Refresh(available_);return true;
}
void SystemPanelModel::Select(std::string page)
{ if(closed_)return;page_=std::move(page);scroll_=0;scan_=page_=="wifi";error_.clear();Refresh(available_); }
void SystemPanelModel::Scroll(float delta){if(closed_||!std::isfinite(delta))return;scroll_=std::clamp(scroll_+delta,0.f,maxScroll_);Refresh(available_);}
void SystemPanelModel::UpdateSettings(const StatusBarSettings& settings)
{
    if(closed_||(settings_.trayOrder==settings.trayOrder&&settings_.pinnedTrayItems==settings.pinnedTrayItems))return;
    settings_.trayOrder=settings.trayOrder;settings_.pinnedTrayItems=settings.pinnedTrayItems;
    Refresh(available_);
}
void SystemPanelModel::Header(std::wstring title)
{
    Add("back",ui::Role::Icon,Rect(12,10,36,36),L"",L"\uE76B").tooltip=_LW("controlCenter.overview");Command("back",[this]{Select(page_=="wifi-adapters"?"wifi":"");});
    auto& node=Add("title",ui::Role::Text,Rect(58,10,scene_.width-126,36),std::move(title));node.bold=true;node.fontSize=17;
    bodyStart_=56;
}
void SystemPanelModel::Footer(const wchar_t* uri,float& y)
{
    Add("footer.settings",ui::Role::Button,Rect(16,y,scene_.width-32,36),_LW("settings.taskbar.systemSettings.open"),L"\uE713");
    const auto target=std::wstring(uri);Command("footer.settings",[this,target]{if(source_.settings)source_.settings(target.c_str());});y+=48;
}
void SystemPanelModel::Radio(std::string_view key,D2D1_RECT_F rect,bool compact)
{
    const bool wifi=key=="wifi";const auto v=Current(wifi?"network.wifi":"bluetooth.devices");
    const auto& radios=Items(v,wifi?"interfaces":"radios");const JsonValue* radio=nullptr;
    for(const auto& r:radios)if(!wifi||interface_.empty()||j::String(r,"id")==interface_)
    {if(!radio)radio=&r;if(j::Flag(r,"available")){radio=&r;break;}}
    const bool available=radio&&j::Flag(*radio,"available"),on=available&&j::Flag(*radio,"enabled");
    std::wstring label=_LW(wifi?"statusBar.wifiControls":"statusBar.bluetoothControls");
    if(wifi&&radio)for(const auto& n:Items(*radio,"networks"))if(j::Flag(n,"connected")){label=Wide(j::String(n,"ssid"));break;}
    if(!wifi)for(const auto& d:Items(v,"devices"))if(j::Flag(d,"connected")){label=Wide(j::String(d,"name"));break;}
    const auto id="radio:"+std::string(key);auto& button=Add(id,ui::Role::Toggle,rect,compact?L"":label,wifi?L"\uE701":L"\uE702");
    button.enabled=available;button.selected=on;button.tooltip=label+L" · "+_LW(on?"controlCenter.on":"controlCenter.off");
    if(!compact)button.detail=_LW(!available?"controlCenter.unavailable":on?"controlCenter.on":"controlCenter.off");
    const auto radioId=radio?j::String(*radio,"id"):std::string{};
    Command(id,[this,wifi,radioId,on]{Start(wifi?"network.wifi.setRadio":"bluetooth.setRadio",{{wifi?"interfaceId":"radioId",radioId},{"enabled",on?"0":"1"}});});
}
void SystemPanelModel::Volume(std::string_view direction,float& y)
{
    const auto prefix="audio."+std::string(direction);const auto state=source_.current?source_.current(prefix+".volume"):std::nullopt;
    const auto endpoint=state?j::String(state->value,"endpointId"):std::string{};
    const bool valid=state&&state->available&&!endpoint.empty()&&InRange(Number(state->value,"volume")),muted=valid&&j::Flag(state->value,"muted");
    const float volume=valid?static_cast<float>(Number(state->value,"volume")):0;
    const auto label=std::wstring(_LW(direction=="input"?"controlCenter.input":"statusBar.volume"));
    Add(prefix+".label",ui::Role::Text,Rect(16,y,scene_.width-90,22),label).fontSize=12;
    Add(prefix+".value",ui::Role::Text,Rect(scene_.width-68,y,52,22),valid?Percent(volume*100):L"—").fontSize=12;y+=24;
    const wchar_t* speaker=muted?L"\uE74F":volume<=0?L"\uE992":volume<.34f?L"\uE993":volume<.67f?L"\uE994":L"\uE995";
    auto& mute=Add(prefix+".mute",ui::Role::Icon,Rect(12,y,40,40),L"",direction=="input"?(muted?L"\uF781":L"\uE720"):speaker);
    mute.enabled=valid;mute.tooltip=label+L" · "+_LW(muted?"controlCenter.unmute":"controlCenter.mute");
    Command(prefix+".mute",[this,prefix,endpoint]{const auto s=source_.current?source_.current(prefix+".volume"):std::nullopt;if(s&&s->available&&j::String(s->value,"endpointId")==endpoint&&InRange(Number(s->value,"volume")))Start(prefix+".setMute",{{"muted",j::Flag(s->value,"muted")?"0":"1"}});});
    const auto sliderId=prefix+".volume:"+endpoint;
    auto& slider=Add(sliderId,ui::Role::Slider,Rect(58,y,scene_.width-(page_.empty()?116:74),40));slider.enabled=valid;slider.value=volume;slider.tooltip=label;
    sliderTargets_[sliderId]=valid?endpoint:std::string{};valueControls_[prefix+".value"]=sliderId;
    actions_[sliderId]=[this,prefix,endpoint](auto value){const auto s=source_.current?source_.current(prefix+".volume"):std::nullopt;if(value&&s&&s->available&&j::String(s->value,"endpointId")==endpoint&&InRange(Number(s->value,"volume")))Start(prefix+".setVolume",{{"volume",std::to_string(*value)}});};
    if(page_.empty()){Add("audio.more",ui::Role::Icon,Rect(scene_.width-52,y,36,40),L"",L"\uE76C").tooltip=_LW("statusBar.audioControls");Command("audio.more",[this]{Select("audio");});}y+=50;
}
void SystemPanelModel::Overview(float& y)
{
    std::vector<std::string> radios;
    if(settings_.wifiControls)radios.push_back("wifi");if(settings_.bluetoothControls)radios.push_back("bluetooth");
    const float radioWidth=radios.size()==2?(scene_.width-40)/2:scene_.width-32;
    for(std::size_t i=0;i<radios.size();++i)
    {
        const auto& key=radios[i];const float left=16+static_cast<float>(i)*(radioWidth+8);
        Radio(key,Rect(left,y,radioWidth-32,64),false);scene_.nodes.back().joinRight=true;
        auto& more=Add(key+".more",ui::Role::Button,Rect(left+radioWidth-32,y,32,64),L"",L"\uE76C");
        more.accent=scene_.Find("radio:"+key)->selected;more.joinLeft=true;more.tooltip=_LW(key=="bluetooth"?"statusBar.bluetoothControls":"statusBar.wifiControls");
        Command(key+".more",[this,key]{Select(key);});
    }
    if(!radios.empty())y+=82;
    if(settings_.audioControls)Volume("output",y);
    if(settings_.brightnessControls)
    {
    const auto value=Current("system.display.brightness");const auto& monitors=Items(value,"monitors");
    auto m=std::find_if(monitors.begin(),monitors.end(),[](const auto& v){return j::Flag(v,"available")&&!j::String(v,"id").empty()&&InRange(Number(v,"brightness"),100);});
    const bool valid=m!=monitors.end();const auto id=valid?j::String(*m,"id"):std::string{};
    const float level=valid?static_cast<float>(j::Numeric(*m,"brightness")):0;
    Add("brightness.label",ui::Role::Text,Rect(16,y,scene_.width-90,22),_LW("statusBar.brightnessControls")).fontSize=12;
    Add("brightness.value",ui::Role::Text,Rect(scene_.width-68,y,52,22),valid?Percent(level):L"—").fontSize=12;y+=24;
    Add("brightness.icon",ui::Role::Text,Rect(12,y,40,40),L"",L"\uE706");
    const auto sliderId="brightness.level:"+id;
    auto& slider=Add(sliderId,ui::Role::Slider,Rect(58,y,scene_.width-116,40));slider.enabled=valid;slider.value=level/100;slider.tooltip=_LW("statusBar.brightnessControls");
    sliderTargets_[sliderId]=valid?id:std::string{};valueControls_["brightness.value"]=sliderId;
    actions_[sliderId]=[this,id](auto v){const auto current=Current("system.display.brightness");const auto& displays=Items(current,"monitors");if(v&&std::any_of(displays.begin(),displays.end(),[&](const auto& d){return j::String(d,"id")==id&&j::Flag(d,"available");}))Start("system.display.setBrightness",{{"monitorId",id},{"brightness",std::to_string(*v*100)}});};
    Add("brightness.more",ui::Role::Icon,Rect(scene_.width-52,y,36,40),L"",L"\uE76C").tooltip=_LW("statusBar.brightnessControls");Command("brightness.more",[this]{Select("brightness");});y+=54;
    }
    Add("divider",ui::Role::Separator,Rect(16,y,scene_.width-32,1));y+=12;
    if(settings_.powerControls)
    {
    const auto power=Current("system.power.plans");const auto* battery=power.Find("batteryPercent");
    const bool valid=battery&&battery->IsNumber()&&InRange(battery->number,100);
    const wchar_t glyph=valid?(battery->number>=99.5?L'\uE83F':static_cast<wchar_t>(0xE850+std::clamp(static_cast<int>(battery->number/10),0,9))):L'\uE996';
    auto& batteryNode=Add("battery",ui::Role::Text,Rect(16,y,scene_.width-128,36),valid?Percent(battery->number):L"—",std::wstring(1,glyph));
    batteryNode.charging=valid&&j::Flag(power,"charging");
    Add("power.more",ui::Role::Icon,Rect(scene_.width-100,y,36,36),L"",L"\uE7E8").tooltip=_LW("statusBar.powerControls");Command("power.more",[this]{Select("power");});
    }
    Add("system.settings",ui::Role::Icon,Rect(scene_.width-52,y,36,36),L"",L"\uE713").tooltip=_LW("statusBar.systemSettings");Command("system.settings",[this]{if(source_.settings)source_.settings(L"ms-settings:");});y+=48;
}
void SystemPanelModel::Audio(float& y)
{
    const auto value=Current("audio.devices");
    for(const auto* direction:{"output","input"})
    {
        Add(std::string(direction)+".heading",ui::Role::Text,Rect(16,y,scene_.width-32,28),_LW(std::string_view(direction)=="input"?"controlCenter.input":"controlCenter.output")).bold=true;y+=34;
        const auto prefix="audio."+std::string(direction);const auto active=j::String(Current((prefix+".volume").c_str()),"endpointId");
        bool found=false;for(const auto& d:Items(value,"devices"))if(j::String(d,"direction")==direction)
        {
            found=true;const auto endpoint=j::String(d,"id"),id=prefix+".device:"+endpoint;
            auto& node=Add(id,ui::Role::ListItem,Rect(8,y,scene_.width-16,48),Wide(j::String(d,"name")),std::string_view(direction)=="input"?L"\uE720":L"\uE767");node.selected=endpoint==active;
            Command(id,[this,prefix,endpoint]{Start(prefix+".selectDevice",{{"endpointId",endpoint}});});y+=50;
        }
        if(!found){Add(prefix+".empty",ui::Role::Text,Rect(16,y,scene_.width-32,32),_LW("controlCenter.unavailable"));y+=36;}
        y+=8;Volume(direction,y);y+=8;
    }Footer(L"ms-settings:sound",y);
}
void SystemPanelModel::Brightness(float& y)
{
    const auto value=Current("system.display.brightness");const auto& monitors=Items(value,"monitors");
    for(const auto& m:monitors)
    {
        const auto id=j::String(m,"id");const bool valid=!id.empty()&&j::Flag(m,"available")&&InRange(Number(m,"brightness"),100);
        Add("display:"+id,ui::Role::Text,Rect(16,y,scene_.width-90,34),Wide(j::String(m,"name")),L"\uE7F4");
        Add("display.value:"+id,ui::Role::Text,Rect(scene_.width-68,y,52,34),valid?Percent(Number(m,"brightness")):L"—");y+=38;
        const auto sliderId="display.level:"+id;
        auto& slider=Add(sliderId,ui::Role::Slider,Rect(16,y,scene_.width-32,40));slider.enabled=valid;slider.value=valid?static_cast<float>(Number(m,"brightness")/100):0;slider.tooltip=Wide(j::String(m,"name"));
        sliderTargets_[sliderId]=valid?id:std::string{};valueControls_["display.value:"+id]=sliderId;
        actions_[sliderId]=[this,id](auto v){const auto current=Current("system.display.brightness");const auto& displays=Items(current,"monitors");if(v&&std::any_of(displays.begin(),displays.end(),[&](const auto& d){return j::String(d,"id")==id&&j::Flag(d,"available");}))Start("system.display.setBrightness",{{"monitorId",id},{"brightness",std::to_string(*v*100)}});};y+=58;
    }
    if(monitors.empty()){Add("empty",ui::Role::Text,Rect(16,y,scene_.width-32,56),_LW("controlCenter.unsupportedHint"));y+=64;}Footer(L"ms-settings:display",y);
}
void SystemPanelModel::WifiPage(float& y)
{
    const auto value=Current("network.wifi");const auto& adapters=Items(value,"interfaces");
    if(page_=="wifi-adapters")
    {
        for(const auto& a:adapters){const auto id=j::String(a,"id");auto& n=Add("adapter:"+id,ui::Role::ListItem,Rect(8,y,scene_.width-16,48),Wide(j::String(a,"name")));n.selected=id==interface_;Command(n.id,[this,id]{interface_=id;network_.clear();Select("wifi");});y+=50;}return;
    }
    if(adapters.size()>1){Add("wifi.adapter",ui::Role::Button,Rect(16,y,scene_.width-32,38),Wide(j::String(Wifi(),"name")),L"\uE701");Command("wifi.adapter",[this]{Select("wifi-adapters");});y+=46;}
    Radio("wifi",Rect(scene_.width-54,10,40,36),true);
    const auto current=Wifi();const auto networks=system_control::WifiPresentationNetworks(current);
    if(j::String(current,"error")=="accessDenied")
    {Add("wifi.denied",ui::Role::Text,Rect(16,y,scene_.width-32,44),_LW("controlCenter.locationDenied")).fontSize=12;y+=48;Add("wifi.location",ui::Role::Button,Rect(16,y,scene_.width-32,36),_LW("controlCenter.locationSettings"));Command("wifi.location",[this]{if(source_.settings)source_.settings(L"ms-settings:privacy-location");});y+=44;}
    for(const auto& n:networks)
    {
        const auto id=j::String(n,"id"),profile=j::String(n,"profileName");const bool connected=j::Flag(n,"connected"),open=id==network_||connected;
        auto& row=Add("wifi.network:"+id,ui::Role::ListItem,Rect(8,y,scene_.width-16,56),Wide(j::String(n,"ssid")),L"\uE701");row.selected=open;row.detail=(connected?std::wstring(_LW("controlCenter.connected"))+L" · ":L"")+Percent(j::Numeric(n,"signal"));
        Command(row.id,[this,id]{network_=id;});y+=58;
        if(open)
        {
            auto& button=Add("wifi.connect:"+id,ui::Role::Button,Rect(scene_.width-150,y,134,36),_LW(connected?"controlCenter.disconnect":"controlCenter.connect"));button.enabled=connected||j::Flag(n,"connectable");
            const auto security=j::String(n,"security");Command(button.id,[this,id,profile,connected,security]{if(connected)Start("network.wifi.disconnect",{{"interfaceId",interface_}});else if(!profile.empty())Start("network.wifi.connect",{{"interfaceId",interface_},{"profileName",profile}});else if(security=="system"){if(source_.settings)source_.settings(L"ms-settings:network-wifi");}else Start("network.wifi.connect",{{"interfaceId",interface_},{"networkId",id}});});
            if(!profile.empty()){auto& forget=Add("wifi.forget:"+id,ui::Role::Icon,Rect(16,y,36,36),L"",L"\uE74D");forget.tooltip=_LW("controlCenter.forget");Command(forget.id,[this,profile]{Start("network.wifi.forget",{{"interfaceId",interface_},{"profileName",profile}});});}y+=44;
        }
    }
    if(networks.empty()){Add("wifi.empty",ui::Role::Text,Rect(16,y,scene_.width-32,48),_LW(j::Flag(current,"enabled")?"controlCenter.noDevices":"controlCenter.off"));y+=56;}
    Add("wifi.hidden",ui::Role::Button,Rect(16,y,scene_.width-84,36),_LW("controlCenter.hiddenNetwork"),L"\uE72E").enabled=j::Flag(current,"enabled");
    Command("wifi.hidden",[this]{Start("network.wifi.connect",{{"interfaceId",interface_},{"hidden","1"}});});
    Add("wifi.scan",ui::Role::Icon,Rect(scene_.width-52,y,36,36),L"",L"\uE72C").enabled=j::Flag(current,"enabled");Command("wifi.scan",[this]{Start("network.wifi.scan",{{"interfaceId",interface_}});});y+=48;Footer(L"ms-settings:network-wifi",y);
}
void SystemPanelModel::Bluetooth(float& y)
{
    const auto value=Current("bluetooth.devices");const auto& radios=Items(value,"radios");Radio("bluetooth",Rect(scene_.width-54,10,40,36),true);
    const bool available=std::any_of(radios.begin(),radios.end(),[](const auto& r){return j::Flag(r,"available");});
    const bool powered=std::any_of(radios.begin(),radios.end(),[](const auto& r){return j::Flag(r,"available")&&j::Flag(r,"enabled");});
    if(radios.size()>1)for(const auto& r:radios){const auto id=j::String(r,"id");auto& n=Add("bluetooth.radio:"+id,ui::Role::Toggle,Rect(16,y,scene_.width-32,42),Wide(j::String(r,"name")));n.selected=j::Flag(r,"enabled");n.enabled=j::Flag(r,"available");const bool on=n.selected;Command(n.id,[this,id,on]{Start("bluetooth.setRadio",{{"radioId",id},{"enabled",on?"0":"1"}});});y+=50;}
    const auto& devices=Items(value,"devices");
    for(const auto& d:devices)
    {
        const auto id=j::String(d,"id");const bool connected=j::Flag(d,"connected"),supported=j::Flag(d,"canConnect");
        auto& row=Add("bluetooth.device:"+id,ui::Role::ListItem,Rect(8,y,scene_.width-16,62),Wide(j::String(d,"name")),L"\uE702");
        row.enabled=powered;
        row.detail=_LW(connected?"controlCenter.connected":"controlCenter.connect");if(const auto* level=d.Find("batteryPercent");level&&level->IsNumber())row.detail+=L" · "+Percent(level->number);row.selected=connected;
        Command(row.id,[this,id,connected,supported]{if(supported)Start(connected?"bluetooth.disconnect":"bluetooth.connect",{{"deviceId",id}});else if(source_.settings)source_.settings(L"ms-settings:bluetooth");});y+=66;
    }
    if(devices.empty()){Add("bluetooth.empty",ui::Role::Text,Rect(16,y,scene_.width-32,52),_LW(!available?"controlCenter.unavailable":!powered?"controlCenter.off":"controlCenter.noDevices"));y+=60;}Footer(L"ms-settings:bluetooth",y);
}
void SystemPanelModel::Power(float& y)
{
    const auto value=Current("system.power.plans");
    for(const auto& p:Items(value,"plans")){const auto id=j::String(p,"id");auto& n=Add("power.plan:"+id,ui::Role::ListItem,Rect(8,y,scene_.width-16,44),Wide(j::String(p,"name")),L"\uE945");n.selected=j::Flag(p,"active");Command(n.id,[this,id]{Start("system.power.setPlan",{{"planId",id}});});y+=48;}
    if(j::Flag(value,"modeSupported"))for(const auto* mode:{"efficiency","balanced","performance"})
    {auto& n=Add(std::string("power.mode:")+mode,ui::Role::ListItem,Rect(8,y,scene_.width-16,42),_LW((std::string("controlCenter.")+mode).c_str()));n.selected=j::String(value,j::Flag(value,"onAC")?"acMode":"dcMode")==mode;Command(n.id,[this,mode]{Start("system.power.setMode",{{"mode",mode}});});y+=46;}
    y+=8;int index=0;const float w=(scene_.width-40)/2;
    for(const auto* action:{"lock","sleep","restart","shutdown"}){const std::string id=std::string("power.")+action;Add(id,ui::Role::Button,Rect(16+(index%2)*(w+8),y+(index/2)*48,w,40),_LW((std::string("controlCenter.")+action).c_str()));Command(id,[this,action]{Start(std::string("system.power.")+action);});++index;}y+=104;Footer(L"ms-settings:powersleep",y);
}
void SystemPanelModel::Media(float& y)
{
    const auto state=source_.media?source_.media():std::nullopt;if(!state||!state->available||state->sessions.empty())return;
    auto selected=std::find_if(state->sessions.begin(),state->sessions.end(),[&](const auto& s){return s.id==state->currentSessionId;});
    if(selected==state->sessions.end())selected=state->sessions.begin();media_=selected->id;
    auto& art=Add("media.artwork",ui::Role::Image,Rect(12,y,32,32),L"",L"\uE8D6");
    if(const auto a=source_.artwork?source_.artwork():std::nullopt;a&&a->available&&a->sessionId==media_&&a->pixels&&wr::IsValidWidgetRuntimeImage(*a->pixels))
    {auto image=std::make_shared<ui::Image>();image->width=a->pixels->width;image->height=a->pixels->height;image->stride=a->pixels->stride;image->pixels.resize(a->pixels->bgraPremultiplied.size()/4);std::memcpy(image->pixels.data(),a->pixels->bgraPremultiplied.data(),a->pixels->bgraPremultiplied.size());art.image=std::move(image);}
    const float controlsLeft=scene_.width-120;
    auto& title=Add("media.title",ui::Role::Text,Rect(56,y,controlsLeft-64,32),Wide(selected->title.empty()?selected->sourceName:selected->title));
    title.tooltip=title.text+(selected->artist.empty()?L"":L" · "+Wide(selected->artist));
    int index=0;
    for(const auto* command:{"previous","toggle","next"})
    {
        const std::string id=std::string("media.")+command;
        auto& node=Add(id,ui::Role::Icon,Rect(controlsLeft+index*36,y,32,32),L"",
            index==0?L"\uE892":index==2?L"\uE893":selected->playbackStatus=="playing"?L"\uE769":L"\uE768");
        node.tooltip=_LW((std::string("controlCenter.")+command).c_str());
        node.enabled=index==0?selected->controls.canPrevious:index==2?selected->controls.canNext:selected->controls.canPlayPause;
        Command(id,[this,id]{Start(id,{{"sessionId",media_}});});++index;
    }
    y+=32;
}
void SystemPanelModel::Finish(float bodyEnd,bool withMedia)
{
    float mediaHeight=0;const auto state=withMedia&&source_.media?source_.media():std::nullopt;if(state&&state->available&&!state->sessions.empty())mediaHeight=56;
    const float maxBody=(std::max)(32.f,available_-mediaHeight-(mediaHeight?8:0));
    const float end=(std::min)(bodyEnd+8,maxBody);maxScroll_=(std::max)(0.f,bodyEnd+8-end);scroll_=std::clamp(scroll_,0.f,maxScroll_);
    if(end<bodyStart_+48)bodyStart_=0;
    const auto clip=Rect(0,bodyStart_,scene_.width,(std::max)(0.f,end-bodyStart_-8));scrollViewport_=clip;
    for(auto& n:scene_.nodes)if(n.bounds.top>=bodyStart_){n.clip=clip;n.bounds.top-=scroll_;n.bounds.bottom-=scroll_;for(auto& path:n.paths)for(auto& p:path)p.y-=scroll_;}
    scene_.cards.push_back(Rect(0,0,scene_.width,end));scene_.height=end;
    if(maxScroll_>0){const float railHeight=(std::min)(24.f,clip.bottom-clip.top);auto& rail=Add("scrollbar",ui::Role::Card,Rect(scene_.width-4,clip.top+(clip.bottom-clip.top-railHeight)*scroll_/maxScroll_,2,railHeight));rail.enabled=false;}
    if(mediaHeight){float y=end+20;Media(y);scene_.cards.push_back(Rect(0,end+8,scene_.width,mediaHeight));scene_.height=end+8+mediaHeight;}
}
void SystemPanelModel::Refresh(float availableHeight)
{
    if(closed_)return;available_=std::isfinite(availableHeight)?(std::max)(96.f,availableHeight):800;scene_={};actions_.clear();sliderTargets_.clear();valueControls_.clear();bodyStart_=16;scrollViewport_={};
    if(source_.completions)for(const auto& completion:source_.completions())
    {
        std::erase_if(pendingValues_,[&](const auto& item){return item.second.task==completion.id;});
        if(feedback_.Take(completion.id)&&completion.error!="canceled")error_=completion.ok?L"":_LW(completion.error=="accessDenied"?"controlCenter.accessDenied":completion.error=="timeout"?"controlCenter.timeout":"controlCenter.failed");
    }
    if(page_=="media"||(page_=="audio"&&!settings_.audioControls)||(page_=="brightness"&&!settings_.brightnessControls)||
        (page_.starts_with("wifi")&&!settings_.wifiControls)||(page_=="bluetooth"&&!settings_.bluetoothControls)||(page_=="power"&&!settings_.powerControls))page_.clear();
    SyncSubscriptions();
    if(subscriptions_.contains("network.wifi"))
    {
        const auto wifi=Current("network.wifi");const auto& adapters=Items(wifi,"interfaces");
        if(std::none_of(adapters.begin(),adapters.end(),[this](const auto& a){return j::String(a,"id")==interface_;}))
        {interface_=adapters.empty()?std::string{}:j::String(adapters.front(),"id");network_.clear();}
        if(scan_&&page_=="wifi"&&!interface_.empty()&&j::Flag(Wifi(),"enabled"))
        {scan_=false;Start("network.wifi.scan",{{"interfaceId",interface_}});}
    }
    if(action_==StatusBarAction::Tray){Tray();return;}if(action_==StatusBarAction::Calendar){Calendar();return;}if(IsSystemResourceAction(action_)){Resources();return;}
    // The old offline "media" preset still selects the overview; media has no detail page.
    if(page_=="media")page_.clear();
    const char* title=page_=="audio"?"statusBar.audioControls":page_=="brightness"?"statusBar.brightnessControls":page_.starts_with("wifi")?"statusBar.wifiControls":page_=="bluetooth"?"statusBar.bluetoothControls":"statusBar.powerControls";
    if(!page_.empty())Header(_LW(title));float y=bodyStart_;
    if(page_.empty())Overview(y);else if(page_=="audio")Audio(y);else if(page_=="brightness")Brightness(y);else if(page_.starts_with("wifi"))WifiPage(y);else if(page_=="bluetooth")Bluetooth(y);else if(page_=="power")Power(y);
    if(!error_.empty()){auto& n=Add("status",ui::Role::Text,Rect(16,y,scene_.width-32,40),error_);n.fontSize=12;y+=44;}
    Finish(y,settings_.mediaControls);
    std::erase_if(pendingValues_,[&](const auto& entry){const auto* node=scene_.Find(entry.first);if(!node)return false;const auto target=sliderTargets_.find(entry.first);return !node->enabled||target==sliderTargets_.end()||target->second!=entry.second.target;});
    for(auto& node:scene_.nodes)
    {
        if(const auto found=pendingValues_.find(node.id);found!=pendingValues_.end())
        {node.value=found->second.value;node.tooltip=Percent(node.value*100)+L" · "+_LW("controlCenter.working");}
        if(const auto control=valueControls_.find(node.id);control!=valueControls_.end())
            if(const auto found=pendingValues_.find(control->second);found!=pendingValues_.end())node.text=Percent(found->second.value*100);
    }
}
void SystemPanelModel::Tray()
{
    scene_.width=208;bodyStart_=10;const auto snapshot=source_.tray?source_.tray():tray::Snapshot{};auto icons=snapshot.icons;
    std::erase_if(icons,[this](const auto& i){return(i.state&NIS_HIDDEN)||tray::DuplicatesControlCenter(i)||std::find(settings_.pinnedTrayItems.begin(),settings_.pinnedTrayItems.end(),i.persistentKey)!=settings_.pinnedTrayItems.end();});
    const auto rank=[this](const auto& i){return std::find(settings_.trayOrder.begin(),settings_.trayOrder.end(),i.persistentKey)-settings_.trayOrder.begin();};std::stable_sort(icons.begin(),icons.end(),[&](const auto& a,const auto& b){return rank(a)<rank(b);});
    int index=0;for(const auto& icon:icons){auto& n=Add("tray:"+icon.key,ui::Role::Icon,Rect(10+(index%5)*38.f,10+(index/5)*38.f,36,36),L"",L"\uE8A5");n.tooltip=icon.tip.empty()?icon.application:icon.tip;if(icon.width&&icon.height&&icon.pixels.size()==static_cast<std::size_t>(icon.width)*icon.height){auto bitmap=std::make_shared<ui::Image>();bitmap->width=icon.width;bitmap->height=icon.height;bitmap->stride=icon.width*4;bitmap->pixels=icon.pixels;n.image=std::move(bitmap);}++index;}
    float end=10+static_cast<float>((index+4)/5)*38;
    if(icons.empty()||snapshot.degraded||!snapshot.connected){Add("tray.notice",ui::Role::Text,Rect(10,end,188,40),_LW(snapshot.degraded?"statusBar.trayUnavailable":!snapshot.connected?"statusBar.trayConnecting":"statusBar.trayEmpty")).fontSize=12;end+=44;}
    Finish(end,false);
}
bool SystemPanelModel::Drop(std::string_view key,D2D1_POINT_2F p)
{
    if(closed_||action_!=StatusBarAction::Tray||p.x<0||p.x>=scene_.width||p.y<0||p.y>=scene_.height||!source_.tray)return false;
    std::string before;for(const auto& n:scene_.nodes)if(n.id.starts_with("tray:")&&(p.y<n.bounds.top||(p.y<n.bounds.bottom&&p.x<(n.bounds.left+n.bounds.right)/2))){before=n.id.substr(5);break;}
    if(!tray::PlaceIcon(settings_,source_.tray(),key,false,before))return false;
    if(source_.trayChanged)source_.trayChanged(settings_);Refresh(available_);return true;
}
void SystemPanelModel::Calendar()
{
    scene_.width=520;
    auto today=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(today))today=calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(date_))date_=today;
    if(!calendar::CalendarService::GetDateInfo(month_))month_=date_.substr(0,7)+"-01";
    const auto info=calendar::CalendarService::GetDateInfo(date_);const auto month=calendar::CalendarService::GetDateInfo(month_);
    if(!info||!month)return;
    Add("calendar.month",ui::Role::Text,Rect(150,14,230,36),std::to_wstring(month->year)+L" / "+std::to_wstring(month->month)).bold=true;
    Add("calendar.today",ui::Role::Button,Rect(16,14,100,32),_LW("app.widget.date_picker.today"));
    Command("calendar.today",[this,today]{date_=today;month_=date_.substr(0,7)+"-01";scroll_=0;});
    for(int i=0;i<2;++i)
    {
        const auto id=i?"calendar.next":"calendar.previous";
        auto& n=Add(id,ui::Role::Icon,Rect(432+i*38.f,14,32,32),L"",i?L"\uE76C":L"\uE76B");
        n.tooltip=_LW(i?"app.widget.date_picker.next":"app.widget.date_picker.previous");
        n.enabled=i?month->year<9999||month->month<12:month->year>1||month->month>1;
        Command(id,[this,i]{const auto d=calendar::CalendarService::GetDateInfo(month_);if(!d)return;int m=d->month+(i?1:-1),year=d->year;if(m<1){m=12;--year;}if(m>12){m=1;++year;}if(year>=1&&year<=9999){month_=Date(year,m,1);scroll_=0;}});
    }
    SYSTEMTIME time{};time.wYear=static_cast<WORD>(info->year);time.wMonth=static_cast<WORD>(info->month);time.wDay=static_cast<WORD>(info->day);wchar_t weekday[96]{};const auto locale=Wide(Locale::Instance().GetEffectiveLanguage());GetDateFormatEx(locale.c_str(),0,&time,L"dddd",weekday,96,nullptr);
    Add("calendar.weekday",ui::Role::Text,Rect(16,78,116,30),weekday).centered=true;
    auto& day=Add("calendar.day",ui::Role::Text,Rect(16,108,116,76),std::to_wstring(info->day));day.fontSize=54;day.bold=day.centered=true;
    Add("calendar.date",ui::Role::Text,Rect(16,192,116,24),std::to_wstring(info->year)+L" / "+std::to_wstring(info->month)).centered=true;
    if(source_.calendar.secondaryDate)
    {
        const auto text=Wide(source_.calendar.secondaryDate(date_));
        if(!text.empty()){auto& secondary=Add("calendar.secondary",ui::Role::Text,Rect(12,222,124,70),text);secondary.fontSize=12;secondary.centered=secondary.wrap=true;}
    }
    const int offset=(month->weekday+5)%7;const float cell=50;
    for(int i=0;i<7;++i)
    {
        const auto key="app.widget.date_picker.weekday"+std::to_string((i+1)%7+1);
        auto& n=Add("calendar.column:"+std::to_string(i),ui::Role::Text,Rect(150+i*cell,66,46,22),_LW(key.c_str()));
        n.centered=n.secondary=true;n.fontSize=12;
    }
    for(int i=0;i<42;++i)
    {
        const auto date=calendar::CalendarService::AddDays(month_,i-offset);if(!date)continue;
        const auto d=calendar::CalendarService::GetDateInfo(*date);if(!d)continue;
        auto& n=Add("date:"+*date,ui::Role::ListItem,Rect(150+(i%7)*cell,94+(i/7)*34.f,46,30),std::to_wstring(d->day));
        n.centered=true;n.outlined=*date==date_;n.selected=n.accent=*date==today;n.secondary=d->month!=month->month;n.fontSize=13;n.tooltip=Wide(*date);
        Command(n.id,[this,date=*date]{date_=date;month_=date.substr(0,7)+"-01";});
    }
    float y=310;Add("calendar.selected",ui::Role::Text,Rect(16,y,330,36),Wide(date_)).bold=true;Add("calendar.manage",ui::Role::Button,Rect(354,y,150,36),_LW("statusBar.manageCalendar"));Command("calendar.manage",[this]{if(source_.calendar.manage)source_.calendar.manage();});y+=48;
    const auto events=source_.calendar.events?source_.calendar.events(date_):std::vector<calendar::CalendarEvent>{};
    if(events.empty()){Add("calendar.empty",ui::Role::Text,Rect(16,y,488,40),_LW("settings.calendar.empty"));y+=48;}
    for(const auto& e:events){wchar_t when[32]{};swprintf_s(when,L"%02d:%02d",e.startMinutes/60,e.startMinutes%60);auto& n=Add("event:"+e.id,ui::Role::Card,Rect(16,y,488,56),Wide(e.title));n.detail=e.allDay?_LW("settings.calendar.allDay"):when;y+=64;}
    bodyStart_=available_<420?56.f:310.f;Finish(y,false);
}
void SystemPanelModel::Resources()
{
    const char* labels[4]{};std::array<std::wstring,4> values{L"—",L"—",L"—",L"—"};std::wstring subtitle;bool available=false;const std::string topic=Topic(action_);
    const auto title=_LW(action_==StatusBarAction::Cpu?"statusBar.cpu":action_==StatusBarAction::Memory?"statusBar.memory":action_==StatusBarAction::Gpu?"statusBar.gpu":"statusBar.traffic");
    const auto now=source_.now?source_.now():0;
    const auto fresh=[now](std::int64_t stamp){return stamp<=0||now<=0||static_cast<double>(now)-static_cast<double>(stamp)<=5000;};
    if(action_==StatusBarAction::Gpu&&page_=="gpu-select")Header(title);
    else Add("resource.title",ui::Role::Text,Rect(16,12,408,36),title).bold=true;
    float y=52;
    if(action_==StatusBarAction::Cpu)
    {
        labels[0]="resourcePanel.usage";labels[1]="resourcePanel.logical";
        if(auto v=source_.cpu?source_.cpu():std::nullopt)
        {
            available=v->available&&!v->warmingUp&&fresh(v->timestampMs)&&InRange(v->usagePercent,100);subtitle=Wide(v->name);
            if(available)values[0]=Percent(v->usagePercent);if(v->logicalProcessors)values[1]=std::to_wstring(v->logicalProcessors);
        }
    }
    else if(action_==StatusBarAction::Memory)
    {
        labels[0]="resourcePanel.used";labels[1]="resourcePanel.available";labels[2]="resourcePanel.total";labels[3]="resourcePanel.committed";subtitle=_LW("resourcePanel.physical");
        if(auto v=source_.memory?source_.memory():std::nullopt;v&&v->available&&v->totalBytes)
        {
            values[2]=Bytes(v->totalBytes);
            available=fresh(v->timestampMs)&&v->usedBytes<=v->totalBytes&&v->freeBytes<=v->totalBytes;
            if(available){values[0]=Bytes(v->usedBytes);values[1]=Bytes(v->freeBytes);}
            if(fresh(v->timestampMs)&&v->commitLimitBytes&&v->commitUsedBytes<=v->commitLimitBytes)values[3]=Bytes(v->commitUsedBytes);
        }
    }
    else if(action_==StatusBarAction::Gpu)
    {
        labels[0]="resourcePanel.usage";labels[1]="resourcePanel.dedicated";labels[2]="resourcePanel.shared";labels[3]="resourcePanel.capacity";
        const auto v=source_.gpu?source_.gpu():std::nullopt;
        auto list=v?wr::PresentGpuAdapters(v->adapters):std::vector<wr::WidgetGpuAdapterDataSnapshot>{};
        std::erase_if(list,[](const auto& a){return a.id.empty();});
        if(std::none_of(list.begin(),list.end(),[this](const auto& a){return a.id==gpu_;}))gpu_=list.empty()?std::string{}:list.front().id;
        for(const auto& a:list)if(a.id==gpu_)
        {
            subtitle=Wide(a.name);const bool sampled=v&&v->available&&!v->warmingUp&&fresh(v->timestampMs);
            available=sampled&&a.usageAvailable&&InRange(a.usagePercent,100);
            if(available)values[0]=Percent(a.usagePercent);
            if(sampled&&a.dedicatedUsageAvailable)values[1]=Bytes(a.dedicatedUsedBytes);
            if(sampled&&a.sharedUsageAvailable)values[2]=Bytes(a.sharedUsedBytes);
            if(a.dedicatedMemoryBytes)values[3]=Bytes(a.dedicatedMemoryBytes);
        }
        if(page_=="gpu-select")
        {
            y=bodyStart_;
            for(const auto& a:list){auto& n=Add("gpu:"+a.id,ui::Role::ListItem,Rect(8,y,424,48),Wide(a.name));n.selected=a.id==gpu_;Command(n.id,[this,id=a.id]{gpu_=id;Select("");});y+=52;}
            if(list.empty()){Add("gpu.empty",ui::Role::Text,Rect(16,y,408,44),_LW("resourcePanel.unavailable"));y+=52;}
            Finish(y,false);return;
        }
        if(list.size()>1){Add("gpu.select",ui::Role::Button,Rect(16,y,408,38),subtitle,L"\uE7F4");Command("gpu.select",[this]{Select("gpu-select");});y+=46;subtitle.clear();}
    }
    else
    {
        labels[0]="resourcePanel.download";labels[1]="resourcePanel.upload";labels[2]="resourcePanel.received";labels[3]="resourcePanel.sent";subtitle=_LW("resourcePanel.networkLegend");
        if(auto v=source_.traffic?source_.traffic():std::nullopt;v&&v->available&&!v->warmingUp&&fresh(v->timestampMs))
        {available=true;values={StatusBarRate(v->downloadBytesPerSecond),StatusBarRate(v->uploadBytesPerSecond),Bytes(v->receivedBytes),Bytes(v->sentBytes)};}
    }
    if(!subtitle.empty()){Add("resource.subtitle",ui::Role::Text,Rect(16,y,408,26),subtitle).fontSize=12;y+=30;}
    const auto points=source_.history?source_.history(topic,gpu_):std::vector<wr::WidgetResourcePoint>{};
    const bool traffic=action_==StatusBarAction::Traffic;
    const auto valid=[traffic](const std::optional<double>& value){return value&&InRange(*value,traffic?(std::numeric_limits<double>::max)():100);};
    if(action_!=StatusBarAction::Memory&&(points.empty()||!fresh(points.back().timestampMs)||!valid(points.back().primary)))
    {available=false;values[0]=L"—";}
    if(traffic&&(points.empty()||!fresh(points.back().timestampMs)||!valid(points.back().secondary)))values[1]=L"—";
    auto& chart=Add("resource.chart",ui::Role::Chart,Rect(16,y,408,124));double maximum=traffic?1024:100;
    const auto end=points.empty()?now:(std::max)(now,points.back().timestampMs);
    if(traffic)for(const auto& p:points)if(p.timestampMs>=end-60000&&p.timestampMs<=end)for(const auto& value:{p.primary,p.secondary})if(valid(value))maximum=(std::max)(maximum,*value);
    for(int channel=0;channel<(traffic?2:1);++channel)
    {
        std::vector<D2D1_POINT_2F> path;std::int64_t previous=0;
        for(const auto& p:points)
        {
            const auto value=channel?p.secondary:p.primary;const bool usable=valid(value)&&p.timestampMs>=end-60000&&p.timestampMs<=end;
            if(!usable||(!path.empty()&&(p.timestampMs<=previous||p.timestampMs-previous>2500)))
            {if(!path.empty())chart.paths.push_back(std::move(path));path.clear();}
            if(usable){path.push_back({16+408.f*static_cast<float>(1.-static_cast<double>(end-p.timestampMs)/60000),y+122-120.f*static_cast<float>(std::clamp(*value/maximum,0.,1.))});previous=p.timestampMs;}
        }
        if(!path.empty())chart.paths.push_back(std::move(path));
    }
    y+=134;
    if(!available){Add("resource.status",ui::Role::Text,Rect(16,y,408,28),_LW(points.empty()?"resourcePanel.waiting":"resourcePanel.unavailable")).fontSize=12;y+=34;}
    const int count=action_==StatusBarAction::Cpu?2:4;for(int i=0;i<count;++i){auto& n=Add("resource.card:"+std::to_string(i),ui::Role::Card,Rect(16+(i%2)*210.f,y+(i/2)*90.f,198,80),values[i]);n.fontSize=22;n.bold=true;n.detail=_LW(labels[i]);}y+=count==2?90:180;Finish(y,false);
}
}
