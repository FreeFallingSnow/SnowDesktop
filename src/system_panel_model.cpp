#include "system_panel_model.h"
#include "status_bar_battery.h"
#include "system_control_wifi_presentation.h"
#include "system_control_audio_presentation.h"
#include "widget_gpu_presentation.h"
#include "status_bar_presentation.h"
#include "system_calendar_editor.h"
#include "tray_order.h"
#include "l10n.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

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
std::wstring CalendarButtonLabel(std::string_view calendarId,std::string_view language)
{
    if(calendarId=="chinese")return _LW("statusBar.calendarChineseShort");
    if(calendarId=="roc")return _LW("statusBar.calendarRocShort");
    for(const auto& option:calendar::CalendarOptions(language))
        if(option.id==calendarId)return option.label;
    return _LW("settings.calendar.type");
}
}
bool IsSystemResourceAction(StatusBarAction a)
{ return a==StatusBarAction::Cpu||a==StatusBarAction::Memory||a==StatusBarAction::Gpu||a==StatusBarAction::Traffic; }
ui::Palette SystemPanelPalette(const PersonalizationSettings& appearance, bool highContrast)
{
    if(highContrast)
    {
        const auto color=[](int index){const auto c=GetSysColor(index);return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f);};
        return {color(COLOR_WINDOWTEXT),color(COLOR_WINDOWTEXT),color(COLOR_HIGHLIGHT),color(COLOR_HIGHLIGHTTEXT),color(COLOR_BTNFACE),color(COLOR_WINDOW),color(COLOR_WINDOWTEXT),appearance.cornerRadius,true};
    }
    const bool light=appearance.contentTheme==1;
    return {D2D1::ColorF(light?0x202020:0xffffff),D2D1::ColorF(light?0x626262:0xceced2),D2D1::ColorF(0x4cc2ff),D2D1::ColorF(0x001c2b),D2D1::ColorF(light?0:0xffffff,light?.10f:.13f),D2D1::ColorF(0xffffff,light?.46f:.065f),D2D1::ColorF(light?0:0xffffff,.20f),appearance.cornerRadius};
}
SystemPanelSource LiveSystemPanelSource(std::shared_ptr<wr::WidgetSystemDataProvider> data)
{
    SystemPanelSource s;auto service=data->Controls();
    s.current=[service](auto topic){return service->Current(topic);};
    s.start=[service](auto request){return service->Start("nativeSystemPanel",std::move(request));};
    s.cancel=[service](auto id){return service->Cancel(id);};
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
SystemPanelModel::SystemPanelModel(SystemPanelSource source,StatusBarSettings settings,StatusBarAction action,bool calendarStacked)
    :source_(std::move(source)),settings_(std::move(settings)),action_(action),calendarStacked_(calendarStacked)
{
    date_=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(date_))date_=calendar::CalendarService::CurrentLocalNow().date;
    month_=date_.substr(0,7)+"-01";
    Refresh();
}
SystemPanelModel::~SystemPanelModel(){Close();}
void SystemPanelModel::Close()
{
    if(closed_)return;
    CancelControlInput();
    closed_=true;dismissRequested_=false;actions_.clear();feedback_.Clear();pendingValues_.clear();sliderTargets_.clear();valueControls_.clear();actionBindings_.clear();pendingActions_.clear();ClearError();invokingControl_.clear();mediaState_.reset();subscriptions_.clear();calendarAnnotations_.clear();calendarDetails_.clear();calendarDisplaySecondary_=false;calendarAnnotationKey_.clear();calendarDetailsRevision_.clear();lastStarted_=0;
    // A close callback may pump messages. Detach all effects before calling it,
    // and keep its callable alive even if the callback re-enters Close().
    auto close=std::move(source_.close);source_={};if(close)close();
}
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
void SystemPanelModel::Start(std::string task,system_control::Arguments args,std::string_view control)
{
    if(closed_||!source_.start)return;
    const std::string controlId=control.empty()?invokingControl_:std::string(control);
    const auto found=actionBindings_.find(controlId);
    const auto binding=found==actionBindings_.end()?std::optional<ActionBinding>{}:std::optional<ActionBinding>(found->second);
    if(binding&&(pendingActions_.contains(binding->group)||RadioTransitionPending(*binding)))return;
    system_control::Request request;request.name=std::move(task);request.arguments=std::move(args);
    const bool connect=request.name=="network.wifi.connect"&&!request.arguments.contains("profileName");
    const bool hiddenNetwork=connect&&request.arguments.contains("hidden");
    if(connect&&!hiddenNetwork)
    {
        const auto networks=system_control::WifiPresentationNetworks(Wifi());
        const auto network=std::find_if(networks.begin(),networks.end(),[&](const auto& n){return j::String(n,"id")==request.arguments["networkId"];});
        if(network==networks.end())return;
        request.arguments["security"]=j::String(*network,"security");
        if(request.arguments["security"]!="open"&&request.arguments["security"]!="wpa2"&&request.arguments["security"]!="wpa3")return;
    }
    if(hiddenNetwork||system_control::RequiresConfirmation(request.name)||(connect&&request.arguments["security"]!="open"))
    {
        CancelControlInput();
        ControlDraft draft;draft.request=std::move(request);draft.control=controlId;draft.binding=binding;draft.hidden=hiddenNetwork;
        draft.passwordId="control.password:"+std::to_string(++controlSerial_);
        if(hiddenNetwork){page_="wifi-hidden";scroll_=0;draft.request.arguments["security"]="wpa2";}
        controlDraft_=std::move(draft);ClearError();return;
    }
    CancelControlInput();SubmitControl(std::move(request),controlId);
}
void SystemPanelModel::SubmitControl(system_control::Request request,const std::string& controlId)
{
    if(closed_||!source_.start)return;
    const auto generation=navigation_;
    const auto found=actionBindings_.find(controlId);
    const auto binding=found==actionBindings_.end()?std::optional<ActionBinding>{}:std::optional<ActionBinding>(found->second);
    if(binding&&(pendingActions_.contains(binding->group)||RadioTransitionPending(*binding)))return;
    if(binding)
    {
        const auto current=actionBindings_.find(controlId);
        if(current==actionBindings_.end()||current->second.group!=binding->group||current->second.target!=binding->target||pendingActions_.contains(binding->group)||RadioTransitionPending(current->second))return;
        if(binding->group==binding->radioGroup&&source_.cancel)
        {
            // Service serializes execution/readback for each source. Cancel a
            // predecessor before queuing its radio transition, so it cannot
            // execute afterwards or report an obsolete connection failure.
            // Unknown Bluetooth ownership is deliberately not guessed here.
            std::vector<std::uint64_t> canceled;
            std::erase_if(pendingActions_,[&](const auto& entry) {
                if(entry.second.radioGroup!=binding->radioGroup)return false;
                canceled.push_back(entry.second.task);feedback_.Take(entry.second.task);return true;
            });
            const auto cancel=source_.cancel;
            for(const auto id:canceled){cancel(id);if(closed_||generation!=navigation_)return;}
        }
    }
    const auto start=source_.start;
    PendingAction pending;pending.control=controlId;pending.navigation=generation;
    if(binding)
    {
        pending.target=binding->target;pending.radioGroup=binding->radioGroup;
        const auto argument=[&](const char* field){const auto at=request.arguments.find(field);return at==request.arguments.end()?std::string{}:at->second;};
        if(request.name.starts_with("network.wifi.")){pending.topic="network.wifi";pending.collection="interfaces";pending.device=argument("interfaceId");}
        else if(request.name.starts_with("bluetooth.")){pending.topic="bluetooth.devices";pending.collection=request.name=="bluetooth.setRadio"?"radios":"devices";pending.device=argument(request.name=="bluetooth.setRadio"?"radioId":"deviceId");}
        else if(request.name.starts_with("audio.")){pending.topic="audio.devices";pending.collection="devices";pending.device=binding->target;}
        else if(request.name.starts_with("media.")){pending.topic="media.sessions";pending.device=argument("sessionId");}
        else if(request.name=="system.power.setPlan"){pending.topic="system.power.plans";pending.collection="plans";pending.device=argument("planId");}
    }
    const auto key=system_control::ControlFeedback::Key(request);const auto id=start(std::move(request));
    if(closed_||generation!=navigation_)return;
    feedback_.Track(key,id);lastStarted_=id;
    if(id&&binding){pending.task=id;pendingActions_[binding->group]=std::move(pending);}
    ClearError();
}
void SystemPanelModel::BindAction(std::string control,std::string group,std::string target,std::string indicator,std::string radioGroup)
{actionBindings_[std::move(control)]={std::move(group),std::move(target),std::move(indicator),std::move(radioGroup)};}
bool SystemPanelModel::RadioTransitionPending(const ActionBinding& binding) const
{
    if(binding.radioGroup.empty())return false;
    if(binding.radioGroup=="bluetooth.unknown")
        return std::any_of(pendingActions_.begin(),pendingActions_.end(),[](const auto& entry){return entry.first.starts_with("bluetooth.radio:");});
    return binding.group!=binding.radioGroup&&pendingActions_.contains(binding.radioGroup);
}
void SystemPanelModel::ClearError()
{error_.clear();errorControl_.clear();errorGroup_.clear();errorTarget_.clear();}
void SystemPanelModel::ApplyError(float& bodyEnd)
{
    if(error_.empty())return;
    if(!errorControl_.empty())
    {
        const auto binding=actionBindings_.find(errorControl_);
        if(binding==actionBindings_.end()||binding->second.group!=errorGroup_||binding->second.target!=errorTarget_){ClearError();return;}
        const auto& row=binding->second.indicator.empty()?errorControl_:binding->second.indicator;
        for(auto& node:scene_.nodes)if(node.id==row&&node.role==ui::Role::ListItem)
        {
            node.detail=error_;node.tooltip=node.text+L"\n"+error_;node.accessibilityLabel=node.text+L" · "+error_;return;
        }
    }
    // Header/slider errors have no device row. Reserve a fixed, visible line
    // above the scroll viewport; moving it with the list hides real failures.
    const float top=bodyStart_,height=44;
    for(auto& node:scene_.nodes)if(node.bounds.top>=top)
    {node.bounds.top+=height;node.bounds.bottom+=height;}
    bodyStart_+=height;bodyEnd+=height;
    auto& node=Add("status",ui::Role::Text,Rect(16,top,scene_.width-32,40),error_);node.fontSize=12;node.wrap=true;
}
void SystemPanelModel::PrunePendingActions()
{
    std::erase_if(pendingActions_,[this](const auto& entry) {
        const auto& pending=entry.second;bool removed=false;
        if(pending.topic=="media.sessions")
            removed=mediaState_&&mediaState_->available&&std::none_of(mediaState_->sessions.begin(),mediaState_->sessions.end(),[&](const auto& session){return session.id==pending.device;});
        else if(!pending.topic.empty()&&!pending.device.empty()&&source_.current)
        {
            const auto snapshot=source_.current(pending.topic);
            // An unreadable or unsubscribed source does not prove removal. The
            // backend's bounded completion still owns the task's final outcome.
            if(snapshot&&snapshot->available&&snapshot->error.empty())
            {
                const auto* items=snapshot->value.Find(pending.collection);
                if(items&&items->IsArray())removed=std::none_of(items->array.begin(),items->array.end(),[&](const auto& item){return j::String(item,"id")==pending.device&&
                    (pending.topic!="audio.devices"||(j::Flag(item,"available")&&j::String(item,"state")=="active"));});
            }
        }
        if(removed)feedback_.Take(pending.task);return removed;
    });
}
void SystemPanelModel::ApplyPendingActions()
{
    const std::wstring working=_LW("controlCenter.working");
    for(auto& node:scene_.nodes)
        if(const auto binding=actionBindings_.find(node.id);binding!=actionBindings_.end()&&RadioTransitionPending(binding->second))node.enabled=false;
    for(const auto& [group,pending]:pendingActions_)
    {
        const auto origin=actionBindings_.find(pending.control);
        const bool sameTarget=origin!=actionBindings_.end()&&origin->second.group==group&&origin->second.target==pending.target;
        const auto indicator=sameTarget?origin->second.indicator:std::string{};
        const auto* control=scene_.Find(pending.control);
        const bool visibleButton=control&&control->role==ui::Role::Button;
        for(auto& node:scene_.nodes)
        {
            const auto binding=actionBindings_.find(node.id);
            if(binding!=actionBindings_.end()&&binding->second.group==group)node.enabled=false;
            // Transport controls acknowledge presses with their existing icon
            // state. Keep the task guard, without replacing song/action text.
            if(pending.topic=="media.sessions")continue;
            if(!sameTarget||(node.id!=pending.control&&node.id!=indicator))continue;
            if(node.id==indicator&&visibleButton)continue;
            const auto label=!node.accessibilityLabel.empty()?node.accessibilityLabel:!node.text.empty()?node.text:node.tooltip;
            node.tooltip=label.empty()?working:label+L" · "+working;
            if(node.id==pending.control){node.accessibilityLabel=node.tooltip;node.busy=true;}
            if(node.role==ui::Role::Button)node.text=working;
            else if(node.role==ui::Role::Text&&node.id.ends_with(".label"))node.text=node.tooltip;
            else if(!node.switchStyle&&!node.text.empty())node.detail=working;
        }
    }
}
void SystemPanelModel::OpenSettings(const wchar_t* uri)
{if(closed_)return;const auto open=source_.settings;if(open)open(uri);}
ui::Node& SystemPanelModel::Add(std::string id,ui::Role role,D2D1_RECT_F rect,std::wstring text,std::wstring glyph)
{ ui::Node node;node.id=std::move(id);node.role=role;node.bounds=rect;node.text=std::move(text);node.glyph=std::move(glyph);node.tooltip=node.text;
  if(action_!=StatusBarAction::Calendar&&action_!=StatusBarAction::Tray&&!IsSystemResourceAction(action_))node.fontSize=13;
  scene_.nodes.push_back(std::move(node));return scene_.nodes.back(); }
void SystemPanelModel::Command(std::string id,std::function<void()> fn)
{ actions_[std::move(id)]=[fn=std::move(fn)](auto){fn();}; }
bool SystemPanelModel::Invoke(std::string_view id,std::optional<float> value)
{
    if(closed_||dismissRequested_)return false;
    const std::string key(id);const auto* node=scene_.Find(key);
    if(!node||!node->Interactive()||(value&&(node->role!=ui::Role::Slider||!std::isfinite(*value))))return false;
    if(value)*value=std::clamp(*value,0.f,1.f);
    const auto it=actions_.find(key);if(it==actions_.end())return false;
    const auto target=sliderTargets_.contains(key)?sliderTargets_.at(key):std::string{};
    // Navigation/close rebuilds or clears actions_ during the call.
    const auto fn=it->second;const auto previous=std::exchange(invokingControl_,key);lastStarted_=0;fn(value);
    if(closed_)return false;
    invokingControl_=previous;
    if(value){if(lastStarted_)pendingValues_[key]={lastStarted_,*value,target};else pendingValues_.erase(key);}
    Refresh(available_);return true;
}
void SystemPanelModel::Select(std::string page)
{ if(closed_)return;CancelControlInput();if(CalendarEditing())LeaveCalendarEditor(false);dismissRequested_=false;++navigation_;page_=std::move(page);scroll_=0;scan_=page_=="wifi";ClearError();Refresh(available_); }
void SystemPanelModel::CancelControlInput()
{
    if(!controlDraft_)return;
    const auto task=controlDraft_->task;controlDraft_.reset();
    if(task)
    {
        feedback_.Take(task);
        std::erase_if(pendingActions_,[&](const auto& item){return item.second.task==task;});
        const auto cancel=source_.cancel;if(cancel)cancel(task);
    }
}
bool SystemPanelModel::BeginPowerConfirmation(std::string_view task,bool dismissOnCancel)
{
    if(closed_||(task!="system.power.sleep"&&task!="system.power.restart"&&task!="system.power.shutdown"))return false;
    CancelControlInput();dismissRequested_=false;page_="power";scroll_=0;
    Start(std::string(task),{},"power.confirmation");
    if(controlDraft_)controlDraft_->returnRoute=dismissOnCancel?ControlDraft::ReturnRoute::ClosePanel:ControlDraft::ReturnRoute::PreviousPage;
    Refresh(available_);return controlDraft_.has_value();
}
bool SystemPanelModel::TakeDismissRequest(){return std::exchange(dismissRequested_,false);}
bool SystemPanelModel::ControlBack()
{
    if(!controlDraft_&&page_!="wifi-hidden")return false;
    const bool dismiss=controlDraft_&&controlDraft_->returnRoute==ControlDraft::ReturnRoute::ClosePanel;
    const bool hidden=page_=="wifi-hidden";CancelControlInput();
    if(dismiss){dismissRequested_=true;return true;}
    if(hidden){page_="wifi";scroll_=0;}
    ClearError();Refresh(available_);return true;
}
std::string SystemPanelModel::ControlFocusTarget() const
{
    if(!controlDraft_)return {};
    if(controlDraft_->task)return "control.cancel";
    if(controlDraft_->hidden)return "control.ssid";
    return controlDraft_->request.name=="network.wifi.connect"?controlDraft_->passwordId:"control.cancel";
}
std::vector<SystemCalendarInputField> SystemPanelModel::ControlInputFields() const
{
    std::vector<SystemCalendarInputField> fields;if(!controlDraft_)return fields;
    for(const auto& id:{std::string("control.ssid"),controlDraft_->passwordId})
    {
        const auto* node=scene_.Find(id);if(!node)continue;
        SystemCalendarInputField field;field.id=id;field.label=node->accessibilityLabel;
        field.bounds=node->bounds;field.clip=node->clip;field.enabled=node->enabled;
        field.password=id==controlDraft_->passwordId;field.limit=field.password?63:32;
        if(!field.password)field.text=controlDraft_->ssid;
        fields.push_back(std::move(field));
    }
    return fields;
}
bool SystemPanelModel::SetControlInput(std::string_view id,std::wstring& text)
{
    if(!id.starts_with("control."))return false;
    if(controlDraft_&&!controlDraft_->task&&!closed_)
    {
        if(id==controlDraft_->passwordId)controlDraft_->request.password=system_control::Secret(text);
        else if(id=="control.ssid")controlDraft_->ssid=text;
        controlDraft_->error.clear();
    }
    // No password enters scene strings, the EDIT descriptor, UIA or settings.
    if(id.starts_with("control.password:")&&!text.empty())SecureZeroMemory(text.data(),text.size()*sizeof(wchar_t));
    return true;
}
void SystemPanelModel::ConfirmControl()
{
    if(!controlDraft_||controlDraft_->task||closed_)return;
    auto& draft=*controlDraft_;const bool connect=draft.request.name=="network.wifi.connect";
    if(connect)
    {
        const auto adapter=Wifi();
        if(draft.request.arguments["interfaceId"]!=interface_||!j::Flag(adapter,"available")||!j::Flag(adapter,"enabled"))
        {draft.error=_LW("controlCenter.unavailable");return;}
        if(draft.hidden)
        {
            const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,draft.ssid.data(),static_cast<int>(draft.ssid.size()),nullptr,0,nullptr,nullptr);
            if(count<=0||count>32)return;
            std::string ssid(static_cast<std::size_t>(count),' ');
            WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,draft.ssid.data(),static_cast<int>(draft.ssid.size()),ssid.data(),count,nullptr,nullptr);
            draft.request.arguments["ssid"]=std::move(ssid);
        }
        else
        {
            const auto networks=system_control::WifiPresentationNetworks(adapter);
            const auto found=std::find_if(networks.begin(),networks.end(),[&](const auto& n){return j::String(n,"id")==draft.request.arguments["networkId"]&&j::String(n,"security")==draft.request.arguments["security"]&&j::Flag(n,"connectable");});
            if(found==networks.end()){draft.error=_LW("controlCenter.unavailable");return;}
        }
        const auto password=draft.request.password.View();
        if(draft.request.arguments["security"]!="open"&&(password.size()<8||password.size()>63||std::any_of(password.begin(),password.end(),[](wchar_t ch){return ch<32||ch>126;})))
        {draft.error=_LW("controlCenter.invalidPassword");return;}
    }
    system_control::Request request;request.name=draft.request.name;request.arguments=draft.request.arguments;
    request.password=std::move(draft.request.password);request.hostConfirmed=true;
    const auto control=draft.control;
    if(draft.binding)actionBindings_[control]=*draft.binding;
    lastStarted_=0;SubmitControl(std::move(request),control);
    if(closed_||!controlDraft_)return;
    controlDraft_->task=lastStarted_;
    // Recreate the protected child after submission to erase its old contents.
    controlDraft_->passwordId="control.password:"+std::to_string(++controlSerial_);
    if(!lastStarted_)controlDraft_->error.clear();
}
void SystemPanelModel::ControlForm(float& y,bool inCard)
{
    if(!controlDraft_)return;auto& draft=*controlDraft_;const float left=inCard?24.f:16.f,right=scene_.width-left,width=right-left;
    const bool connect=draft.request.name=="network.wifi.connect",busy=draft.task!=0;
    if(draft.binding)actionBindings_[draft.control]=*draft.binding;
    const auto field=[&](const std::string& id,const wchar_t* label)
    {
        Add(id+".label",ui::Role::Text,Rect(left,y,width,22),label).fontSize=12;y+=26;
        auto& node=Add(id,ui::Role::Button,Rect(left,y,width,36));node.accessibilityLabel=label;node.enabled=!busy;
        y+=44;
    };
    if(draft.hidden)
    {
        field("control.ssid",_LW("controlCenter.ssid"));
        Add("control.security.label",ui::Role::Text,Rect(left,y,width,22),_LW("controlCenter.security")).fontSize=12;y+=26;
        for(const auto* security:{"open","wpa2","wpa3"})
        {
            const std::string id="control.security:"+std::string(security);
            auto& node=Add(id,ui::Role::ListItem,Rect(left,y,width,36),std::string_view(security)=="open"?_LW("controlCenter.openNetwork"):std::string_view(security)=="wpa2"?L"WPA2-Personal":L"WPA3-Personal");
            node.selected=draft.request.arguments["security"]==security;node.enabled=!busy;
            Command(id,[this,value=std::string(security)]{if(controlDraft_&&!controlDraft_->task){controlDraft_->request.arguments["security"]=value;controlDraft_->request.password.Clear();controlDraft_->passwordId="control.password:"+std::to_string(++controlSerial_);controlDraft_->error.clear();}});y+=38;
        }
        y+=8;
    }
    if(connect&&draft.request.arguments["security"]!="open")field(draft.passwordId,_LW("controlCenter.password"));
    if(!connect)
    {
        if(draft.request.name=="network.wifi.forget"){Add("control.target",ui::Role::Text,Rect(left,y,width,28),Wide(draft.request.arguments["profileName"]));y+=32;}
        const auto* key=draft.request.name=="network.wifi.forget"?"controlCenter.confirmForget":draft.request.name=="system.power.sleep"?"controlCenter.confirmSleep":draft.request.name=="system.power.restart"?"controlCenter.confirmRestart":"controlCenter.confirmShutdown";
        auto& text=Add("control.confirmation",ui::Role::Text,Rect(left,y,width,68),_LW(key));text.wrap=true;y+=76;
    }
    if(!draft.error.empty()){auto& error=Add("control.error",ui::Role::Text,Rect(left,y,width,52),draft.error);error.wrap=true;error.fontSize=12;y+=60;}
    const float button=(std::min)(134.f,(width-8)/2);
    Add("control.cancel",ui::Role::Button,Rect(right-2*button-8,y,button,36),_LW("settings.dialog.cancel")).centered=true;Command("control.cancel",[this]{ControlBack();});
    auto& confirm=Add("control.confirm",ui::Role::Button,Rect(right-button,y,button,36),_LW(connect?"controlCenter.connect":"settings.dialog.confirm"));confirm.enabled=!busy;confirm.busy=busy;confirm.centered=confirm.accent=true;
    if(draft.hidden){const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,draft.ssid.data(),static_cast<int>(draft.ssid.size()),nullptr,0,nullptr,nullptr);confirm.enabled=confirm.enabled&&count>0&&count<=32;}
    Command("control.confirm",[this]{ConfirmControl();});y+=44;
}
void SystemPanelModel::Scroll(float delta)
{
    if(closed_||!std::isfinite(delta))return;
    const float next=std::clamp(scroll_+delta,0.f,maxScroll_);
    if(next==scroll_)return;
    scroll_=next;Refresh(available_);
}
bool SystemPanelModel::Reveal(std::string_view id)
{
    const auto* node=scene_.Find(id);
    if(closed_||!node||node->clip.bottom<=node->clip.top||maxScroll_<=0)return false;
    const auto& clip=node->clip;
    const float delta=node->bounds.top<clip.top?node->bounds.top-clip.top:
        node->bounds.bottom>clip.bottom?(std::min)(node->bounds.top-clip.top,node->bounds.bottom-clip.bottom):0.f;
    const auto previous=scroll_;Scroll(delta);return previous!=scroll_;
}
ui::InputResult SystemPanelModel::HandleKey(ui::Input& input,unsigned key,bool shift)
{
    if(closed_)return {};
    if(calendarPicker_&&calendarPicker_->Type()==ui::DateTimePicker::Kind::Time)
    {
        if(key==VK_TAB&&input.Focused().starts_with("picker."))
        {
            const auto target=calendarPicker_->TimeTabTarget(input.Focused(),shift);
            input.Sync(scene_);input.Focus(target,true);Reveal(target);input.Sync(scene_);return {};
        }
        if(calendarPicker_->TimeKey(input.Focused(),key))
        {
            Refresh(available_);input.Sync(scene_);input.Focus(calendarPicker_->FocusTarget(),true);
            Reveal(input.Focused());input.Sync(scene_);return {};
        }
    }
    auto result=input.Key(scene_,key,shift);
    // A slider owns its direction and range keys, including at either limit.
    // Otherwise a list can scroll without triggering a device control.
    const auto* focused=scene_.Find(input.Focused());
    const bool fixedCalendar=action_==StatusBarAction::Calendar&&focused&&
        ((bodyLeft_>0&&(focused->bounds.right<=bodyLeft_||focused->bounds.bottom<=bodyStart_))||
            (calendarStacked_&&scene_.cards.size()>1&&focused->bounds.top>=scene_.cards.back().top));
    if(result.kind==ui::InputResult::Kind::None&&!fixedCalendar)
    {
        const float page=(std::max)(42.f,scrollViewport_.bottom-scrollViewport_.top-32);
        if(key==VK_UP||key==VK_DOWN)Scroll(key==VK_UP?-42.f:42.f);
        else if(key==VK_PRIOR||key==VK_NEXT)Scroll(key==VK_PRIOR?-page:page);
        else if(key==VK_HOME||key==VK_END)Scroll(key==VK_HOME?-scroll_:maxScroll_-scroll_);
    }
    if(key==VK_TAB||result.kind!=ui::InputResult::Kind::None)Reveal(input.Focused());
    input.Sync(scene_);return result;
}
void SystemPanelModel::Wheel(D2D1_POINT_2F point,float notches)
{
    if(closed_||!std::isfinite(notches)||notches==0)return;
    if(calendarPicker_)
        if(const auto* target=scene_.Hit(point,false);target&&calendarPicker_->TimeWheel(target->id,notches))
        {Refresh(available_);return;}
    const auto* node=scene_.Hit(point);
    if(node&&node->role==ui::Role::Slider)
    {
        const auto next=std::clamp(node->value+notches*.02f,0.f,1.f);
        if(next!=node->value)Invoke(node->id,next);
    }
    else if(action_!=StatusBarAction::Calendar||
        (calendarStacked_&&scene_.cards.size()>1?
            point.y>=scrollViewport_.top&&point.y<scrollViewport_.bottom:
            bodyLeft_<=0||(point.x>=scrollViewport_.left&&point.x<scrollViewport_.right&&
                point.y>=scrollViewport_.top&&point.y<scrollViewport_.bottom)))Scroll(-notches*42.f);
}
widget_scroll_rules::ScrollbarAxisGeometry SystemPanelModel::ScrollbarGeometry() const
{
    const auto extent=static_cast<int>(std::lround(scrollViewport_.bottom-scrollViewport_.top));
    return widget_scroll_rules::ResolveScrollbarAxisGeometry(
        static_cast<int>(std::lround(scrollViewport_.top)),static_cast<int>(std::lround(scrollViewport_.bottom)),
        extent+static_cast<int>(std::lround(maxScroll_)),extent,static_cast<int>(std::lround(scroll_)));
}
void SystemPanelModel::DragScrollbar(int startOffset,int pointerDelta)
{
    Scroll(static_cast<float>(widget_scroll_rules::ApplyScrollbarThumbDrag(startOffset,pointerDelta,ScrollbarGeometry()))-scroll_);
}
void SystemPanelModel::UpdateSettings(const StatusBarSettings& settings)
{
    if(closed_||(settings_.trayOrder==settings.trayOrder&&settings_.pinnedTrayItems==settings.pinnedTrayItems))return;
    settings_.trayOrder=settings.trayOrder;settings_.pinnedTrayItems=settings.pinnedTrayItems;
    Refresh(available_);
}
void SystemPanelModel::Header(std::wstring title)
{
    Add("back",ui::Role::Icon,Rect(12,10,36,36),L"",L"\uE76B").tooltip=_LW("controlCenter.overview");Command("back",[this]{if(!ControlBack())Select(page_=="wifi-adapters"?"wifi":"");});
    const wchar_t* uri=page_=="audio"?L"ms-settings:sound":page_=="brightness"?L"ms-settings:display":page_.starts_with("wifi")?L"ms-settings:network-wifi":page_=="bluetooth"?L"ms-settings:bluetooth":page_=="power"?L"ms-settings:powersleep":nullptr;
    const float settingsLeft=scene_.width-(page_=="wifi"||page_=="bluetooth"?112.f:52.f);
    auto& node=Add("title",ui::Role::Text,Rect(58,10,uri?settingsLeft-(page_=="wifi"?110.f:70.f):scene_.width-126,36),std::move(title));node.bold=true;node.fontSize=16;
    if(uri)
    {
        auto& settings=Add("header.settings",ui::Role::Icon,Rect(settingsLeft,10,36,36),L"",L"\uE713");
        settings.tooltip=settings.accessibilityLabel=_LW("controlCenter.moreSettings");
        const std::wstring target=uri;Command("header.settings",[this,target]{OpenSettings(target.c_str());});
    }
    bodyStart_=56;
}
void SystemPanelModel::Radio(std::string_view key,D2D1_RECT_F rect,bool compact)
{
    const bool wifi=key=="wifi";const auto snapshot=source_.current?source_.current(wifi?"network.wifi":"bluetooth.devices"):std::nullopt;
    const auto v=snapshot&&snapshot->available?snapshot->value:j::Object();
    const auto& radios=Items(v,wifi?"interfaces":"radios");const JsonValue* radio=nullptr;
    for(const auto& r:radios)if(!wifi||interface_.empty()||j::String(r,"id")==interface_)
    {if(!radio)radio=&r;if(j::Flag(r,"available")){radio=&r;break;}}
    const auto* enabled=radio?radio->Find("enabled"):nullptr;
    const bool available=radio&&j::Flag(*radio,"available")&&enabled&&enabled->IsBoolean(),on=available&&enabled->boolean;
    const auto status=_LW(available?(on?"controlCenter.on":"controlCenter.off"):
        snapshot&&snapshot->available&&snapshot->error.empty()&&radios.empty()?"controlCenter.noHardware":"controlCenter.unavailable");
    std::wstring label=_LW(wifi?"statusBar.wifiControls":"statusBar.bluetoothControls");
    if(wifi&&radio)for(const auto& n:Items(*radio,"networks"))if(j::Flag(n,"connected")){label=Wide(j::String(n,"ssid"));break;}
    if(!wifi)for(const auto& d:Items(v,"devices"))if(j::Flag(d,"connected")){label=Wide(j::String(d,"name"));break;}
    const auto id="radio:"+std::string(key);auto& button=Add(id,ui::Role::Toggle,rect,compact?L"":label,wifi?L"\uE701":L"\uE702");
    button.enabled=available;button.selected=on;button.switchStyle=compact;button.tooltip=label+L" · "+status;
    button.accessibilityLabel=_LW(wifi?"statusBar.wifiControls":"statusBar.bluetoothControls");
    if(!compact)button.detail=status;
    const auto radioId=radio?j::String(*radio,"id"):std::string{};
    if(!radioId.empty())
    {const auto group=(wifi?"wifi.radio:":"bluetooth.radio:")+radioId;BindAction(id,group,radioId,{},group);}
    Command(id,[this,wifi,radioId,on]{Start(wifi?"network.wifi.setRadio":"bluetooth.setRadio",{{wifi?"interfaceId":"radioId",radioId},{"enabled",on?"0":"1"}});});
}
void SystemPanelModel::UnavailableControl(std::string_view key,std::wstring label,std::wstring glyph,std::wstring reason,float& y)
{
    const auto prefix=std::string(key);
    Add(prefix+".icon",ui::Role::Text,Rect(16,y,32,44),L"",std::move(glyph));
    Add(prefix+".label",ui::Role::Text,Rect(56,y,scene_.width-72,22),std::move(label)).fontSize=12;
    auto& detail=Add(prefix+".unavailable",ui::Role::Text,Rect(56,y+22,scene_.width-72,22),std::move(reason));
    detail.fontSize=12;detail.secondary=true;y+=52;
}
void SystemPanelModel::Volume(std::string_view direction,float& y)
{
    const auto prefix="audio."+std::string(direction);const auto state=source_.current?source_.current(prefix+".volume"):std::nullopt;
    const auto endpoint=state?j::String(state->value,"endpointId"):std::string{};
    const bool valid=state&&state->available&&!endpoint.empty()&&InRange(Number(state->value,"volume")),muted=valid&&j::Flag(state->value,"muted");
    const float volume=valid?static_cast<float>(Number(state->value,"volume")):0;
    const auto label=std::wstring(_LW(direction=="input"?"controlCenter.input":"statusBar.volume"));
    if(!valid)
    {
        UnavailableControl(prefix,label,direction=="input"?L"\uE720":L"\uE992",_LW("controlCenter.unavailable"),y);
        return;
    }
    const float sliderRight=scene_.width-(page_.empty()?58.f:16.f),valueRight=sliderRight-10;
    Add(prefix+".label",ui::Role::Text,Rect(16,y,valueRight-76,22),label).fontSize=12;
    auto& number=Add(prefix+".value",ui::Role::Text,Rect(valueRight-52,y,52,22),valid?Percent(volume*100):L"—");number.fontSize=12;number.trailing=true;y+=22;
    const wchar_t* speaker=muted?L"\uE74F":volume<=0?L"\uE992":volume<.34f?L"\uE993":volume<.67f?L"\uE994":L"\uE995";
    auto& mute=Add(prefix+".mute",ui::Role::Icon,Rect(20,y,40,40),L"",direction=="input"?(muted?L"\uF781":L"\uE720"):speaker);
    mute.enabled=valid;mute.tooltip=label+L" · "+_LW(muted?"controlCenter.unmute":"controlCenter.mute");
    if(valid)BindAction(prefix+".mute",prefix+".mute:"+endpoint,endpoint,prefix+".label");
    Command(prefix+".mute",[this,prefix,endpoint]{const auto s=source_.current?source_.current(prefix+".volume"):std::nullopt;if(s&&s->available&&j::String(s->value,"endpointId")==endpoint&&InRange(Number(s->value,"volume")))Start(prefix+".setMute",{{"muted",j::Flag(s->value,"muted")?"0":"1"}});});
    const auto sliderId=prefix+".volume:"+endpoint;
    auto& slider=Add(sliderId,ui::Role::Slider,Rect(68,y,sliderRight-68,40));slider.enabled=valid;slider.value=volume;slider.tooltip=slider.accessibilityLabel=label;
    sliderTargets_[sliderId]=valid?endpoint:std::string{};valueControls_[prefix+".value"]=sliderId;
    actions_[sliderId]=[this,prefix,endpoint](auto value){const auto s=source_.current?source_.current(prefix+".volume"):std::nullopt;if(value&&s&&s->available&&j::String(s->value,"endpointId")==endpoint&&InRange(Number(s->value,"volume")))Start(prefix+".setVolume",{{"volume",std::to_string(*value)}});};
    if(page_.empty()){Add("audio.more",ui::Role::Icon,Rect(scene_.width-52,y,36,40),L"",L"\uE76C").tooltip=_LW("statusBar.audioControls");Command("audio.more",[this]{Select("audio");});}y+=46;
}
void SystemPanelModel::Overview(float& y)
{
    std::vector<std::string> radios;
    if(settings_.wifiControls)radios.push_back("wifi");if(settings_.bluetoothControls)radios.push_back("bluetooth");
    const float radioWidth=radios.size()==2?(scene_.width-40)/2:scene_.width-32;
    for(std::size_t i=0;i<radios.size();++i)
    {
        const auto& key=radios[i];const float left=16+static_cast<float>(i)*(radioWidth+8);
        Radio(key,Rect(left,y,radioWidth-32,56),false);
        auto& radio=scene_.nodes.back();
        if(!radio.enabled)
        {
            const auto snapshot=source_.current?source_.current(key=="wifi"?"network.wifi":"bluetooth.devices"):std::nullopt;
            const auto value=snapshot&&snapshot->available?snapshot->value:j::Object();
            const auto& devices=Items(value,key=="wifi"?"interfaces":"radios");
            const bool denied=(snapshot&&snapshot->error=="accessDenied")||std::any_of(devices.begin(),devices.end(),[](const auto& device){return j::String(device,"error")=="accessDenied";});
            if(!denied)
            {
                // Missing hardware is an informational card, not a faded switch
                // with a chevron that suggests another working control.
                radio.bounds.right=left+radioWidth;radio.role=ui::Role::Card;radio.enabled=true;
                actions_.erase(radio.id);continue;
            }
        }
        radio.joinRight=true;
        auto& more=Add(key+".more",ui::Role::Button,Rect(left+radioWidth-32,y,32,56),L"",L"\uE76C");
        more.accent=scene_.Find("radio:"+key)->selected;more.joinLeft=true;more.tooltip=_LW(key=="bluetooth"?"statusBar.bluetoothControls":"statusBar.wifiControls");
        Command(key+".more",[this,key]{Select(key);});
    }
    if(!radios.empty())y+=68;
    if(settings_.audioControls)Volume("output",y);
    if(settings_.brightnessControls)
    {
    const auto value=Current("system.display.brightness");const auto& monitors=Items(value,"monitors");
    auto m=std::find_if(monitors.begin(),monitors.end(),[](const auto& v){return j::Flag(v,"available")&&!j::String(v,"id").empty()&&InRange(Number(v,"brightness"),100);});
    const bool valid=m!=monitors.end();const auto id=valid?j::String(*m,"id"):std::string{};
    if(valid)
    {
    const float level=valid?static_cast<float>(j::Numeric(*m,"brightness")):0;
    Add("brightness.label",ui::Role::Text,Rect(16,y,scene_.width-144,22),_LW("statusBar.brightnessControls")).fontSize=12;
    auto& number=Add("brightness.value",ui::Role::Text,Rect(scene_.width-120,y,52,22),valid?Percent(level):L"—");number.fontSize=12;number.trailing=true;y+=22;
    Add("brightness.icon",ui::Role::Text,Rect(20,y,40,40),L"",L"\uE706");
    const auto sliderId="brightness.level:"+id;
    auto& slider=Add(sliderId,ui::Role::Slider,Rect(68,y,scene_.width-126,40));slider.enabled=valid;slider.value=level/100;slider.tooltip=slider.accessibilityLabel=_LW("statusBar.brightnessControls");
    sliderTargets_[sliderId]=valid?id:std::string{};valueControls_["brightness.value"]=sliderId;
    actions_[sliderId]=[this,id](auto v){const auto current=Current("system.display.brightness");const auto& displays=Items(current,"monitors");if(v&&std::any_of(displays.begin(),displays.end(),[&](const auto& d){return j::String(d,"id")==id&&j::Flag(d,"available");}))Start("system.display.setBrightness",{{"monitorId",id},{"brightness",std::to_string(*v*100)}});};
    Add("brightness.more",ui::Role::Icon,Rect(scene_.width-52,y,36,40),L"",L"\uE76C").tooltip=_LW("statusBar.brightnessControls");Command("brightness.more",[this]{Select("brightness");});y+=48;
    }
    }
    if(settings_.powerControls||source_.nativeControls)
    {Add("divider",ui::Role::Separator,Rect(16,y,scene_.width-32,1));y+=8;}
    if(settings_.powerControls)
    {
    const auto power=Current("system.power.plans");const auto* battery=power.Find("batteryPercent");
    const bool valid=j::Flag(power,"batteryPresent")&&battery&&battery->IsNumber()&&InRange(battery->number,100);
    if(valid)
    {
    const auto visual=ResolveStatusBarBatteryVisual(valid?battery->number:-1,j::Flag(power,"charging"),j::Flag(power,"onAC"));
    const wchar_t glyph=valid?(battery->number==100?L'\uE83F':static_cast<wchar_t>(0xE850+std::clamp(static_cast<int>(battery->number/10),0,9))):L'\uE996';
    auto& batteryNode=Add("battery",ui::Role::Text,Rect(16,y,scene_.width-128,36),valid?Percent(battery->number):L"—",std::wstring(1,glyph));
    batteryNode.batteryStyle=true;batteryNode.value=valid?static_cast<float>(battery->number/100):-1;
    batteryNode.charging=visual.tone==StatusBarBatteryTone::Charging;
    batteryNode.pluggedIn=visual.pluggedIn;
    batteryNode.positiveGlyph=visual.tone==StatusBarBatteryTone::FullyCharged;
    batteryNode.tooltip=batteryNode.accessibilityLabel=std::wstring(_LW(visual.label))+L" · "+batteryNode.text;
    }
    Add("power.more",ui::Role::Icon,Rect(scene_.width-(source_.nativeControls?100.f:52.f),y,36,36),L"",L"\uE7E8").tooltip=_LW("statusBar.powerControls");Command("power.more",[this]{Select("power");});
    }
    if(source_.nativeControls)
    {
        Add("system.settings",ui::Role::Icon,Rect(scene_.width-52,y,36,36),L"",L"\uE713").tooltip=_LW("statusBar.nativeControls");
        Command("system.settings",[this]{const auto open=source_.nativeControls;if(open)open();});
    }
    if(settings_.powerControls||source_.nativeControls)y+=40;
}
void SystemPanelModel::Audio(float& y)
{
    const auto value=Current("audio.devices");
    // Keep both everyday adjustments ahead of arbitrarily long endpoint lists.
    // Device selection remains complete in the same accessible scroll flow.
    Volume("output",y);Volume("input",y);
    Add("audio.divider",ui::Role::Separator,Rect(16,y,scene_.width-32,1));y+=12;
    for(const auto* direction:{"output","input"})
    {
        Add(std::string(direction)+".heading",ui::Role::Text,Rect(16,y,scene_.width-32,24),_LW(std::string_view(direction)=="input"?"controlCenter.input":"controlCenter.output")).bold=true;y+=30;
        const auto prefix="audio."+std::string(direction);const auto active=j::String(Current((prefix+".volume").c_str()),"endpointId");
        bool found=false;for(const auto& d:system_control::AudioPresentationDevices(value,direction))
        {
            found=true;const auto endpoint=j::String(d,"id"),id=prefix+".device:"+endpoint;
            auto& node=Add(id,ui::Role::ListItem,Rect(16,y,scene_.width-32,44),Wide(j::String(d,"name")),std::string_view(direction)=="input"?L"\uE720":L"\uE767");node.selected=endpoint==active;
            BindAction(id,prefix+".selectDevice",endpoint);
            Command(id,[this,prefix,endpoint]{Start(prefix+".selectDevice",{{"endpointId",endpoint}});});y+=48;
        }
        if(!found){Add(prefix+".empty",ui::Role::Text,Rect(16,y,scene_.width-32,32),_LW("controlCenter.unavailable"));y+=36;}
        y+=8;
    }
}
void SystemPanelModel::Brightness(float& y)
{
    const auto value=Current("system.display.brightness");const auto& monitors=Items(value,"monitors");
    for(const auto& m:monitors)
    {
        const auto id=j::String(m,"id");const bool valid=!id.empty()&&j::Flag(m,"available")&&InRange(Number(m,"brightness"),100);
        if(!valid)
        {
            UnavailableControl("display:"+id,Wide(j::String(m,"name")),L"\uE7F4",_LW("controlCenter.brightnessUnavailable"),y);
            continue;
        }
        Add("display:"+id,ui::Role::Text,Rect(16,y,scene_.width-102,30),Wide(j::String(m,"name")),L"\uE7F4");
        Add("display.value:"+id,ui::Role::Text,Rect(scene_.width-78,y,52,30),valid?Percent(Number(m,"brightness")):L"—").trailing=true;y+=32;
        const auto sliderId="display.level:"+id;
        auto& slider=Add(sliderId,ui::Role::Slider,Rect(16,y,scene_.width-32,40));slider.enabled=valid;slider.value=valid?static_cast<float>(Number(m,"brightness")/100):0;slider.tooltip=slider.accessibilityLabel=Wide(j::String(m,"name"))+L" · "+_LW("statusBar.brightnessControls");
        sliderTargets_[sliderId]=valid?id:std::string{};valueControls_["display.value:"+id]=sliderId;
        actions_[sliderId]=[this,id](auto v){const auto current=Current("system.display.brightness");const auto& displays=Items(current,"monitors");if(v&&std::any_of(displays.begin(),displays.end(),[&](const auto& d){return j::String(d,"id")==id&&j::Flag(d,"available");}))Start("system.display.setBrightness",{{"monitorId",id},{"brightness",std::to_string(*v*100)}});};y+=48;
    }
    if(monitors.empty()){Add("empty",ui::Role::Text,Rect(16,y,scene_.width-32,56),_LW("controlCenter.unsupportedHint"));y+=64;}
}
void SystemPanelModel::WifiPage(float& y)
{
    const auto snapshot=source_.current?source_.current("network.wifi"):std::nullopt;
    const auto value=snapshot&&snapshot->available?snapshot->value:j::Object();const auto& adapters=Items(value,"interfaces");
    if(page_=="wifi-hidden"){ControlForm(y);return;}
    if(page_=="wifi-adapters")
    {
        for(const auto& a:adapters){const auto id=j::String(a,"id");auto& n=Add("adapter:"+id,ui::Role::ListItem,Rect(16,y,scene_.width-32,48),Wide(j::String(a,"name")));n.selected=id==interface_;Command(n.id,[this,id]{interface_=id;network_.clear();Select("wifi");});y+=50;}return;
    }
    if(adapters.size()>1){Add("wifi.adapter",ui::Role::Button,Rect(16,y,scene_.width-32,38),Wide(j::String(Wifi(),"name")),L"\uE701");Command("wifi.adapter",[this]{Select("wifi-adapters");});y+=46;}
    Radio("wifi",Rect(scene_.width-64,10,48,36),true);
    const auto current=Wifi();const auto networks=system_control::WifiPresentationNetworks(current);
    const auto* enabled=current.Find("enabled");
    const bool available=snapshot&&snapshot->available&&j::Flag(current,"available")&&enabled&&enabled->IsBoolean();
    const bool powered=available&&enabled->boolean;
    const bool denied=(snapshot&&snapshot->error=="accessDenied")||j::String(current,"error")=="accessDenied";
    const auto radioGroup="wifi.radio:"+interface_;
    auto& scan=Add("wifi.scan",ui::Role::Icon,Rect(scene_.width-152,10,36,36),L"",L"\uE72C");scan.enabled=powered;
    scan.tooltip=scan.accessibilityLabel=_LW("controlCenter.scan");
    if(!interface_.empty())BindAction("wifi.scan","wifi.scan:"+interface_,interface_,{},radioGroup);
    Command("wifi.scan",[this]{Start("network.wifi.scan",{{"interfaceId",interface_}});});
    if(denied)
    {Add("wifi.denied",ui::Role::Text,Rect(16,y,scene_.width-32,44),_LW("controlCenter.locationDenied")).fontSize=12;y+=48;Add("wifi.location",ui::Role::Button,Rect(16,y,scene_.width-32,36),_LW("controlCenter.locationSettings"));Command("wifi.location",[this]{OpenSettings(L"ms-settings:privacy-location");});y+=44;}
    for(const auto& n:networks)
    {
        const auto id=j::String(n,"id"),profile=j::String(n,"profileName");const bool connected=j::Flag(n,"connected"),open=id==network_;
        const auto card="wifi.card:"+id;const float cardTop=y;
        Add(card,ui::Role::Card,Rect(16,y,scene_.width-32,open?96.f:52.f)).hoverGroup=card;
        auto& row=Add("wifi.network:"+id,ui::Role::ListItem,Rect(16,y,scene_.width-32,52),Wide(j::String(n,"ssid")),L"\uE701");row.detail=(connected?std::wstring(_LW("controlCenter.connected"))+L" · ":L"")+Percent(j::Numeric(n,"signal"));
        row.hoverGroup=card;
        BindAction("wifi.connect:"+id,"wifi.connection:"+interface_,id,row.id,radioGroup);
        if(!profile.empty())BindAction("wifi.forget:"+id,"wifi.forget:"+interface_,profile,row.id,radioGroup);
        Command(row.id,[this,id]{CancelControlInput();network_=network_==id?std::string{}:id;});y+=52;
        if(open)
        {
            const bool editing=controlDraft_&&controlDraft_->request.name=="network.wifi.connect"&&!controlDraft_->hidden&&controlDraft_->request.arguments["networkId"]==id;
            if(editing)
            {
                ControlForm(y,true);
                for(auto& node:scene_.nodes)if(node.id==card)node.bounds.bottom=y;
                for(auto& node:scene_.nodes)if(node.bounds.top>=cardTop&&node.bounds.bottom<=y)node.hoverGroup=card;
                y+=8;continue;
            }
            auto& button=Add("wifi.connect:"+id,ui::Role::Button,Rect(scene_.width-158,y,134,36),_LW(connected?"controlCenter.disconnect":"controlCenter.connect"));button.enabled=powered&&(connected||j::Flag(n,"connectable"));
            button.hoverGroup=card;
            const auto security=j::String(n,"security");Command(button.id,[this,id,profile,connected,security]{if(connected)Start("network.wifi.disconnect",{{"interfaceId",interface_}});else if(!profile.empty())Start("network.wifi.connect",{{"interfaceId",interface_},{"profileName",profile}});else if(security=="system")OpenSettings(L"ms-settings:network-wifi");else Start("network.wifi.connect",{{"interfaceId",interface_},{"networkId",id}});});
            if(!profile.empty()){auto& forget=Add("wifi.forget:"+id,ui::Role::Icon,Rect(24,y,36,36),L"",L"\uE74D");forget.hoverGroup=card;forget.tooltip=_LW("controlCenter.forget");Command(forget.id,[this,profile]{Start("network.wifi.forget",{{"interfaceId",interface_},{"profileName",profile}});});}y+=44;
        }
        y+=8;
    }
    if(networks.empty()&&!denied)
    {
        const char* label=!snapshot||!snapshot->available||!snapshot->error.empty()?"controlCenter.unavailable":adapters.empty()?"controlCenter.noHardware":
            !available||!j::String(current,"error").empty()?"controlCenter.unavailable":powered?"controlCenter.noDevices":"controlCenter.off";
        Add("wifi.empty",ui::Role::Text,Rect(16,y,scene_.width-32,48),_LW(label));y+=56;
    }
    Add("wifi.hidden",ui::Role::Button,Rect(16,y,scene_.width-32,36),_LW("controlCenter.hiddenNetwork"),L"\uE72E").enabled=powered;
    if(!interface_.empty())BindAction("wifi.hidden","wifi.connection:"+interface_,interface_,{},radioGroup);
    Command("wifi.hidden",[this]{Start("network.wifi.connect",{{"interfaceId",interface_},{"hidden","1"}});});
    y+=44;
}
void SystemPanelModel::Bluetooth(float& y)
{
    const auto value=Current("bluetooth.devices");const auto& radios=Items(value,"radios");Radio("bluetooth",Rect(scene_.width-64,10,48,36),true);
    const bool available=std::any_of(radios.begin(),radios.end(),[](const auto& r){return j::Flag(r,"available");});
    const bool powered=std::any_of(radios.begin(),radios.end(),[](const auto& r){return j::Flag(r,"available")&&j::Flag(r,"enabled");});
    if(radios.size()>1)for(const auto& r:radios){const auto id=j::String(r,"id"),group="bluetooth.radio:"+id;auto& n=Add(group,ui::Role::Toggle,Rect(16,y,scene_.width-32,42),Wide(j::String(r,"name")));n.switchStyle=true;n.selected=j::Flag(r,"enabled");n.enabled=j::Flag(r,"available");if(n.enabled)BindAction(n.id,group,id,{},group);const bool on=n.selected;Command(n.id,[this,id,on]{Start("bluetooth.setRadio",{{"radioId",id},{"enabled",on?"0":"1"}});});y+=50;}
    const auto& devices=Items(value,"devices");
    for(const auto& d:devices)
    {
        const auto id=j::String(d,"id");const bool connected=j::Flag(d,"connected"),supported=j::Flag(d,"canConnect"),open=id==bluetooth_;
        // The public snapshot does not promise adapter ownership. A single
        // radio is unambiguous; otherwise only an explicit identity is used.
        auto radioId=j::String(d,"radioId");if(radioId.empty()&&radios.size()==1)radioId=j::String(radios.front(),"id");
        const auto radioGroup=radioId.empty()?std::string("bluetooth.unknown"):"bluetooth.radio:"+radioId;
        const bool devicePowered=radioId.empty()?powered:std::any_of(radios.begin(),radios.end(),[&](const auto& r){return j::String(r,"id")==radioId&&j::Flag(r,"available")&&j::Flag(r,"enabled");});
        const auto card="bluetooth.card:"+id;
        Add(card,ui::Role::Card,Rect(16,y,scene_.width-32,open?98.f:54.f)).hoverGroup=card;
        auto& row=Add("bluetooth.device:"+id,ui::Role::ListItem,Rect(16,y,scene_.width-32,54),Wide(j::String(d,"name")),L"\uE702");
        row.hoverGroup=card;
        row.enabled=devicePowered;
        if(supported)BindAction("bluetooth.connect:"+id,"bluetooth.connection:"+id,id,row.id,radioGroup);
        row.detail=_LW(connected?"controlCenter.connected":"controlCenter.connect");if(const auto* level=d.Find("batteryPercent");level&&level->IsNumber())row.detail+=L" · "+Percent(level->number);
        Command(row.id,[this,id]{bluetooth_=bluetooth_==id?std::string{}:id;});y+=54;
        if(open)
        {
            const float buttonWidth=(std::min)(supported?134.f:196.f,scene_.width-48);
            auto& button=Add("bluetooth.connect:"+id,ui::Role::Button,Rect(scene_.width-24-buttonWidth,y,buttonWidth,36),_LW(!supported?"settings.taskbar.systemSettings.open":connected?"controlCenter.disconnect":"controlCenter.connect"));button.enabled=devicePowered;
            button.hoverGroup=card;
            Command(button.id,[this,id,connected,supported]{if(supported)Start(connected?"bluetooth.disconnect":"bluetooth.connect",{{"deviceId",id}});else OpenSettings(L"ms-settings:bluetooth");});y+=44;
        }
        y+=8;
    }
    if(devices.empty()){Add("bluetooth.empty",ui::Role::Text,Rect(16,y,scene_.width-32,52),_LW(!available?"controlCenter.unavailable":!powered?"controlCenter.off":"controlCenter.noDevices"));y+=60;}
}
void SystemPanelModel::Power(float& y)
{
    const auto value=Current("system.power.plans");
    const auto& plans=Items(value,"plans");
    if(plans.size()>1)
    {
        Add("power.plans.heading",ui::Role::Text,Rect(16,y,scene_.width-32,28),_LW("controlCenter.powerPlan")).bold=true;y+=34;
        for(const auto& p:plans){const auto id=j::String(p,"id");auto& n=Add("power.plan:"+id,ui::Role::ListItem,Rect(16,y,scene_.width-32,44),Wide(j::String(p,"name")),L"\uE945");n.selected=j::Flag(p,"active");BindAction(n.id,"power.plan",id);Command(n.id,[this,id]{Start("system.power.setPlan",{{"planId",id}});});y+=48;}
        y+=8;
    }
    if(j::Flag(value,"modeSupported"))
    {
        Add("power.modes.heading",ui::Role::Text,Rect(16,y,scene_.width-32,28),_LW("controlCenter.powerMode")).bold=true;y+=34;
        for(const auto* mode:{"efficiency","balanced","performance"})
        {auto& n=Add(std::string("power.mode:")+mode,ui::Role::ListItem,Rect(16,y,scene_.width-32,42),_LW((std::string("controlCenter.")+mode).c_str()));n.selected=j::String(value,j::Flag(value,"onAC")?"acMode":"dcMode")==mode;BindAction(n.id,"power.mode",mode);Command(n.id,[this,mode]{Start("system.power.setMode",{{"mode",mode}});});y+=46;}
    }
    y+=8;int index=0;const float w=(scene_.width-40)/2;
    for(const auto* action:{"lock","sleep","restart","shutdown"}){const std::string id=std::string("power.")+action;Add(id,ui::Role::Button,Rect(16+(index%2)*(w+8),y+(index/2)*48,w,40),_LW((std::string("controlCenter.")+action).c_str()));BindAction(id,"power.action",action);Command(id,[this,action]{Start(std::string("system.power.")+action);});++index;}y+=96;
}
void SystemPanelModel::PrepareMedia()
{
    mediaState_=settings_.mediaControls&&source_.media?source_.media():std::nullopt;
    if(!mediaState_||!mediaState_->available||mediaState_->sessions.empty()){media_.clear();return;}
    auto selected=std::find_if(mediaState_->sessions.begin(),mediaState_->sessions.end(),[&](const auto& session){return session.id==mediaState_->currentSessionId;});
    if(selected==mediaState_->sessions.end())selected=mediaState_->sessions.begin();media_=selected->id;
    for(const auto* command:{"previous","toggle","next"})
    {const auto id=std::string("media.")+command;BindAction(id,id+":"+media_,media_);}
}
void SystemPanelModel::Media(float& y)
{
    if(!mediaState_||!mediaState_->available||mediaState_->sessions.empty())return;
    const auto selected=std::find_if(mediaState_->sessions.begin(),mediaState_->sessions.end(),[&](const auto& session){return session.id==media_;});
    if(selected==mediaState_->sessions.end())return;
    auto& art=Add("media.artwork",ui::Role::Image,Rect(16,y,32,32),L"",L"\uE8D6");
    if(const auto a=source_.artwork?source_.artwork():std::nullopt;a&&a->available&&a->sessionId==media_&&a->pixels&&wr::IsValidWidgetRuntimeImage(*a->pixels))
    {auto image=std::make_shared<ui::Image>();image->width=a->pixels->width;image->height=a->pixels->height;image->stride=a->pixels->stride;image->pixels.resize(a->pixels->bgraPremultiplied.size()/4);std::memcpy(image->pixels.data(),a->pixels->bgraPremultiplied.data(),a->pixels->bgraPremultiplied.size());art.image=std::move(image);}
    const float controlsLeft=scene_.width-120;
    auto& title=Add("media.title",ui::Role::Text,Rect(60,y,controlsLeft-68,32),Wide(selected->title.empty()?selected->sourceName:selected->title));
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
void SystemPanelModel::Finish(float bodyEnd,bool withMedia,float minimumBodyHeight,float maximumBodyHeight)
{
    float mediaHeight=0;if(withMedia&&mediaState_&&mediaState_->available&&!mediaState_->sessions.empty())mediaHeight=48;
    float maxBody=(std::max)(32.f,available_-mediaHeight-(mediaHeight?8:0));
    if(maximumBodyHeight>0)maxBody=(std::min)(maxBody,maximumBodyHeight);
    const float end=(std::min)((std::max)(bodyEnd+8,minimumBodyHeight),maxBody);maxScroll_=(std::max)(0.f,bodyEnd+8-end);scroll_=std::clamp(scroll_,0.f,maxScroll_);
    if(end<bodyStart_+48)bodyStart_=0;
    const auto clip=Rect(bodyLeft_,bodyStart_,scene_.width-bodyLeft_,(std::max)(0.f,end-bodyStart_-8));scrollViewport_=clip;
    for(auto& n:scene_.nodes)if(n.bounds.top>=bodyStart_&&n.bounds.left>=bodyLeft_){n.clip=clip;n.bounds.top-=scroll_;n.bounds.bottom-=scroll_;for(auto& path:n.paths)for(auto& p:path)p.y-=scroll_;}
    scene_.cards.push_back(Rect(0,0,scene_.width,end));scene_.height=end;
    if(maxScroll_>0){const auto axis=ScrollbarGeometry();Add("scrollbar",ui::Role::Scrollbar,Rect(scene_.width-4,static_cast<float>(axis.thumbStart),2,static_cast<float>(axis.ThumbExtent())));}
    if(mediaHeight){float y=end+16;Media(y);scene_.cards.push_back(Rect(0,end+8,scene_.width,mediaHeight));scene_.height=end+8+mediaHeight;}
}
void SystemPanelModel::Refresh(float availableHeight,float availableWidth)
{
    if(closed_||dismissRequested_)return;available_=std::isfinite(availableHeight)?(std::max)(96.f,availableHeight):800;scene_={};actions_.clear();sliderTargets_.clear();valueControls_.clear();actionBindings_.clear();bodyStart_=16;bodyLeft_=0;scrollViewport_={};
    if(std::isfinite(availableWidth)&&availableWidth>0)availableWidth_=(std::max)(200.f,availableWidth);
    const auto completions=source_.completions?source_.completions():std::vector<system_control::Completion>{};
    for(const auto& completion:completions)std::erase_if(pendingValues_,[&](const auto& item){return item.second.task==completion.id;});
    if(page_=="media"||(page_=="audio"&&!settings_.audioControls)||(page_=="brightness"&&!settings_.brightnessControls)||
        (page_.starts_with("wifi")&&!settings_.wifiControls)||(page_=="bluetooth"&&!settings_.bluetoothControls)||(page_=="power"&&!settings_.powerControls&&!controlDraft_)){CancelControlInput();++navigation_;page_.clear();ClearError();}
    SyncSubscriptions();
    if(subscriptions_.contains("network.wifi"))
    {
        const auto wifi=Current("network.wifi");const auto& adapters=Items(wifi,"interfaces");
        if(std::none_of(adapters.begin(),adapters.end(),[this](const auto& a){return j::String(a,"id")==interface_;}))
        {CancelControlInput();if(page_=="wifi-hidden")page_="wifi";interface_=adapters.empty()?std::string{}:j::String(adapters.front(),"id");network_.clear();}
        if(controlDraft_&&controlDraft_->request.name=="network.wifi.connect")
        {
            const auto adapter=Wifi();const auto networks=system_control::WifiPresentationNetworks(adapter);
            const auto& args=controlDraft_->request.arguments;
            const bool targetPresent=controlDraft_->hidden||std::any_of(networks.begin(),networks.end(),[&](const auto& n){return j::String(n,"id")==args.at("networkId")&&j::String(n,"security")==args.at("security");});
            if(!j::Flag(adapter,"available")||!j::Flag(adapter,"enabled")||!targetPresent)
            {CancelControlInput();if(page_=="wifi-hidden"){page_="wifi";scroll_=0;}}
        }
        if(scan_&&page_=="wifi"&&!interface_.empty()&&j::Flag(Wifi(),"enabled"))
        {
            scan_=false;
            // Returning to Wi-Fi must not queue another automatic scan behind
            // an operation already using this adapter.
            const bool busy=std::any_of(pendingActions_.begin(),pendingActions_.end(),[this](const auto& entry){return entry.second.topic=="network.wifi"&&entry.second.device==interface_;});
            if(!busy){BindAction("wifi.scan","wifi.scan:"+interface_,interface_,{},"wifi.radio:"+interface_);Start("network.wifi.scan",{{"interfaceId",interface_}},"wifi.scan");}
        }
    }
    if(action_==StatusBarAction::Tray){Tray();return;}if(action_==StatusBarAction::Calendar){Calendar();return;}if(IsSystemResourceAction(action_)){Resources();return;}
    scene_.width=384;
    // The old offline "media" preset still selects the overview; media has no detail page.
    if(page_=="media")page_.clear();
    PrepareMedia();
    const char* title=page_=="audio"?"statusBar.audioControls":page_=="brightness"?"statusBar.brightnessControls":page_.starts_with("wifi")?"statusBar.wifiControls":page_=="bluetooth"?"statusBar.bluetoothControls":"statusBar.powerControls";
    std::uint64_t inlineCompletion=0;
    for(const auto& completion:completions)if(controlDraft_&&controlDraft_->task&&completion.id==controlDraft_->task)
    {
        inlineCompletion=completion.id;
        controlDraft_->task=0;
        if((completion.ok||completion.error=="canceled")&&controlDraft_->returnRoute==ControlDraft::ReturnRoute::ClosePanel)
        {controlDraft_.reset();dismissRequested_=true;return;}
        if(completion.ok){const bool hidden=controlDraft_->hidden;controlDraft_.reset();if(hidden){page_="wifi";scroll_=0;}}
        else if(completion.error=="canceled"){controlDraft_.reset();if(page_=="wifi-hidden")page_="wifi";}
        else if(completion.error=="passwordRequired")controlDraft_->error=_LW("controlCenter.invalidPassword");
        else if(completion.error=="accessDenied")controlDraft_->error=_LW("controlCenter.accessDenied");
        else if(completion.error=="timeout")controlDraft_->error=_LW("controlCenter.timeout");
        else controlDraft_->error.clear();
    }
    const bool standalone=controlDraft_&&controlDraft_->returnRoute==ControlDraft::ReturnRoute::ClosePanel;
    if(standalone)
    {
        auto& heading=Add("title",ui::Role::Text,Rect(16,10,scene_.width-32,36),_LW(title));heading.bold=true;heading.fontSize=16;bodyStart_=56;
    }
    else if(!page_.empty())Header(_LW(page_=="wifi-hidden"?"controlCenter.hiddenNetwork":title));float y=bodyStart_;
    if(controlDraft_&&controlDraft_->request.name!="network.wifi.connect")ControlForm(y);
    else if(page_.empty())Overview(y);else if(page_=="audio")Audio(y);else if(page_=="brightness")Brightness(y);else if(page_.starts_with("wifi"))WifiPage(y);else if(page_=="bluetooth")Bluetooth(y);else if(page_=="power")Power(y);
    PrunePendingActions();
    for(const auto& completion:completions)
    {
        bool relevant=true;
        std::optional<PendingAction> origin;
        const auto pending=std::find_if(pendingActions_.begin(),pendingActions_.end(),[&](const auto& item){return item.second.task==completion.id;});
        if(pending!=pendingActions_.end())
        {
            const auto binding=actionBindings_.find(pending->second.control);
            // Returning to a page restores its guard, but a completion from an
            // earlier visit must not display an error against a new selection.
            relevant=pending->second.navigation==navigation_&&binding!=actionBindings_.end()&&binding->second.group==pending->first&&binding->second.target==pending->second.target;
            if(relevant)origin=pending->second;
            pendingActions_.erase(pending);
        }
        if(feedback_.Take(completion.id)&&completion.id!=inlineCompletion&&relevant&&!completion.ok&&completion.error!="canceled")
        {
            ClearError();
            // Generic failures provide no useful next step. Restore the real
            // device state without adding a banner or replacing its details.
            if(completion.error=="accessDenied")error_=_LW("controlCenter.accessDenied");
            else if(completion.error=="timeout")error_=_LW("controlCenter.timeout");
            if(!error_.empty()&&origin){errorControl_=origin->control;errorTarget_=origin->target;errorGroup_=actionBindings_.at(errorControl_).group;}
        }
    }
    ApplyError(y);
    Finish(y,settings_.mediaControls&&!standalone);
    ApplyPendingActions();
    std::erase_if(pendingValues_,[&](const auto& entry){const auto* node=scene_.Find(entry.first);if(!node)return false;const auto target=sliderTargets_.find(entry.first);return !node->enabled||target==sliderTargets_.end()||target->second!=entry.second.target;});
    for(auto& node:scene_.nodes)
    {
        if(const auto found=pendingValues_.find(node.id);found!=pendingValues_.end())
        {node.value=found->second.value;node.tooltip=node.accessibilityLabel+L" · "+Percent(node.value*100);}
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
std::optional<SystemPanelModel::TrayDropTarget> SystemPanelModel::ResolveTrayDrop(std::string_view key,D2D1_POINT_2F p) const
{
    if(closed_||action_!=StatusBarAction::Tray||!std::isfinite(p.x)||!std::isfinite(p.y)||!source_.tray)return {};
    if(std::none_of(scene_.cards.begin(),scene_.cards.end(),[p](const auto& r){return p.x>=r.left&&p.x<r.right&&p.y>=r.top&&p.y<r.bottom;}))return {};
    const auto readTray=source_.tray;const auto snapshot=readTray();
    if(closed_)return {};
    const auto currentTarget=[&](std::string_view identity) {
        return std::any_of(snapshot.icons.begin(),snapshot.icons.end(),[&](const auto& icon) {
            return icon.key==identity&&!(icon.state&NIS_HIDDEN)&&!icon.persistentKey.empty()&&!tray::DuplicatesControlCenter(icon)&&
                std::find(settings_.pinnedTrayItems.begin(),settings_.pinnedTrayItems.end(),icon.persistentKey)==settings_.pinnedTrayItems.end();
        });
    };
    std::string before;D2D1_RECT_F indicator{};const ui::Node* last=nullptr;
    for(const auto& n:scene_.nodes)if(n.id.starts_with("tray:"))
    {
        const auto identity=std::string_view(n.id).substr(5);
        if(!currentTarget(identity))continue;
        auto visible=n.bounds;
        if(n.clip.bottom>n.clip.top){visible.top=(std::max)(visible.top,n.clip.top);visible.bottom=(std::min)(visible.bottom,n.clip.bottom);}
        if(visible.bottom<=visible.top)continue;
        if(p.y<n.bounds.top||(p.y<n.bounds.bottom&&p.x<(n.bounds.left+n.bounds.right)/2))
        {before=n.id.substr(5);indicator={n.bounds.left-2,visible.top,n.bounds.left,visible.bottom};break;}
        last=&n;
    }
    if(before.empty()&&last)
    {
        indicator={last->bounds.right,last->bounds.top,last->bounds.right+2,last->bounds.bottom};
        if(last->clip.bottom>last->clip.top){indicator.top=(std::max)(indicator.top,last->clip.top);indicator.bottom=(std::min)(indicator.bottom,last->clip.bottom);}
        // At the bottom of a scrolled grid append after the last visible item,
        // before the next offscreen item instead of silently appending globally.
        bool passed=false;for(const auto& n:scene_.nodes){if(&n==last){passed=true;continue;}if(passed&&n.id.starts_with("tray:")&&currentTarget(std::string_view(n.id).substr(5))){before=n.id.substr(5);break;}}
    }
    if(!last&&before.empty())indicator=Rect(10,10,scene_.width-20,(std::max)(2.f,scene_.height-20));
    TrayDropTarget target{settings_,indicator};
    if(!tray::PlaceIcon(target.settings,snapshot,key,false,before))return {};
    return target;
}
std::optional<D2D1_RECT_F> SystemPanelModel::TrayDropIndicator(std::string_view key,D2D1_POINT_2F p) const
{const auto target=ResolveTrayDrop(key,p);return target?std::optional<D2D1_RECT_F>(target->indicator):std::nullopt;}
bool SystemPanelModel::Drop(std::string_view key,D2D1_POINT_2F p)
{
    const auto target=ResolveTrayDrop(key,p);if(!target)return false;settings_=target->settings;
    const auto changed=source_.trayChanged;if(changed)changed(settings_);
    if(closed_)return true;
    Refresh(available_);return true;
}
void SystemPanelModel::Calendar()
{
    calendarEvents_.clear();
    scene_.width=(std::min)(calendarStacked_?384.f:720.f,availableWidth_);
    auto today=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(today))today=calendar::CalendarService::CurrentLocalNow().date;
    if(!calendar::CalendarService::GetDateInfo(date_))date_=today;
    if(!calendar::CalendarService::GetDateInfo(month_))month_=date_.substr(0,7)+"-01";
    const auto language=Locale::Instance().GetEffectiveLanguage();const auto locale=Wide(language);
    const auto revision=language+":"+(source_.calendar.secondaryRevision?source_.calendar.secondaryRevision():std::string{});
    if(revision!=calendarDetailsRevision_)
    {
        calendarDetails_.clear();calendarAnnotations_.clear();calendarAnnotationKey_.clear();
        calendarDetailsRevision_=revision;
    }
    std::string monthStart=month_;
    if(calendarDisplaySecondary_)
    {
        if(!calendarDetails_.contains(month_)&&source_.calendar.secondaryAnnotations)
            for(auto& annotation:source_.calendar.secondaryAnnotations(month_,month_))
                if(annotation.calendarAvailable)calendarDetails_.emplace(annotation.date,std::move(annotation));
        if(closed_)return;
        const auto anchor=calendarDetails_.find(month_);
        const auto start=anchor!=calendarDetails_.end()&&anchor->second.day>=1&&anchor->second.day<=31?
            calendar::CalendarService::AddDays(month_,1-anchor->second.day):std::nullopt;
        if(start)monthStart=*start;
        else
        {
            calendarDisplaySecondary_=false;
            month_=date_.substr(0,7)+"-01";monthStart=month_;
        }
    }
    const auto month=calendar::CalendarService::GetDateInfo(monthStart);
    if(!month)return;
    const bool two=scene_.width>=560;
    if(CalendarEditing()&&!two&&!calendarStacked_)
    {
        bodyLeft_=0;bodyStart_=56;
        const float end=CalendarEditor(scene_.width);Finish(end,false);return;
    }
    const float monthLeft=16,monthTop=16;
    const float monthWidth=two?(scene_.width-48)/2:scene_.width-32,toolbar=monthWidth<280?72.f:36.f;
    const float buttonsTop=monthTop+(toolbar>36?36.f:0.f),gridTop=monthTop+toolbar+12;
    const auto dates=ui::DateTimePicker::MonthDates(monthStart);
    const auto first=**std::find_if(dates.begin(),dates.end(),[](const auto& date){return date.has_value();});
    const auto last=**std::find_if(dates.rbegin(),dates.rend(),[](const auto& date){return date.has_value();});
    std::set<std::string> requestedDates;
    for(const auto& date:dates)if(date)requestedDates.insert(*date);
    for(int i=0;i<3;++i)if(const auto date=calendar::CalendarService::AddDays(date_,i))requestedDates.insert(*date);
    std::map<std::string,std::vector<calendar::CalendarEvent>> eventsByDate;
    if(source_.calendar.eventsInRange&&!requestedDates.empty())
    {
        // The live callback filters CalendarService's memory snapshot once;
        // month dots and the selected/nearby agenda observe the same revision.
        const auto events=source_.calendar.eventsInRange(*requestedDates.begin(),*requestedDates.rbegin());
        if(closed_)return;
        for(const auto& event:events)if(requestedDates.contains(event.date))eventsByDate[event.date].push_back(event);
    }
    else if(source_.calendar.events)
        for(const auto& date:requestedDates)eventsByDate[date]=source_.calendar.events(date);
    const auto annotationKey=first+":"+last+":"+revision;
    if(annotationKey!=calendarAnnotationKey_)
    {
        calendarAnnotations_.clear();
        calendarDetails_.clear();
        if(source_.calendar.secondaryAnnotations)
        {
            for(auto& annotation:source_.calendar.secondaryAnnotations(first,last))
                if(annotation.calendarAvailable&&!annotation.fullDate.empty())
                {
                    calendarAnnotations_[annotation.date]=annotation.fullDate;
                    calendarDetails_.emplace(annotation.date,std::move(annotation));
                }
        }
        else if(source_.calendar.secondaryDates)calendarAnnotations_=source_.calendar.secondaryDates(first,last);
        else if(source_.calendar.secondaryDate)
            for(const auto& date:dates)if(date)calendarAnnotations_[*date]=source_.calendar.secondaryDate(*date);
        calendarAnnotationKey_=annotationKey;
    }
    if((date_<first||date_>last)&&!calendarAnnotations_.contains(date_)&&source_.calendar.secondaryDate)
        calendarAnnotations_[date_]=source_.calendar.secondaryDate(date_);
    if((date_<first||date_>last)&&!calendarDetails_.contains(date_)&&source_.calendar.secondaryAnnotations)
        for(auto& annotation:source_.calendar.secondaryAnnotations(date_,date_))
            if(annotation.calendarAvailable&&!annotation.fullDate.empty())
                calendarDetails_.emplace(annotation.date,std::move(annotation));
    if(calendarDisplaySecondary_&&!calendarDetails_.contains(monthStart))
    {
        calendarDisplaySecondary_=false;month_=date_.substr(0,7)+"-01";
        Calendar();return;
    }
    std::optional<std::string> nextSecondaryMonth;
    if(calendarDisplaySecondary_)
        for(const auto& date:dates)
            if(date&&*date>monthStart)
                if(const auto day=calendarDetails_.find(*date);day!=calendarDetails_.end()&&day->second.day==1)
                {nextSecondaryMonth=*date;break;}
    const bool hasToggle=calendarDetails_.contains(monthStart);
    const auto monthText=calendarDisplaySecondary_&&!calendarDetails_.at(monthStart).monthHeading.empty()?Wide(calendarDetails_.at(monthStart).monthHeading):
        std::to_wstring(month->year)+L" / "+std::to_wstring(month->month);
    auto& monthLabel=Add("calendar.month",ui::Role::Text,Rect(monthLeft,monthTop,toolbar>36?monthWidth:monthWidth-(hasToggle?224.f:150.f),32),monthText);monthLabel.bold=true;monthLabel.fontSize=16;monthLabel.tooltip=monthText;
    if(hasToggle)
    {
        const auto target=calendarDisplaySecondary_?_LW("statusBar.calendarGregorianShort"):
            CalendarButtonLabel(source_.calendar.secondaryCalendarId?source_.calendar.secondaryCalendarId():std::string{},language);
        auto& toggle=Add("calendar.toggleCalendar",ui::Role::Button,Rect(monthLeft+monthWidth-220,buttonsTop,72,32),target);
        toggle.centered=true;toggle.fontSize=12;
        toggle.tooltip=_LW(calendarDisplaySecondary_?"statusBar.calendarShowGregorian":"statusBar.calendarShowSecondary");
        toggle.accessibilityLabel=toggle.tooltip;
        Command(toggle.id,[this]{
            calendarDisplaySecondary_=!calendarDisplaySecondary_;
            month_=calendarDisplaySecondary_?date_:date_.substr(0,7)+"-01";
            scroll_=0;
        });
    }
    auto& todayButton=Add("calendar.today",ui::Role::Button,Rect(monthLeft+monthWidth-144,buttonsTop,68,32),_LW("app.widget.date_picker.today"));todayButton.centered=true;todayButton.fontSize=12;
    Command("calendar.today",[this,today]{date_=today;month_=calendarDisplaySecondary_?today:date_.substr(0,7)+"-01";if(!CalendarEditing())scroll_=0;calendarNotice_.clear();});
    for(int i=0;i<2;++i)
    {
        const auto id=i?"calendar.next":"calendar.previous";
        auto& n=Add(id,ui::Role::Icon,Rect(monthLeft+monthWidth-70+i*36.f,buttonsTop,32,32),L"",i?L"\uE76C":L"\uE76B");
        n.tooltip=_LW(i?"app.widget.date_picker.next":"app.widget.date_picker.previous");
        n.enabled=calendarDisplaySecondary_?
            (i?nextSecondaryMonth.has_value():calendar::CalendarService::AddDays(monthStart,-1).has_value()):
            (i?month->year<9999||month->month<12:month->year>1||month->month>1);
        Command(id,[this,i,monthStart,nextSecondaryMonth]{
            if(calendarDisplaySecondary_)
            {
                if(i){if(nextSecondaryMonth)month_=*nextSecondaryMonth;}
                else if(const auto anchor=calendar::CalendarService::AddDays(monthStart,-1))month_=*anchor;
            }
            else if(const auto d=calendar::CalendarService::GetDateInfo(month_))
            {
                int m=d->month+(i?1:-1),year=d->year;
                if(m<1){m=12;--year;}if(m>12){m=1;++year;}
                if(year>=1&&year<=9999)month_=Date(year,m,1);
            }
            if(!CalendarEditing())scroll_=0;calendarNotice_.clear();
        });
    }
    const float cell=monthWidth/7;
    for(int i=0;i<7;++i)
    {
        const auto key="app.widget.date_picker.weekday"+std::to_string((i+1)%7+1);
        auto& n=Add("calendar.column:"+std::to_string(i),ui::Role::Text,Rect(monthLeft+i*cell,gridTop,cell-4,22),_LW(key.c_str()));
        n.centered=n.secondary=true;n.fontSize=12;
    }
    for(int i=0;i<42;++i)
    {
        const auto& date=dates[i];if(!date)continue;
        const auto d=calendar::CalendarService::GetDateInfo(*date);if(!d)continue;
        const auto displayed=calendarDisplaySecondary_&&calendarDetails_.contains(*date)?calendarDetails_.at(*date).day:d->day;
        auto& n=Add("date:"+*date,ui::Role::ListItem,Rect(monthLeft+(i%7)*cell,gridTop+28+(i/7)*34.f,cell-4,30),std::to_wstring(displayed));
        n.centered=true;n.outlined=*date==date_;n.selected=n.accent=*date==today;n.fontSize=13;
        if(calendarDisplaySecondary_)
        {
            const auto current=calendarDetails_.find(monthStart),day=calendarDetails_.find(*date);
            n.secondary=day==calendarDetails_.end()||current==calendarDetails_.end()||
                day->second.year!=current->second.year||day->second.era!=current->second.era||
                day->second.month!=current->second.month||day->second.leapMonth!=current->second.leapMonth;
        }
        else n.secondary=d->month!=month->month;
        n.marked=eventsByDate.contains(*date)&&!eventsByDate.at(*date).empty();
        SYSTEMTIME time{};time.wYear=static_cast<WORD>(d->year);time.wMonth=static_cast<WORD>(d->month);time.wDay=static_cast<WORD>(d->day);wchar_t weekday[96]{};
        GetDateFormatEx(locale.c_str(),0,&time,L"dddd",weekday,96,nullptr);
        n.tooltip=Wide(*date);if(weekday[0])n.tooltip+=L" · "+std::wstring(weekday);
        if(const auto secondary=calendarAnnotations_.find(*date);secondary!=calendarAnnotations_.end()&&!secondary->second.empty())n.tooltip+=L"\n"+Wide(secondary->second);
        n.accessibilityLabel=n.tooltip;
        Command(n.id,[this,date=*date]{date_=date;month_=calendarDisplaySecondary_?date:date.substr(0,7)+"-01";calendarNotice_.clear();});
    }
    const float monthEnd=gridTop+28+6*34,agendaLeft=two?monthLeft+monthWidth+16:16,agendaWidth=scene_.width-agendaLeft-16;
    const float agendaTop=two?monthTop:monthEnd+16;
    const bool fixedMonth=two&&available_>=monthEnd+4.f;
    const float panelHeight=monthEnd+8.f;
    if(CalendarEditing())
    {
        // Secondary content owns the agenda column or lower area. Keep the
        // original month nodes and actions while the form/picker builds in its
        // own local coordinates; live EDIT descriptors use the final geometry.
        auto monthScene=std::move(scene_);scene_={};
        const float end=CalendarEditor(agendaWidth+32),offsetX=agendaLeft-16,offsetY=agendaTop-10;
        auto editorNodes=std::move(scene_.nodes);scene_=std::move(monthScene);
        for(auto& node:editorNodes)
        {
            node.bounds.left+=offsetX;node.bounds.right+=offsetX;node.bounds.top+=offsetY;node.bounds.bottom+=offsetY;
            // Finish supplies the column viewport rather than keeping a
            // picker's old local per-node clip after translation.
            node.clip={};
            for(auto& path:node.paths)for(auto& point:path){point.x+=offsetX;point.y+=offsetY;}
            scene_.nodes.push_back(std::move(node));
        }
        bodyLeft_=fixedMonth?agendaLeft:0;bodyStart_=fixedMonth?agendaTop:0;
        if(calendarStacked_)FinishStackedCalendar(monthEnd,agendaTop,agendaTop,end+offsetY,false);
        else if(fixedMonth)Finish(end+offsetY,false,panelHeight,panelHeight);
        else Finish((std::max)(monthEnd,end+offsetY),false);
        return;
    }
    float y=agendaTop;
    const auto selectedText=calendarDisplaySecondary_&&calendarDetails_.contains(date_)?
        Wide(calendarDetails_.at(date_).fullDate):Wide(date_);
    auto& selectedHeading=Add("calendar.selected",ui::Role::Text,Rect(agendaLeft,y,agendaWidth-80,calendarDisplaySecondary_?48.f:32.f),selectedText);
    selectedHeading.bold=true;selectedHeading.wrap=calendarDisplaySecondary_;selectedHeading.tooltip=selectedText;
    Add("calendar.add",ui::Role::Icon,Rect(scene_.width-84,y,32,32),L"",L"\uE710").tooltip=_LW("settings.calendar.add");
    Command("calendar.add",[this]{calendar::CalendarEvent event;event.date=date_;event.startMinutes=9*60;event.endMinutes=10*60;EditCalendar(std::move(event));});
    Add("calendar.manage",ui::Role::Icon,Rect(scene_.width-48,y,32,32),L"",L"\uE713").tooltip=_LW("statusBar.manageCalendar");
    Command("calendar.manage",[this]{const auto manage=source_.calendar.manage;if(manage)manage();});y+=calendarDisplaySecondary_?56.f:40.f;
    if(calendarDisplaySecondary_)
    {
        auto& n=Add("calendar.selectedSecondary",ui::Role::Text,Rect(agendaLeft,y-16,agendaWidth,36),Wide(date_));
        n.fontSize=12;n.secondary=true;y+=40;
    }
    else if(const auto selected=calendarAnnotations_.find(date_);selected!=calendarAnnotations_.end())
    {
        const auto text=Wide(selected->second);
        if(!text.empty()){auto& n=Add("calendar.selectedSecondary",ui::Role::Text,Rect(agendaLeft,y-16,agendaWidth,36),text);n.fontSize=12;n.secondary=n.wrap=true;y+=40;}
    }
    if(!calendarNotice_.empty())
    {
        auto& notice=Add("calendar.notice",ui::Role::Text,Rect(agendaLeft,y,agendaWidth,52),_LW(calendarNotice_.c_str()));
        notice.wrap=true;notice.fontSize=13;notice.tooltip=notice.text;y+=60;
    }
    const float agendaStart=y;
    std::array<std::pair<std::string,std::vector<calendar::CalendarEvent>>,3> agenda;
    bool hasEvents=false;
    for(int i=0;i<3;++i)if(const auto date=calendar::CalendarService::AddDays(date_,i))
    {
        agenda[i].first=*date;
        if(const auto found=eventsByDate.find(*date);found!=eventsByDate.end())agenda[i].second=found->second;
        hasEvents=hasEvents||!agenda[i].second.empty();
    }
    // The selected date is first; the following two days provide nearby agenda
    // without changing the selection or manufacturing events for empty dates.
    for(int offsetDay=0;offsetDay<3;++offsetDay)
    {
        const auto& [agendaDate,events]=agenda[offsetDay];if(agendaDate.empty())continue;
        if(offsetDay&&events.empty())continue;
        if(offsetDay){auto& n=Add("calendar.agenda:"+agendaDate,ui::Role::Text,Rect(agendaLeft,y,agendaWidth,28),Wide(agendaDate));n.fontSize=12;n.secondary=true;y+=32;}
        if(events.empty()&&hasEvents){Add("calendar.empty",ui::Role::Text,Rect(agendaLeft,y,agendaWidth,40),_LW("settings.calendar.empty")).fontSize=12;y+=48;}
        for(const auto& e:events)
        {
            wchar_t when[32]{};swprintf_s(when,L"%02d:%02d",e.startMinutes/60,e.startMinutes%60);
            auto& n=Add("event:"+agendaDate+":"+e.id,ui::Role::ListItem,Rect(agendaLeft,y,agendaWidth,56),Wide(e.title));
            n.detail=e.allDay?_LW("settings.calendar.allDay"):when;n.tooltip=Wide(e.title)+L"\n"+Wide(e.date)+L" · "+n.detail;
            if(!e.notes.empty())n.tooltip+=L"\n"+Wide(e.notes);
            n.accessibilityLabel=Wide(e.title)+L" · "+Wide(e.date)+L" · "+n.detail;
            calendarEvents_[n.id]=e;
            Command(n.id,[this,id=n.id]{CalendarEventCommand(id,false);});y+=64;
        }
    }
    if(!hasEvents){auto& empty=Add("calendar.empty",ui::Role::Text,Rect(agendaLeft,y,agendaWidth,48),_LW("settings.calendar.empty"));empty.fontSize=12;empty.centered=empty.wrap=true;y+=two?48.f:104.f;}
    // Keep month navigation and the day grid stationary when the
    // full month fits. Short/narrow viewports retain one accessible scroll flow.
    bodyLeft_=fixedMonth?agendaLeft:0;bodyStart_=fixedMonth?agendaStart:0;
    if(calendarStacked_)FinishStackedCalendar(monthEnd,agendaTop,agendaStart,y,!hasEvents);
    else if(fixedMonth)Finish(y,false,panelHeight,panelHeight);
    else Finish((std::max)(monthEnd,y),false);
    if(calendarStacked_)return;
    if(!hasEvents)
        for(auto& node:scene_.nodes)if(node.id=="calendar.empty")
        {
            // Text paragraphs are vertically centered by the shared renderer.
            // Fill the visible agenda body rather than pinning empty state to
            // the selected-date heading or centering it over the month.
            const float top=agendaStart-scroll_;
            node.bounds.top=top;node.bounds.bottom=(std::max)(top+48,scene_.cards.front().bottom-8);
            break;
        }
}
void SystemPanelModel::FinishStackedCalendar(float monthEnd,float agendaTop,float agendaStart,float agendaEnd,bool empty)
{
    const float monthHeight=monthEnd+8.f,gap=8.f;
    const float room=available_-monthHeight-gap;
    const bool separate=room>=240.f;
    const float agendaHeight=separate?(std::min)(400.f,room):
        (std::max)(220.f,agendaEnd-agendaTop+24.f);
    const float monthShift=agendaHeight+gap,agendaShift=16.f-agendaTop;
    const float scrollTop=agendaStart+agendaShift;
    const auto shift=[](ui::Node& node,float amount) {
        node.bounds.top+=amount;node.bounds.bottom+=amount;
        for(auto& path:node.paths)for(auto& point:path)point.y+=amount;
    };
    if(separate)
    {
        maxScroll_=(std::max)(0.f,agendaEnd+agendaShift+8.f-agendaHeight);
        scroll_=std::clamp(scroll_,0.f,maxScroll_);
        scrollViewport_=Rect(0,scrollTop,scene_.width,agendaHeight-scrollTop-8.f);
        bodyLeft_=0;bodyStart_=scrollTop;
    }
    else
    {
        bodyLeft_=bodyStart_=0;
    }
    for(auto& node:scene_.nodes)
    {
        const bool monthNode=node.bounds.top<agendaTop;
        const bool scrollNode=!monthNode&&node.bounds.top>=agendaStart;
        shift(node,monthNode?monthShift:agendaShift);
        if(separate&&scrollNode){node.clip=scrollViewport_;shift(node,-scroll_);}
    }
    if(empty)
        for(auto& node:scene_.nodes)if(node.id=="calendar.empty")
        {
            node.bounds.top=separate?scrollTop:agendaStart+agendaShift;
            node.bounds.bottom=agendaHeight-8.f;
            break;
        }
    if(!separate)
    {
        Finish(monthEnd+monthShift,false);
        return;
    }
    scene_.cards.push_back(Rect(0,0,scene_.width,agendaHeight));
    scene_.cards.push_back(Rect(0,monthShift,scene_.width,monthHeight));
    scene_.height=monthShift+monthHeight;
    if(maxScroll_>0)
    {
        const auto axis=ScrollbarGeometry();
        Add("scrollbar",ui::Role::Scrollbar,Rect(scene_.width-4,static_cast<float>(axis.thumbStart),2,static_cast<float>(axis.ThumbExtent())));
    }
}
void SystemPanelModel::EditCalendar(calendar::CalendarEvent event)
{
    if(closed_||!source_.calendar.mutations.save)return;
    calendarReturnScroll_=scroll_;scroll_=0;++navigation_;page_="calendar-edit";calendarNotice_.clear();
    calendarConfirmDelete_=calendarReminderOpen_=false;calendarDeleteOrigin_=CalendarDeleteOrigin::Editor;
    calendarPicker_.reset();calendarPickerDates_.clear();calendarPickerField_.clear();calendarFocus_="calendar.edit.title";
    calendarEditor_=std::make_shared<SystemCalendarEditorState>();
    calendarEditor_->original=event;calendarEditor_->draft=std::move(event);
    calendarEditor_->actions=source_.calendar.mutations;
    calendarSeriesOriginal_.reset();calendarScopeSeries_=calendarDiscardConfirmed_=false;
    calendarMode_="single";
    if(!calendarEditor_->original.seriesId.empty()&&source_.calendar.mutations.seriesById)
        calendarSeriesOriginal_=source_.calendar.mutations.seriesById(calendarEditor_->original.seriesId);
    if(calendarSeriesOriginal_)calendarRule_=calendarSeriesOriginal_->rule;
    else
    {
        const auto& date=calendarEditor_->draft.date;
        const auto info=calendar::CalendarService::GetDateInfo(date);
        calendarRule_={};calendarRule_.kind="dates";calendarRule_.dates={date};
        calendarRule_.startDate=date;calendarRule_.interval=1;
        calendarRule_.weekdays={info?info->weekday:1};calendarRule_.monthDay=info?info->day:1;
    }
    calendarEditor_->valid=[this,weak=std::weak_ptr(calendarEditor_)]{
        return !closed_&&page_=="calendar-edit"&&weak.lock()==calendarEditor_;
    };
    const auto time=[](int minutes){wchar_t value[16]{};swprintf_s(value,L"%02d:%02d",minutes/60,minutes%60);return std::wstring(value);};
    const auto& draft=calendarEditor_->draft;
    calendarText_={{"calendar.edit.title",Wide(draft.title)},{"calendar.edit.date",Wide(draft.date)},
        {"calendar.edit.start",time(draft.startMinutes)},{"calendar.edit.end",time(draft.endMinutes)},
        {"calendar.edit.notes",Wide(draft.notes)}};
}
void SystemPanelModel::SwitchCalendarScope(bool wholeSeries)
{
    if(!CalendarEditing()||!calendarSeriesOriginal_)return;
    calendarScopeSeries_=wholeSeries;calendarDiscardConfirmed_=false;
    calendarMode_=wholeSeries?calendarRule_.kind:"single";
    calendarEditor_->draft=wholeSeries?calendarSeriesOriginal_->event:calendarEditor_->original;
    const auto& draft=calendarEditor_->draft;
    const auto time=[](int minutes){wchar_t value[16]{};swprintf_s(value,L"%02d:%02d",minutes/60,minutes%60);return std::wstring(value);};
    calendarText_["calendar.edit.title"]=Wide(draft.title);
    calendarText_["calendar.edit.date"]=Wide(draft.date);
    calendarText_["calendar.edit.start"]=time(draft.startMinutes);
    calendarText_["calendar.edit.end"]=time(draft.endMinutes);
    calendarText_["calendar.edit.notes"]=Wide(draft.notes);
    calendarEditor_->error.clear();
}
bool SystemPanelModel::CalendarEditing() const
{return !closed_&&action_==StatusBarAction::Calendar&&page_=="calendar-edit"&&calendarEditor_;}
std::string SystemPanelModel::CalendarFocusTarget() const
{
    if(closed_||action_!=StatusBarAction::Calendar)return {};
    if(!CalendarEditing())return calendarFocus_.empty()?std::string{}:
        scene_.Find(calendarFocus_)?calendarFocus_:std::string("calendar.add");
    if(calendarPicker_)return calendarPicker_->FocusTarget();
    return calendarConfirmDelete_?"calendar.edit.cancelDelete":calendarFocus_;
}
std::vector<SystemCalendarInputField> SystemPanelModel::CalendarInputFields() const
{
    std::vector<SystemCalendarInputField> fields;if(!CalendarEditing()||calendarPicker_||
        (calendarConfirmDelete_&&calendarDeleteOrigin_==CalendarDeleteOrigin::ContextMenu))return fields;
    for(const auto* suffix:{"title","notes"})
    {
        const std::string id="calendar.edit."+std::string(suffix);
        const auto* node=scene_.Find(id);const auto value=calendarText_.find(id);
        if(!node||value==calendarText_.end())continue;
        const bool notes=std::string_view(suffix)=="notes";
        fields.push_back({id,node->accessibilityLabel,value->second,node->bounds,node->clip,notes,node->enabled,
            notes?8192:512});
    }
    return fields;
}
bool SystemPanelModel::SetCalendarInput(std::string_view id,std::wstring text)
{
    if(!CalendarEditing()||calendarConfirmDelete_||calendarPicker_||
        (id!="calendar.edit.title"&&id!="calendar.edit.notes"))return false;
    const auto found=calendarText_.find(std::string(id));if(found==calendarText_.end())return false;
    // Preserve native text and IME text verbatim. Validation belongs to
    // Save, and typing must not rebuild or reset the live native EDIT selection.
    found->second=std::move(text);return true;
}
void SystemPanelModel::LeaveCalendarEditor(bool followSavedDate)
{
    if(!CalendarEditing())return;
    if(followSavedDate&&calendar::CalendarService::GetDateInfo(calendarEditor_->draft.date))
    {date_=calendarEditor_->draft.date;month_=date_.substr(0,7)+"-01";scroll_=0;}
    else scroll_=calendarReturnScroll_;
    const auto& returning=followSavedDate?calendarEditor_->draft:calendarEditor_->original;
    calendarFocus_=returning.id.empty()?"calendar.add":"event:"+returning.date+":"+returning.id;
    ++navigation_;page_.clear();calendarEditor_.reset();calendarText_.clear();
    calendarSeriesOriginal_.reset();calendarRule_={};calendarMode_="single";
    calendarScopeSeries_=calendarDiscardConfirmed_=false;
    calendarConfirmDelete_=calendarReminderOpen_=false;calendarDeleteOrigin_=CalendarDeleteOrigin::Editor;
    calendarPicker_.reset();calendarPickerDates_.clear();calendarPickerField_.clear();
}
bool SystemPanelModel::CalendarBack()
{
    if(!CalendarEditing())return false;
    if(calendarPicker_){calendarPicker_.reset();calendarPickerDates_.clear();calendarFocus_=calendarPickerField_;calendarPickerField_.clear();scroll_=calendarEditorScroll_;}
    else if(calendarConfirmDelete_)
    {
        if(calendarDeleteOrigin_==CalendarDeleteOrigin::ContextMenu)LeaveCalendarEditor(false);
        else{calendarConfirmDelete_=false;calendarEditor_->error.clear();}
    }
    else if(calendarReminderOpen_)calendarReminderOpen_=false;
    else LeaveCalendarEditor(false);
    Refresh(available_);return true;
}
bool SystemPanelModel::CalendarEventIsSeries(std::string_view nodeId) const
{
    const auto found=calendarEvents_.find(std::string(nodeId));
    return found!=calendarEvents_.end()&&!found->second.seriesId.empty();
}
bool SystemPanelModel::CalendarEventCommand(std::string_view nodeId,bool remove,bool wholeSeries)
{
    if(closed_||action_!=StatusBarAction::Calendar||CalendarEditing())return false;
    const auto found=calendarEvents_.find(std::string(nodeId));
    if(found==calendarEvents_.end()||!scene_.Find(nodeId))
    {
        if(nodeId.starts_with("event:")){calendarNotice_="settings.calendar.conflict";Refresh(available_);}
        return false;
    }
    const auto event=found->second;
    const auto generation=navigation_;
    const auto current=source_.calendar.mutations.current;
    if(!current){calendarNotice_="settings.calendar.failed";Refresh(available_);return false;}
    const auto present=current(event);
    // A context menu may have stayed open while this event was removed or
    // rescheduled. Never substitute a different row or silently adopt its edit.
    if(closed_||generation!=navigation_||CalendarEditing())return false;
    if(!present||present->id!=event.id||present->revision!=event.revision||present->date!=event.date)
    {calendarNotice_="settings.calendar.conflict";Refresh(available_);return false;}
    EditCalendar(event);if(!CalendarEditing())return false;
    if(wholeSeries)
    {
        if(!calendarSeriesOriginal_){LeaveCalendarEditor(false);return false;}
        SwitchCalendarScope(true);
    }
    calendarConfirmDelete_=remove;calendarDeleteOrigin_=remove?CalendarDeleteOrigin::ContextMenu:CalendarDeleteOrigin::Editor;
    Refresh(available_);return true;
}
void SystemPanelModel::SaveCalendar()
{
    if(!CalendarEditing()||calendarConfirmDelete_||calendarPicker_)return;
    const auto editor=calendarEditor_;
    const auto utf8=[](const std::wstring& value)->std::optional<std::string>{
        if(value.empty())return std::string{};
        if(value.size()>static_cast<std::size_t>((std::numeric_limits<int>::max)()))return {};
        const int length=static_cast<int>(value.size());
        const int count=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),length,nullptr,0,nullptr,nullptr);
        if(count<=0)return {};
        std::string result(static_cast<std::size_t>(count),'\0');
        if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),length,result.data(),count,nullptr,nullptr)!=count)return {};
        return result;
    };
    auto candidate=editor->draft;
    const auto title=utf8(calendarText_.at("calendar.edit.title")),date=utf8(calendarText_.at("calendar.edit.date")),notes=utf8(calendarText_.at("calendar.edit.notes"));
    const auto start=ParseCalendarEditorTime(calendarText_.at("calendar.edit.start")),end=ParseCalendarEditorTime(calendarText_.at("calendar.edit.end"));
    if(!title||!date||!notes||!calendar::CalendarService::GetDateInfo(*date)||
        (!candidate.allDay&&(!start||!end||*end<*start)))
    {editor->error="invalid_time";return;}
    candidate.title=*title;candidate.date=*date;candidate.notes=*notes;
    candidate.startMinutes=candidate.allDay?0:*start;candidate.endMinutes=candidate.allDay?1439:*end;
    bool saved=false;
    if(calendarMode_=="single")saved=editor->Save(std::move(candidate));
    else
    {
        auto series=calendarSeriesOriginal_.value_or(calendar::CalendarSeries{});
        series.rule=calendarRule_;series.rule.kind=calendarMode_;
        if(calendarMode_=="dates")
        {
            auto& dates=series.rule.dates;
            std::sort(dates.begin(),dates.end());dates.erase(std::unique(dates.begin(),dates.end()),dates.end());
            if(dates.empty()||dates.size()>366){editor->error="invalid_date";return;}
            series.rule.startDate=dates.front();series.rule.endDate=dates.back();
            series.rule.interval=1;series.rule.weekdays.clear();series.rule.monthDay=0;
        }
        else
        {
            series.rule.startDate=*date;series.rule.dates.clear();
            if(!series.rule.endDate.empty()&&
                (!calendar::CalendarService::GetDateInfo(series.rule.endDate)||series.rule.endDate<*date))
            {editor->error="invalid_date";return;}
            if(calendarMode_=="weekly")
            {series.rule.monthDay=0;if(series.rule.weekdays.empty()){editor->error="invalid_rule";return;}}
            else series.rule.weekdays.clear();
        }
        candidate.date=series.rule.startDate;
        series.event=candidate;
        if(calendarSeriesOriginal_)
        {
            bool discards=false;
            for(const auto& [originalDate,exception]:calendarSeriesOriginal_->exceptions)
                if(!calendar::CalendarService::MatchesRule(series.rule,originalDate))
                {discards=true;break;}
            if(discards&&!calendarDiscardConfirmed_)
            {calendarDiscardConfirmed_=true;editor->error="discard_exceptions";return;}
        }
        if(!source_.calendar.mutations.saveSeries){editor->error="unavailable";return;}
        editor->error.clear();const auto result=source_.calendar.mutations.saveSeries(std::move(series));
        if(result.ok){editor->draft=candidate;editor->draft.id=result.id;editor->draft.revision=result.revision;saved=true;}
        else editor->error=result.error;
    }
    if(closed_||calendarEditor_!=editor)return;
    if(saved)LeaveCalendarEditor(true);
}
void SystemPanelModel::RemoveCalendar()
{
    if(!CalendarEditing()||!calendarConfirmDelete_)return;
    const auto editor=calendarEditor_;
    bool removed=false;
    if(calendarScopeSeries_&&calendarSeriesOriginal_)
    {
        if(!source_.calendar.mutations.removeSeries)editor->error="unavailable";
        else
        {
            const auto result=source_.calendar.mutations.removeSeries(
                calendarSeriesOriginal_->id,calendarSeriesOriginal_->revision);
            removed=result.ok;if(!removed)editor->error=result.error;
        }
    }
    else removed=editor->Remove();
    if(closed_||calendarEditor_!=editor)return;
    if(removed)LeaveCalendarEditor(false);
}
float SystemPanelModel::CalendarEditor(float viewportWidth)
{
    const auto editor=calendarEditor_;if(!editor)return 0;
    if(calendarPicker_)return CalendarPicker(viewportWidth);
    scene_.width=viewportWidth;
    if(calendarConfirmDelete_&&calendarDeleteOrigin_==CalendarDeleteOrigin::ContextMenu)
    {
        const float width=scene_.width-32;
        auto& heading=Add("calendar.delete.heading",ui::Role::Text,Rect(16,10,width,36),_LW("app.settings.delete"));heading.bold=true;heading.fontSize=17;
        auto& title=Add("calendar.delete.target",ui::Role::Text,Rect(16,64,width,48),Wide(editor->original.title));title.wrap=true;title.bold=true;
        Add("calendar.delete.date",ui::Role::Text,Rect(16,116,width,28),Wide(calendarScopeSeries_&&calendarSeriesOriginal_?
            calendarSeriesOriginal_->rule.startDate:editor->original.date)).secondary=true;
        auto& question=Add("calendar.edit.confirmation",ui::Role::Text,Rect(16,152,width,52),_LW(calendarScopeSeries_?
            "settings.calendar.deleteSeries":"settings.calendar.confirmDelete"));question.wrap=true;
        float y=212;
        if(!editor->error.empty())
        {
            const auto key=editor->error=="conflict"||editor->error=="not_found"?"settings.calendar.conflict":"settings.calendar.failed";
            auto& message=Add("calendar.edit.error",ui::Role::Text,Rect(16,y,width,48),_LW(key));message.wrap=true;y+=56;
        }
        const float button=(width-12)/2;
        Add("calendar.edit.cancelDelete",ui::Role::Button,Rect(16,y,button,36),_LW("settings.dialog.cancel")).centered=true;
        Command("calendar.edit.cancelDelete",[this]{CalendarBack();});
        auto& confirm=Add("calendar.edit.confirmDelete",ui::Role::Button,Rect(28+button,y,button,36),_LW("app.settings.delete"));confirm.centered=confirm.accent=true;
        Command("calendar.edit.confirmDelete",[this]{RemoveCalendar();});return y+36;
    }
    Add("calendar.edit.back",ui::Role::Icon,Rect(16,10,36,36),L"",L"\uE76B").tooltip=_LW("settings.shell.back");
    Command("calendar.edit.back",[this]{CalendarBack();});
    auto& heading=Add("calendar.edit.heading",ui::Role::Text,Rect(56,10,scene_.width-72,36),
        _LW(editor->original.id.empty()?"settings.calendar.add":"settings.calendar.edit"));heading.bold=true;heading.fontSize=17;
    const bool wide=scene_.width>=560;const float width=scene_.width-32;
    const float leftWidth=wide?(width-16)*.56f:width,right=wide?32+leftWidth:16,rightWidth=wide?width-leftWidth-16:width;
    const auto field=[&](const char* suffix,float x,float y,float w,float h,bool enabled=true){
        const std::string id="calendar.edit."+std::string(suffix),key=std::string_view(suffix)=="date"&&
            (calendarMode_=="weekly"||calendarMode_=="monthly")?
            "settings.calendar.startDate":"settings.calendar."+std::string(suffix);
        const auto label=_LW(key.c_str());
        auto& caption=Add(id+".label",ui::Role::Text,Rect(x,y,w,20),label);caption.fontSize=13;caption.secondary=true;
        const bool picker=std::string_view(suffix)=="date"||std::string_view(suffix)=="start"||std::string_view(suffix)=="end";
        std::wstring value=picker?calendarText_.at(id):std::wstring{};
        if(std::string_view(suffix)=="date"&&calendarMode_=="dates")
        {
            const auto& dates=calendarRule_.dates;
            value=dates.empty()?std::wstring(_LW("settings.calendar.chooseDates")):Wide(dates.front());
            if(dates.size()>1)value+=L" · "+Wide(dates[1]);
            if(dates.size()>2)value+=L" +"+std::to_wstring(dates.size()-2);
        }
        auto& node=Add(id,ui::Role::Button,Rect(x,y+24,w,h),std::move(value));node.accessibilityLabel=node.tooltip=label;
        node.enabled=enabled&&!calendarConfirmDelete_;
        if(picker){node.tooltip=node.accessibilityLabel=std::wstring(label)+L" · "+node.text;Command(id,[this,id]{OpenCalendarPicker(id);});}
    };
    const auto option=[&](const std::string& id,const std::wstring& label,float x,float y,float w,bool selected=false){
        auto& node=Add(id,ui::Role::Button,Rect(x,y,w,36),label);node.centered=true;node.selected=node.accent=selected;
        node.enabled=!calendarConfirmDelete_;node.accessibilityLabel=node.tooltip=label;
    };
    field("title",16,64,leftWidth,36);
    const float dateY=wide?64.f:136.f;
    field("date",right,dateY,rightWidth,36);
    float allDayY=dateY+72;
    if(editor->original.id.empty()||calendarScopeSeries_)
    {
        auto& caption=Add("calendar.edit.mode.label",ui::Role::Text,Rect(right,allDayY,rightWidth,20),_LW("settings.calendar.dateMode"));
        caption.fontSize=13;caption.secondary=true;
        const float modeY=allDayY+24;
        const std::vector<std::string> modes=calendarScopeSeries_?
            std::vector<std::string>{"dates","weekly","monthly"}:
            std::vector<std::string>{"single","dates","weekly","monthly"};
        const std::size_t columns=2;
        const float gap=6,buttonWidth=(rightWidth-gap)/columns;
        for(std::size_t index=0;index<modes.size();++index)
        {
            const auto mode=modes[index],id="calendar.edit.mode."+mode,key="settings.calendar.mode."+mode;
            option(id,_LW(key.c_str()),right+(index%columns)*(buttonWidth+gap),
                modeY+(index/columns)*48,buttonWidth,calendarMode_==mode);
            Command(id,[this,mode]{
                const auto previous=calendarMode_;
                const auto start=calendar::CalendarService::GetDateInfo(calendarRule_.startDate);
                if(mode=="dates"&&calendarRule_.dates.empty()&&start)
                    calendarRule_.dates.push_back(calendarRule_.startDate);
                if(mode=="weekly"&&calendarRule_.weekdays.empty()&&start)
                    calendarRule_.weekdays.push_back(start->weekday);
                if(mode=="monthly"&&previous!="monthly"&&calendarRule_.monthDay==0&&start)
                    calendarRule_.monthDay=start->day;
                calendarMode_=mode;calendarRule_.kind=mode;
                calendarDiscardConfirmed_=false;calendarEditor_->error.clear();});
        }
        allDayY=modeY+((modes.size()+columns-1)/columns)*48;
    }
    if(calendarMode_=="weekly"||calendarMode_=="monthly")
    {
        float ruleBottom=allDayY;
        const auto intervalLabel=std::wstring(_LW("settings.calendar.interval"))+L" · "+std::to_wstring(calendarRule_.interval);
        Add("calendar.edit.interval.label",ui::Role::Text,Rect(right,ruleBottom,rightWidth,24),intervalLabel).secondary=true;ruleBottom+=28;
        option("calendar.edit.interval.minus",L"−",right,ruleBottom,44);
        option("calendar.edit.interval.plus",L"+",right+rightWidth-44,ruleBottom,44);
        Command("calendar.edit.interval.minus",[this]{calendarRule_.interval=(std::max)(1,calendarRule_.interval-1);calendarDiscardConfirmed_=false;});
        Command("calendar.edit.interval.plus",[this]{calendarRule_.interval=(std::min)(99,calendarRule_.interval+1);calendarDiscardConfirmed_=false;});
        ruleBottom+=44;
        if(calendarMode_=="weekly")
        {
            const float gap=4,dayWidth=(rightWidth-gap*6)/7;
            for(int day=1;day<=7;++day)
            {
                const auto id="calendar.edit.weekday."+std::to_string(day),key="settings.calendar.weekday."+std::to_string(day);
                const auto selected=std::find(calendarRule_.weekdays.begin(),calendarRule_.weekdays.end(),day)!=calendarRule_.weekdays.end();
                option(id,_LW(key.c_str()),right+(day-1)*(dayWidth+gap),ruleBottom,dayWidth,selected);
                Command(id,[this,day]{auto& days=calendarRule_.weekdays;
                    const auto found=std::find(days.begin(),days.end(),day);
                    if(found!=days.end())days.erase(found);else{days.push_back(day);std::sort(days.begin(),days.end());}
                    calendarDiscardConfirmed_=false;});
            }
            ruleBottom+=48;
        }
        else
        {
            const auto label=calendarRule_.monthDay==0?std::wstring(_LW("settings.calendar.lastDay")):
                std::wstring(_LW("settings.calendar.monthDay"))+L" · "+std::to_wstring(calendarRule_.monthDay);
            Add("calendar.edit.monthDay.label",ui::Role::Text,Rect(right,ruleBottom,rightWidth,24),label).secondary=true;ruleBottom+=28;
            option("calendar.edit.monthDay.minus",L"−",right,ruleBottom,44);
            option("calendar.edit.monthDay.last",_LW("settings.calendar.lastDay"),right+52,ruleBottom,rightWidth-104,calendarRule_.monthDay==0);
            option("calendar.edit.monthDay.plus",L"+",right+rightWidth-44,ruleBottom,44);
            Command("calendar.edit.monthDay.minus",[this]{calendarRule_.monthDay=(std::max)(0,calendarRule_.monthDay-1);calendarDiscardConfirmed_=false;});
            Command("calendar.edit.monthDay.plus",[this]{calendarRule_.monthDay=(std::min)(31,calendarRule_.monthDay+1);calendarDiscardConfirmed_=false;});
            Command("calendar.edit.monthDay.last",[this]{calendarRule_.monthDay=0;calendarDiscardConfirmed_=false;});
            ruleBottom+=48;
        }
        const bool forever=calendarRule_.endDate.empty();
        option("calendar.edit.endType",_LW(forever?"settings.calendar.neverEnds":"settings.calendar.endsOn"),right,ruleBottom,rightWidth);
        Command("calendar.edit.endType",[this]{if(calendarRule_.endDate.empty())
                calendarRule_.endDate=calendar::CalendarService::AddDays(calendarRule_.startDate,30).value_or(date_);
            else calendarRule_.endDate.clear();calendarDiscardConfirmed_=false;});
        ruleBottom+=44;
        if(!forever)
        {
            option("calendar.edit.endDate",Wide(calendarRule_.endDate),right,ruleBottom,rightWidth);
            Command("calendar.edit.endDate",[this]{OpenCalendarPicker("calendar.edit.endDate");});ruleBottom+=44;
        }
        allDayY=ruleBottom+12;
    }
    auto& allDay=Add("calendar.edit.allDay",ui::Role::Toggle,Rect(right,allDayY,rightWidth,36),_LW("settings.calendar.allDay"));
    allDay.switchStyle=true;allDay.selected=editor->draft.allDay;allDay.enabled=!calendarConfirmDelete_;
    Command(allDay.id,[this]{calendarEditor_->draft.allDay=!calendarEditor_->draft.allDay;});
    const float timeY=allDayY+48,timeWidth=(rightWidth-12)/2;
    field("start",right,timeY,timeWidth,36,!editor->draft.allDay);
    field("end",right+timeWidth+12,timeY,timeWidth,36,!editor->draft.allDay);
    const float reminderY=timeY+72;
    auto& reminderLabel=Add("calendar.edit.reminder.label",ui::Role::Text,Rect(right,reminderY,rightWidth,20),_LW("settings.calendar.reminder"));reminderLabel.fontSize=13;reminderLabel.secondary=true;
    const auto reminderKey="settings.calendar.reminder."+std::to_string(editor->draft.reminderMinutes);
    auto& reminder=Add("calendar.edit.reminder",ui::Role::Button,Rect(right,reminderY+24,rightWidth,36),_LW(reminderKey.c_str()));
    reminder.enabled=!calendarConfirmDelete_;reminder.tooltip=std::wstring(_LW("settings.calendar.reminder"))+L" · "+reminder.text;
    reminder.accessibilityLabel=reminder.tooltip;
    Command(reminder.id,[this]{calendarReminderOpen_=!calendarReminderOpen_;});
    float rightEnd=reminderY+72;
    if(calendarReminderOpen_&&!calendarConfirmDelete_)
        for(const auto minutes:SystemCalendarReminderMinutes)
        {
            const auto id="calendar.edit.reminder:"+std::to_string(minutes),key="settings.calendar.reminder."+std::to_string(minutes);
            auto& choice=Add(id,ui::Role::ListItem,Rect(right,rightEnd,rightWidth,32),_LW(key.c_str()));choice.selected=minutes==editor->draft.reminderMinutes;choice.fontSize=13;
            Command(id,[this,minutes]{calendarEditor_->draft.reminderMinutes=minutes;calendarReminderOpen_=false;});rightEnd+=36;
        }
    const float notesY=wide?136.f:rightEnd;
    const float notesHeight=MeasureSystemCalendarNotesHeight(calendarText_.at("calendar.edit.notes"),leftWidth);
    field("notes",16,notesY,leftWidth,notesHeight);
    float footer=(std::max)(rightEnd,notesY+24+notesHeight)+16;
    if(calendarSeriesOriginal_)
    {
        auto& caption=Add("calendar.edit.scope.label",ui::Role::Text,Rect(16,footer,width,20),_LW("settings.calendar.scope"));
        caption.fontSize=13;caption.secondary=true;footer+=24;
        const float half=(width-8)/2;
        option("calendar.edit.scope.once",_LW("settings.calendar.scope.once"),16,footer,half,!calendarScopeSeries_);
        option("calendar.edit.scope.series",_LW("settings.calendar.scope.series"),24+half,footer,half,calendarScopeSeries_);
        Command("calendar.edit.scope.once",[this]{SwitchCalendarScope(false);});
        Command("calendar.edit.scope.series",[this]{SwitchCalendarScope(true);});
        footer+=48;
    }
    if(!editor->error.empty())
    {
        const auto& error=editor->error;
        const char* key=error=="discard_exceptions"?"settings.calendar.confirmExceptions":
            error=="conflict"||error=="not_found"?"settings.calendar.conflict":
            error=="title_required"||error=="text_too_long"||error=="invalid_date"||error=="invalid_time"||error=="invalid_reminder"?"settings.calendar.invalid":"settings.calendar.failed";
        auto& message=Add("calendar.edit.error",ui::Role::Text,Rect(16,footer,width,48),_LW(key));message.wrap=true;message.fontSize=13;footer+=56;
    }
    if(calendarConfirmDelete_)
    {
        auto& question=Add("calendar.edit.confirmation",ui::Role::Text,Rect(16,footer,width,36),_LW(calendarScopeSeries_?"settings.calendar.deleteSeries":"settings.calendar.confirmDelete"));question.wrap=true;footer+=44;
        const float buttonWidth=(width-12)/2;
        Add("calendar.edit.cancelDelete",ui::Role::Button,Rect(16,footer,buttonWidth,36),_LW("settings.dialog.cancel")).centered=true;
        Command("calendar.edit.cancelDelete",[this]{CalendarBack();});
        Add("calendar.edit.confirmDelete",ui::Role::Button,Rect(28+buttonWidth,footer,buttonWidth,36),_LW("app.settings.delete")).centered=true;
        Command("calendar.edit.confirmDelete",[this]{RemoveCalendar();});
    }
    else
    {
        const bool existing=!editor->original.id.empty(),separateSave=existing&&!wide;
        const float buttonWidth=(width-(existing&&!separateSave?24.f:12.f))/(existing&&!separateSave?3.f:2.f);
        float x=16;
        if(existing)
        {
            Add("calendar.edit.delete",ui::Role::Button,Rect(x,footer,buttonWidth,36),_LW("app.settings.delete")).centered=true;
            Command("calendar.edit.delete",[this]{calendarConfirmDelete_=true;calendarDeleteOrigin_=CalendarDeleteOrigin::Editor;calendarReminderOpen_=false;calendarEditor_->error.clear();});x+=buttonWidth+12;
        }
        Add("calendar.edit.cancel",ui::Role::Button,Rect(x,footer,buttonWidth,36),_LW("settings.dialog.cancel")).centered=true;
        Command("calendar.edit.cancel",[this]{LeaveCalendarEditor(false);});x+=buttonWidth+12;
        if(separateSave){x=16;footer+=44;}
        auto& save=Add("calendar.edit.save",ui::Role::Button,Rect(x,footer,separateSave?width:buttonWidth,36),_LW("settings.calendar.save"));save.centered=save.accent=true;
        Command(save.id,[this]{SaveCalendar();});
    }
    return footer+36;
}
void SystemPanelModel::OpenCalendarPicker(std::string field)
{
    if(!CalendarEditing()||calendarConfirmDelete_||calendarPicker_)return;
    if(field=="calendar.edit.date"||field=="calendar.edit.endDate")
    {
        const auto text=field=="calendar.edit.endDate"?Wide(calendarRule_.endDate):calendarText_.at(field);
        std::string date;date.reserve(text.size());
        // The form holds an ISO date. Reject unexpected non-ASCII input rather
        // than silently truncating UTF-16 when handing it to the date control.
        for(const auto value:text){if(value>L'\x7f')return;date.push_back(static_cast<char>(value));}
        calendarPicker_.emplace(std::move(date));
    }
    else if((field=="calendar.edit.start"||field=="calendar.edit.end")&&!calendarEditor_->draft.allDay)
    {
        const auto value=ParseCalendarEditorTime(calendarText_.at(field));if(!value)return;calendarPicker_.emplace(*value);
    }
    else return;
    calendarPickerDates_.clear();
    if(field=="calendar.edit.date"&&calendarMode_=="dates")
    {
        calendarPickerDates_=calendarRule_.dates;
        std::sort(calendarPickerDates_.begin(),calendarPickerDates_.end());
        calendarPickerDates_.erase(std::unique(calendarPickerDates_.begin(),calendarPickerDates_.end()),calendarPickerDates_.end());
    }
    calendarPickerField_=std::move(field);calendarEditorScroll_=scroll_;calendarReminderOpen_=false;scroll_=0;
}
float SystemPanelModel::CalendarPicker(float width)
{
    const auto key=calendarPickerField_=="calendar.edit.date"&&calendarMode_=="dates"?
        std::string("settings.calendar.chooseDates"):"settings.calendar."+calendarPickerField_.substr(14);
    const auto today=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    scene_=calendarPicker_->Build(width,_LW(key.c_str()),today);
    if(calendarPickerField_=="calendar.edit.date"&&calendarMode_=="dates")
    {
        const float actionsY=scene_.height-44;
        for(auto& node:scene_.nodes)
        {
            if(node.id.starts_with("picker.day:"))
            {
                const bool selected=std::binary_search(calendarPickerDates_.begin(),calendarPickerDates_.end(),node.id.substr(11));
                node.selected=node.accent=selected;
                if(!selected&&calendarPickerDates_.size()>=366)node.enabled=false;
            }
            else if(node.id=="picker.confirm")node.enabled=!calendarPickerDates_.empty();
            else if(node.id=="picker.today"&&calendarPickerDates_.size()>=366&&
                !std::binary_search(calendarPickerDates_.begin(),calendarPickerDates_.end(),today))node.enabled=false;
            if(node.id=="picker.cancel"||node.id=="picker.confirm")
            {node.bounds.top+=28;node.bounds.bottom+=28;node.clip=node.bounds;}
        }
        ui::Node summary;summary.id="picker.selection";summary.role=ui::Role::Text;
        summary.bounds=summary.clip=Rect(16,actionsY,width-32,24);
        summary.text=std::wstring(_LW("settings.calendar.selectedDates"))+L" · "+
            std::to_wstring(calendarPickerDates_.size());summary.fontSize=13;summary.secondary=true;
        scene_.nodes.push_back(std::move(summary));scene_.height+=28;
    }
    for(const auto& node:scene_.nodes)if(node.Interactive())Command(node.id,[this,id=node.id]{CalendarPickerCommand(id);});
    return scene_.height;
}
void SystemPanelModel::CalendarPickerCommand(std::string_view id)
{
    if(!CalendarEditing()||!calendarPicker_)return;
    const bool multiple=calendarPickerField_=="calendar.edit.date"&&calendarMode_=="dates";
    if(multiple&&id=="picker.confirm"&&calendarPickerDates_.empty())return;
    const auto today=source_.calendar.today?source_.calendar.today():calendar::CalendarService::CurrentLocalNow().date;
    if(multiple&&(id.starts_with("picker.day:")||id=="picker.today"))
    {
        const auto selected=id=="picker.today"?today:std::string(id.substr(11));
        if(!calendar::CalendarService::GetDateInfo(selected))return;
        const auto found=std::lower_bound(calendarPickerDates_.begin(),calendarPickerDates_.end(),selected);
        if(found!=calendarPickerDates_.end()&&*found==selected)calendarPickerDates_.erase(found);
        else if(calendarPickerDates_.size()<366)calendarPickerDates_.insert(found,selected);
        else return;
    }
    const auto result=calendarPicker_->Invoke(id,today);
    if(result==ui::DateTimePicker::Result::Confirmed)
    {
        if(calendarPickerField_=="calendar.edit.endDate")
        {calendarRule_.endDate=calendarPicker_->Date();calendarDiscardConfirmed_=false;}
        else if(calendarPickerField_=="calendar.edit.date"&&calendarMode_=="dates")
        {
            calendarRule_.dates=calendarPickerDates_;
            calendarText_["calendar.edit.date"]=Wide(calendarRule_.dates.front());
            calendarDiscardConfirmed_=false;
        }
        else
        {
            calendarText_[calendarPickerField_]=calendarPicker_->Type()==ui::DateTimePicker::Kind::Date?
                Wide(calendarPicker_->Date()):ui::DateTimePicker::TimeText(calendarPicker_->Minutes());
            if(calendarPickerField_=="calendar.edit.date"&&calendarMode_!="single")
                calendarRule_.startDate=calendarPicker_->Date();
            calendarDiscardConfirmed_=false;
        }
        calendarEditor_->error.clear();
    }
    if(result==ui::DateTimePicker::Result::Confirmed||result==ui::DateTimePicker::Result::Cancelled)
    {
        calendarFocus_=calendarPickerField_;calendarPicker_.reset();calendarPickerDates_.clear();calendarPickerField_.clear();scroll_=calendarEditorScroll_;
    }
    else if(result==ui::DateTimePicker::Result::Changed&&calendarPicker_->Type()==ui::DateTimePicker::Kind::Date)scroll_=0;
}
void SystemPanelModel::Resources()
{
    const char* labels[4]{};std::array<std::wstring,4> values{L"—",L"—",L"—",L"—"};std::wstring subtitle;bool available=false;const std::string topic=Topic(action_);
    const auto title=_LW(action_==StatusBarAction::Cpu?"statusBar.cpu":action_==StatusBarAction::Memory?"statusBar.memory":action_==StatusBarAction::Gpu?"statusBar.gpu":"statusBar.traffic");
    const auto now=source_.now?source_.now():0;
    const auto fresh=[now](std::int64_t stamp){return stamp<=0||now<=0||static_cast<double>(now)-static_cast<double>(stamp)<=5000;};
    if(action_==StatusBarAction::Gpu&&page_=="gpu-select")Header(title);
    float y=0;bool gpuSelector=false;
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
            subtitle=Wide(a.name);const bool sampled=v&&!v->warmingUp&&fresh(v->timestampMs);
            available=sampled&&a.usageAvailable&&InRange(a.usagePercent,100);
            if(available)values[0]=Percent(a.usagePercent);
            if(sampled&&a.dedicatedUsageAvailable)values[1]=Bytes(a.dedicatedUsedBytes);
            if(sampled&&a.sharedUsageAvailable)values[2]=Bytes(a.sharedUsedBytes);
            if(a.dedicatedMemoryBytes)values[3]=Bytes(a.dedicatedMemoryBytes);
        }
        if(page_=="gpu-select")
        {
            y=bodyStart_;
            for(const auto& a:list){auto& n=Add("gpu:"+a.id,ui::Role::ListItem,Rect(16,y,scene_.width-32,48),Wide(a.name));n.selected=a.id==gpu_;Command(n.id,[this,id=a.id]{gpu_=id;Select("");});y+=52;}
            if(list.empty()){Add("gpu.empty",ui::Role::Text,Rect(16,y,408,44),_LW("resourcePanel.unavailable"));y+=52;}
            Finish(y,false);return;
        }
        gpuSelector=list.size()>1;
    }
    else
    {
        labels[0]="resourcePanel.download";labels[1]="resourcePanel.upload";labels[2]="resourcePanel.received";labels[3]="resourcePanel.sent";
        if(auto v=source_.traffic?source_.traffic():std::nullopt;v&&v->available&&!v->warmingUp&&fresh(v->timestampMs))
        {available=true;values={StatusBarRate(v->downloadBytesPerSecond),StatusBarRate(v->uploadBytesPerSecond),Bytes(v->receivedBytes),Bytes(v->sentBytes)};}
    }
    const auto points=source_.history?source_.history(topic,gpu_):std::vector<wr::WidgetResourcePoint>{};
    const bool traffic=action_==StatusBarAction::Traffic;
    const auto valid=[traffic](const std::optional<double>& value){return value&&InRange(*value,traffic?(std::numeric_limits<double>::max)():100);};
    if(action_!=StatusBarAction::Memory&&(points.empty()||!fresh(points.back().timestampMs)||!valid(points.back().primary)))
    {available=false;values[0]=L"—";}
    if(traffic&&(points.empty()||!fresh(points.back().timestampMs)||!valid(points.back().secondary)))values[1]=L"—";
    bodyStart_=0;auto& chart=Add("resource.chart",ui::Role::Chart,Rect(0,0,scene_.width,152));chart.fillPaths=true;double maximum=traffic?1024:100;
    const auto end=points.empty()?now:(std::max)(now,points.back().timestampMs);
    if(traffic)for(const auto& p:points)if(p.timestampMs>=end-60000&&p.timestampMs<=end)for(const auto& value:{p.primary,p.secondary})if(valid(value))maximum=(std::max)(maximum,*value);
    for(int channel=0;channel<(traffic?2:1);++channel)
    {
        std::vector<D2D1_POINT_2F> path;std::int64_t previous=0;
        const auto flush=[&]{if(!path.empty()){chart.paths.push_back(std::move(path));chart.dashedPaths.push_back(channel==1);}path.clear();};
        for(const auto& p:points)
        {
            const auto value=channel?p.secondary:p.primary;const bool usable=valid(value)&&p.timestampMs>=end-60000&&p.timestampMs<=end;
            if(!usable||(!path.empty()&&(p.timestampMs<=previous||p.timestampMs-previous>2500)))
            {flush();}
            if(usable){path.push_back({scene_.width*static_cast<float>(1.-static_cast<double>(end-p.timestampMs)/60000),150-148.f*static_cast<float>(std::clamp(*value/maximum,0.,1.))});previous=p.timestampMs;}
        }
        flush();
    }
    y=164;auto& heading=Add("resource.title",ui::Role::Text,Rect(16,y,scene_.width-32,30),title);heading.bold=true;heading.fontSize=17;y+=36;
    if(gpuSelector){Add("gpu.select",ui::Role::Button,Rect(16,y,scene_.width-32,38),subtitle,L"\uE7F4");Command("gpu.select",[this]{Select("gpu-select");});y+=46;}
    else if(!subtitle.empty()){Add("resource.subtitle",ui::Role::Text,Rect(16,y,scene_.width-32,26),subtitle).fontSize=12;y+=30;}
    if(traffic)
    {
        const float half=(scene_.width-32)/2;
        for(int channel=0;channel<2;++channel)
        {
            const float left=16+channel*half;const auto id="resource.legend:"+std::to_string(channel);
            auto& sample=Add(id,ui::Role::Chart,Rect(left,y,28,22));sample.chartGrid=false;sample.paths={{{left,y+11},{left+28,y+11}}};sample.dashedPaths={channel==1};
            Add(id+".label",ui::Role::Text,Rect(left+36,y,half-36,22),_LW(labels[channel])).fontSize=12;
        }
        y+=30;
    }
    if(!available){Add("resource.status",ui::Role::Text,Rect(16,y,408,28),_LW(points.empty()?"resourcePanel.waiting":"resourcePanel.unavailable")).fontSize=12;y+=34;}
    const float cardWidth=(scene_.width-44)/2;const int count=action_==StatusBarAction::Cpu?2:4;for(int i=0;i<count;++i){auto& n=Add("resource.card:"+std::to_string(i),ui::Role::Card,Rect(16+(i%2)*(cardWidth+12),y+(i/2)*88.f,cardWidth,80),values[i]);n.fontSize=22;n.bold=true;n.detail=_LW(labels[i]);}y+=count==2?88:176;Finish(y,false);
}
}
