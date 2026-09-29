#include "system_panel_preview.h"
#include "system_panel_model.h"
#include "system_calendar_editor.h"
#include "system_control_prompt.h"
#include "calendar_display.h"
#include "l10n.h"
#include "preview_png_writer.h"
#include "widget_preview_stage.h"
#include "widget_system_control_data.h"
#include "widget_view_accessibility.h"

#include <d2d1_1helper.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

namespace snowdesktop
{
namespace
{
namespace ui = native_ui;
namespace wr = widget_runtime;
namespace j = system_control::json;
using Microsoft::WRL::ComPtr;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void Require(HRESULT result)
{
    if (FAILED(result)) throw std::runtime_error("native panel rendering failed: " + std::to_string(result));
}

void CheckControlPromptVisuals(const native_component_preview::Request& request,
    native_component_preview::Result& result,const PersonalizationSettings& appearance)
{
    // Independent forms remain the external Lua-task confirmation boundary.
    // The native panel exercises its own inline/page forms below instead.
    for(const auto& preview:RenderSystemControlPromptPreviews(appearance,request.dpi))
    {
        const auto name="prompt-"+preview.name;
        const auto path=request.outputDirectory/(request.component+"-"+name+".png");
        Require(preview.width>0&&preview.height>0&&preview.pixels.size()==
            static_cast<std::size_t>(preview.width)*preview.height,"system prompt produced invalid pixels");
        if(!preview_png::Save(path,preview.width,preview.height,preview.pixels,result.error))
            throw std::runtime_error(result.error);
        result.outputs.push_back({request.component,name,path,false,false,false,false,false,false,
            static_cast<int>(std::lround(appearance.cornerRadius*request.dpi/96.f)),preview.width,preview.height,0,0});
        Require(preview.width<=static_cast<int>(std::lround(480*request.dpi/96.f))&&
            preview.height<=static_cast<int>(std::lround(720*request.dpi/96.f)),
            "system prompt ignored its compact form bounds");
        if(appearance.cornerRadius>=2)
            Require((preview.pixels.front()>>24)==0,"system prompt lost the shared rounded corner mask");
    }
}

// Sources below are fixtures at the production model boundary. They never
// construct a device service, tray collector, HWND, XAML runtime or subscription
// to the user's data. Unexpected external commands fail the export immediately.
struct PreviewState
{
    bool unavailable = false, bluetoothOn = true, emptyMedia = false, agenda = false;
    bool allowMute = false, outputMuted = false, inputMuted = false;
    std::string manySection, requestedDate, outputEndpoint = "audio-output-preview";
    std::vector<system_control::Completion> completions;
    std::set<std::string> subscriptions;
    std::vector<std::pair<std::string, std::string>> muteRequests;
    unsigned scans = 0, closes = 0, reads = 0, trayChanges = 0, nativeControls = 0, calendarBatches = 0;
    std::set<std::string> requestedDates;
    StatusBarSettings savedTraySettings;
    tray::Snapshot tray;
    wr::WidgetCpuDataSnapshot cpu;
    wr::WidgetMemoryDataSnapshot memory;
    wr::WidgetGpuDataSnapshot gpu;
    wr::WidgetNetworkTrafficDataSnapshot traffic;
    wr::WidgetResourceHistory history;
    std::int64_t now = 100000;
};

tray::Snapshot TrayFixture()
{
    tray::Snapshot snapshot; snapshot.connected = true; snapshot.revision = 1;
    const SHSTOCKICONID icons[]{SIID_SHIELD, SIID_WORLD, SIID_DRIVEFIXED, SIID_FOLDER,
        SIID_INFO, SIID_WARNING, SIID_APPLICATION, SIID_RECYCLER};
    const wchar_t* names[]{L"Snow Shield", L"Snow Sync", L"Snow Drive", L"Snow Notes",
        L"Snow Info", L"Snow Alerts", L"Snow Tools", L"Hidden application"};
    for (unsigned i = 0; i < std::size(icons); ++i)
    {
        SHSTOCKICONINFO info{sizeof(info)};
        Require(SHGetStockIconInfo(icons[i], SHGSI_ICON | SHGSI_SMALLICON, &info));
        struct Pixels
        {
            HICON icon = nullptr; HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ old = nullptr;
            ~Pixels() { if (old) SelectObject(dc, old); if (bitmap) DeleteObject(bitmap); if (dc) DeleteDC(dc); if (icon) DestroyIcon(icon); }
        } bitmap;
        bitmap.icon = info.hIcon; bitmap.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        format.bmiHeader.biWidth = 32; format.bmiHeader.biHeight = -32;
        format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32; format.bmiHeader.biCompression = BI_RGB;
        void* bytes = nullptr;
        bitmap.bitmap = CreateDIBSection(bitmap.dc, &format, DIB_RGB_COLORS, &bytes, nullptr, 0);
        Require(bitmap.dc && bitmap.bitmap && bytes, "cannot construct tray fixture pixels");
        bitmap.old = SelectObject(bitmap.dc, bitmap.bitmap); std::memset(bytes, 0, 32 * 32 * 4);
        Require(DrawIconEx(bitmap.dc, 0, 0, bitmap.icon, 32, 32, 0, nullptr, DI_NORMAL) != FALSE,
            "cannot draw tray fixture icon");
        GdiFlush();
        tray::Icon icon; icon.key = icon.persistentKey = "preview-" + std::to_string(i);
        icon.tip = names[i]; icon.width = icon.height = 32;
        icon.pixels.assign(static_cast<const std::uint32_t*>(bytes), static_cast<const std::uint32_t*>(bytes) + 1024);
        if (i == 7) icon.state = NIS_HIDDEN;
        snapshot.icons.push_back(std::move(icon));
    }
    return snapshot;
}

void FillResourceFixture(PreviewState& state, std::string_view preset)
{
    state.cpu.available = true; state.cpu.warmingUp = false; state.cpu.usagePercent = preset == "idle" ? 0 : 13;
    state.cpu.logicalProcessors = 24; state.cpu.name = "Example 24-thread processor";
    state.memory.available = true; state.memory.totalBytes = 32ull << 30; state.memory.usedBytes = 12ull << 30;
    state.memory.freeBytes = 20ull << 30; state.memory.commitUsedBytes = 18ull << 30; state.memory.commitLimitBytes = 48ull << 30;
    state.traffic.available = true; state.traffic.warmingUp = false; state.traffic.connected = true;
    state.traffic.downloadBytesPerSecond = 2560000; state.traffic.uploadBytesPerSecond = 128000;
    state.traffic.receivedBytes = 12ull << 30; state.traffic.sentBytes = 1ull << 30;
    state.gpu.available = true; state.gpu.warmingUp = false;
    for (int i = 0; i < 2; ++i)
    {
        wr::WidgetGpuAdapterDataSnapshot adapter;
        adapter.id = "gpu-preview-" + std::to_string(i);
        adapter.name = i ? "Example integrated GPU" : "Example discrete GPU";
        adapter.usageAvailable = adapter.dedicatedUsageAvailable = adapter.sharedUsageAvailable = !(i && preset == "gpu-partial");
        adapter.usagePercent = i ? 61 : 23; adapter.dedicatedMemoryBytes = i ? 0 : 8ull << 30;
        adapter.dedicatedUsedBytes = i ? 0 : 2ull << 30; adapter.sharedUsedBytes = (i ? 512ull : 128ull) << 20;
        state.gpu.adapters.push_back(std::move(adapter));
    }
    for (int i = 0; i <= 60; ++i)
    {
        const auto time = state.now - 60000 + i * 1000;
        if (preset == "gap" && i >= 35 && i <= 39) continue;
        const auto percent = i == 60 ? state.cpu.usagePercent : 12 + (i % 9) * 2. + (i % 7) * .6;
        std::optional<double> cpu = preset == "idle" ? 0. : percent;
        if (preset == "gap" && i >= 20 && i <= 25) cpu.reset();
        state.history.Append("system.cpu", {}, {time, cpu, {}});
        state.history.Append("system.memory", {}, {time, 37.5 + (i % 6) * .2, {}});
        state.history.Append("system.network.traffic", {}, {time, i == 60 ? 2560000. : 1500000. + (i % 13) * 100000,
            i == 60 ? 128000. : 60000. + (i % 8) * 10000});
        for (int gpu = 0; gpu < 2; ++gpu)
            state.history.Append("system.gpu", "gpu-preview-" + std::to_string(gpu), {time,
                preset == "gpu-partial" && gpu ? std::nullopt : std::optional<double>(gpu ? 61 : 23), {}});
    }
    if (preset == "warming" || preset == "unavailable")
    {
        state.cpu.available = false; state.cpu.warmingUp = preset == "warming";
        state.history.Clear("system.cpu"); state.history.Append("system.cpu", {}, {state.now, {}, {}});
    }
}

SystemPanelSource FixtureSource(const std::shared_ptr<PreviewState>& state)
{
    SystemPanelSource source;
    source.current = [state](std::string_view topic) -> std::optional<system_control::Snapshot> {
        ++state->reads;
        auto value = wr::PreviewSystemControlData(topic, state->unavailable);
        if (topic == "bluetooth.devices" && !state->unavailable && !state->bluetoothOn)
        {
            value.object["radios"].array.front().object["enabled"] = j::Boolean(false);
            value.object["devices"].array.clear();
        }
        if (topic == "audio.output.volume") ParseJson(R"({"endpointId":"audio-output-preview","volume":0.42,"muted":false})", value);
        if (topic == "audio.output.volume") value.object["endpointId"] = j::Text(state->outputEndpoint);
        if (topic == "audio.devices")
            for (auto& device : value.object["devices"].array)
                if (j::String(device,"direction") == "output") device.object["id"] = j::Text(state->outputEndpoint);
        if (topic == "audio.output.volume" || topic == "audio.input.volume")
            value.object["muted"] = j::Boolean(topic == "audio.output.volume" ? state->outputMuted : state->inputMuted);
        if (topic == "system.power.plans" && !state->unavailable)
        {
            value.object["onAC"] = j::Boolean(true); value.object["charging"] = j::Boolean(true);
        }
        if (!state->unavailable && !state->manySection.empty())
        {
            auto* items = topic == "audio.devices" && state->manySection == "audio" ? &value.object["devices"].array :
                topic == "bluetooth.devices" && state->manySection == "bluetooth" ? &value.object["devices"].array :
                topic == "network.wifi" && state->manySection == "wifi" ? &value.object["interfaces"].array.front().object["networks"].array : nullptr;
            if (items && !items->empty())
            {
                const char* label = state->manySection == "wifi" ? "ssid" : "name";
                items->front().object[label] = j::Text(state->manySection == "wifi" ?
                    "SnowDesktop Guest Network 5 GHz" : "SnowDesktop Studio — Wireless Headphones and Conference Audio Device");
                const auto first = items->front();
                for (int i = 1; i <= 12; ++i)
                {
                    auto item = first; item.object["id"] = j::Text(state->manySection + "-extra-" + std::to_string(i));
                    item.object[label] = j::Text("Snow " + state->manySection + " " + std::to_string(i));
                    item.object["connected"] = j::Boolean(false); item.object["isDefault"] = j::Boolean(false);
                    if (state->manySection == "bluetooth") item.object["canConnect"] = j::Boolean(i % 2 == 0);
                    items->push_back(std::move(item));
                }
            }
        }
        return system_control::Snapshot{!state->unavailable, std::move(value), state->unavailable ? "unavailable" : "", 0, 1};
    };
    source.start = [state](system_control::Request request) -> std::uint64_t {
        if (state->allowMute && (request.name == "audio.output.setMute" || request.name == "audio.input.setMute"))
        {
            const auto muted = request.arguments.at("muted");
            Require(muted == "0" || muted == "1", "invalid offline mute argument");
            state->muteRequests.emplace_back(request.name, muted);
            (request.name == "audio.output.setMute" ? state->outputMuted : state->inputMuted) = muted == "1";
            const auto id = static_cast<std::uint64_t>(100 + state->muteRequests.size());
            system_control::Completion completed; completed.id = id; completed.ok = true;
            state->completions.push_back(std::move(completed)); return id;
        }
        Require(request.name == "network.wifi.scan" && request.arguments.at("interfaceId") == "wifi-preview",
            "offline panel unexpectedly dispatched a device mutation");
        const auto id=++state->scans;
        system_control::Completion completion;completion.id=id;completion.ok=true;
        state->completions.push_back(std::move(completion));return id;
    };
    source.completions = [state] { return std::exchange(state->completions, {}); };
    source.subscribe = [state](std::string topic, std::chrono::milliseconds) { state->subscriptions.insert(std::move(topic)); };
    source.unsubscribe = [state](std::string_view topic) { state->subscriptions.erase(std::string(topic)); };
    source.close = [state] { ++state->closes; state->subscriptions.clear(); };
    source.settings = [](const wchar_t*) { throw std::runtime_error("offline panel must not launch Windows Settings"); };
    source.nativeControls = [state] { ++state->nativeControls; };
    source.prompt = [](system_control::Request&) -> bool { throw std::runtime_error("offline panel must not show confirmation or credential prompts"); };
    source.media = [state]() -> std::optional<wr::WidgetMediaSessionsDataSnapshot> {
        ++state->reads;
        wr::WidgetMediaSessionsDataSnapshot result; result.available = !state->unavailable;
        if (!state->unavailable && !state->emptyMedia)
        {
            wr::WidgetMediaSessionDataSnapshot session;
            session.id = "media-preview"; session.sourceName = "Snow Music";
            session.title = _L("app.widget_preview.api.calendar_publish"); session.artist = "Snow Music";
            session.playbackStatus = "playing"; session.current = true;
            session.controls.canPrevious = session.controls.canNext = session.controls.canPlayPause = true;
            result.currentSessionId = session.id; result.sessions.push_back(std::move(session));
        }
        return result;
    };
    source.artwork = [] { return std::optional<wr::WidgetMediaArtworkDataSnapshot>{}; };
    source.cpu = [state] { ++state->reads; return state->cpu; };
    source.memory = [state] { ++state->reads; return state->memory; };
    source.gpu = [state] { ++state->reads; return state->gpu; };
    source.traffic = [state] { ++state->reads; return state->traffic; };
    source.history = [state](auto topic, auto identity) { ++state->reads; return state->history.Read(topic, identity); };
    source.now = [state] { return state->now; };
    source.tray = [state] { ++state->reads; return state->tray; };
    source.trayChanged = [state](const StatusBarSettings& settings) { ++state->trayChanges; state->savedTraySettings = settings; };
    source.calendar.today = [] { return std::string("2026-09-26"); };
    source.calendar.manage = [] { throw std::runtime_error("offline panel must not open calendar settings"); };
    source.calendar.secondaryDate = [state](const std::string& date) {
        if (!state->agenda) return std::string{};
        calendar::DisplayPreferences preferences; preferences.enabled = true;
        const auto days = calendar::Annotate(date, date, preferences, Locale::Instance().GetEffectiveLanguage());
        Require(!days.empty() && days.front().calendarAvailable && !days.front().fullDate.empty(),
            "secondary calendar fixture is unavailable");
        return days.front().fullDate;
    };
    source.calendar.secondaryDates = [state](const std::string& from,const std::string& to) {
        ++state->calendarBatches;std::map<std::string,std::string> result;
        if(!state->agenda)return result;
        calendar::DisplayPreferences preferences;preferences.enabled=true;
        for(const auto& day:calendar::Annotate(from,to,preferences,Locale::Instance().GetEffectiveLanguage()))
            if(day.calendarAvailable)result[day.date]=day.fullDate;
        return result;
    };
    source.calendar.secondaryRevision = [state] {return state->agenda?std::string("secondary-on"):std::string("secondary-off");};
    source.calendar.secondaryAnnotations = [state](const std::string& from,const std::string& to) {
        ++state->calendarBatches;
        if(!state->agenda)return std::vector<calendar::DayAnnotation>{};
        calendar::DisplayPreferences preferences;preferences.enabled=true;
        return calendar::Annotate(from,to,preferences,Locale::Instance().GetEffectiveLanguage());
    };
    source.calendar.secondaryCalendarId = [] {return std::string("chinese");};
    source.calendar.events = [state](const std::string& date) {
        ++state->reads; state->requestedDate = date;state->requestedDates.insert(date);
        std::vector<calendar::CalendarEvent> events;
        if (!state->agenda) return events;
        calendar::CalendarEvent first; first.id = "preview-brunch"; first.revision = 1;
        first.date = date; first.title = _L("app.widget_preview.api.calendar_review"); first.startMinutes = 630; first.endMinutes = 690;
        calendar::CalendarEvent second; second.id = "preview-weekend"; second.revision = 1;
        second.date = date; second.title = _L("app.widget_preview.api.calendar_publish"); second.allDay = true;
        events.push_back(std::move(first)); events.push_back(std::move(second)); return events;
    };
    return source;
}

const ui::Node& Node(const ui::Scene& scene, std::string_view id)
{
    const auto* node = scene.Find(id);
    if (!node) throw std::runtime_error("native panel omitted required content: " + std::string(id));
    return *node;
}
bool HasArea(D2D1_RECT_F rect) { return rect.right > rect.left && rect.bottom > rect.top; }
D2D1_RECT_F Intersection(D2D1_RECT_F a, D2D1_RECT_F b)
{
    return {(std::max)(a.left,b.left), (std::max)(a.top,b.top), (std::min)(a.right,b.right), (std::min)(a.bottom,b.bottom)};
}
void CheckLayout(const ui::Scene& scene)
{
    Require(std::isfinite(scene.width) && std::isfinite(scene.height) && scene.width > 0 && scene.height > 0,
        "native panel has invalid dimensions");
    const auto viewport = D2D1::RectF(0,0,scene.width,scene.height);
    std::set<std::string> identities;
    for (std::size_t i = 0; i < scene.cards.size(); ++i)
    {
        const auto card = scene.cards[i];
        Require(HasArea(card) && card.left >= 0 && card.top >= 0 && card.right <= scene.width && card.bottom <= scene.height,
            "native panel background escaped its viewport");
        for (std::size_t j = 0; j < i; ++j)
            Require(!HasArea(Intersection(card,scene.cards[j])), "native panel cards overlap");
    }
    for (const auto& node : scene.nodes)
    {
        Require(identities.insert(node.id).second, "native panel duplicated a stable target identity");
        Require(std::isfinite(node.bounds.left) && std::isfinite(node.bounds.top) &&
            std::isfinite(node.bounds.right) && std::isfinite(node.bounds.bottom) && HasArea(node.bounds),
            "native panel has invalid content bounds");
        auto visible = Intersection(node.bounds,viewport);
        if (HasArea(node.clip)) visible = Intersection(visible,node.clip);
        if (!HasArea(visible) || !node.Interactive()) continue;
        const D2D1_POINT_2F center{(visible.left+visible.right)/2,(visible.top+visible.bottom)/2};
        Require(scene.Hit(center) == &node, "native panel input does not match its visible target");
    }
    if (scene.cards.size() == 2)
    {
        const auto a = scene.cards[0], b = scene.cards[1];
        Require(b.top > a.bottom && !scene.Hit({scene.width/2,(a.bottom+b.top)/2},false),
            "the transparent media gap must not receive input");
    }
}

void CheckTooltipViewport()
{
    ui::Scene scene;scene.width=200;scene.height=120;
    ui::Node row;row.bounds={-20,60,240,140};row.clip={12,75,180,108};
    const auto clipped=scene.VisibleBounds(row);
    Require(clipped.left==12&&clipped.top==75&&clipped.right==180&&clipped.bottom==108,
        "a partially scrolled row anchors its tooltip outside the visible part of the row");
    row.clip={};const auto edge=scene.VisibleBounds(row);
    Require(edge.left==0&&edge.top==60&&edge.right==200&&edge.bottom==120,
        "a tooltip anchor extends beyond the panel viewport");
    row.bounds={12,140,180,176};
    Require(!HasArea(scene.VisibleBounds(row)),"an offscreen row retained a tooltip anchor");
}

void CheckControls(SystemPanelModel& model, const std::shared_ptr<PreviewState>& state, std::string_view preset)
{
    const bool media = !state->unavailable && !state->emptyMedia;
    Require(model.View().cards.size() == (media ? 2u : 1u), "media presence did not determine the separate card layout");
    if (media)
    {
        const auto& scene = model.View(); const auto card = scene.cards.back();
        Require(card.bottom-card.top <= 64 && !scene.Find("media.details"), "media must remain a narrow single-row card");
        for (const auto id : {"media.artwork","media.title","media.previous","media.toggle","media.next"})
        {
            const auto bounds = Node(scene,id).bounds;
            Require(bounds.top >= card.top && bounds.bottom <= card.bottom, "media control escaped its card");
        }
    }
    if (preset == "bluetooth-off")
        Require(Node(model.View(),"radio:bluetooth").enabled && !Node(model.View(),"radio:bluetooth").selected,
            "disabled Bluetooth radio was confused with unavailable hardware");
    if (preset.ends_with("-many"))
    {
        const auto& scene = model.View();
        const bool clipped = std::any_of(scene.nodes.begin(),scene.nodes.end(),[](const auto& node) {
            return HasArea(node.clip) && node.bounds.bottom > node.clip.bottom;
        });
        Require(!clipped || scene.Find("scrollbar"), "clipped device lists have no overflow affordance");
    }
    Require(state->scans == (preset == "wifi" || preset == "wifi-many" ? 1u : 0u),
        "a fixture scanned Wi-Fi outside explicit entry to the Wi-Fi page");
}
void CheckMute(SystemPanelModel& model, const std::shared_ptr<PreviewState>& state, bool overview)
{
    state->allowMute = true;
    for (const auto direction : {std::string("output"),std::string("input")})
    {
        if (overview && direction == "input") continue;
        const auto id = "audio." + direction;
        const auto volumeId = id+".volume:"+(direction == "output" ? state->outputEndpoint : "audio-input-preview");
        const auto original = Node(model.View(),id+".mute");
        const float level = Node(model.View(),volumeId).value;
        for (bool muted : {true,false})
        {
            Require(model.Invoke(id+".mute"), "audio mute target cannot be invoked");
            const auto& button = Node(model.View(),id+".mute");
            Require((direction == "output" ? state->outputMuted : state->inputMuted) == muted &&
                Node(model.View(),volumeId).value == level &&
                (button.glyph != original.glyph) == muted && (button.tooltip != original.tooltip) == muted,
                "audio mute changed the level or did not refresh its visible and accessible action");
            Require(!(direction == "output" ? state->inputMuted : state->outputMuted), "mute crossed audio directions");
        }
    }
    state->allowMute = false;
}

void CheckClosedCallbacks()
{
    // The panel owns an unsubmitted confirmation. Closing it must discard the
    // draft without ever invoking the independent external-task prompt.
    auto state = std::make_shared<PreviewState>();
    auto source = FixtureSource(state);
    SystemPanelModel* active = nullptr;
    unsigned starts = 0, prompts = 0;
    source.start = [&](system_control::Request) { ++starts; return std::uint64_t{1}; };
    source.prompt = [&](system_control::Request&) { ++prompts; active->Close(); return true; };
    SystemPanelModel model(std::move(source), {}, StatusBarAction::ControlCenter);
    active = &model; model.Select("power");
    Require(model.Invoke("power.shutdown")&&model.View().Find("control.confirm")&&prompts==0&&starts==0,
        "native confirmation escaped its panel or submitted before explicit confirmation");
    model.Close();
    Require(!model.Invoke("control.confirm") && prompts == 0 && starts == 0 && state->closes == 1,
        "closed confirmation submitted an obsolete action or closed its source twice");
    const auto reads = state->reads;
    model.Refresh(); model.Close();
    Require(state->reads == reads && state->subscriptions.empty() && state->closes == 1,
        "closed confirmation resumed subscriptions");

    auto nextState = std::make_shared<PreviewState>();
    auto nextSource = FixtureSource(nextState);
    nextSource.calendar.manage = [&] { active->Close(); };
    SystemPanelModel next(std::move(nextSource), {}, StatusBarAction::Calendar);
    active = &next;
    Require(!next.Invoke("calendar.manage") && nextState->closes == 1,
        "reentrant settings callback retained a closed model");
}

D2D1_POINT_2F VisibleCenter(const ui::Scene& scene, std::string_view id)
{
    const auto& node = Node(scene,id);
    auto bounds = Intersection(node.bounds,D2D1::RectF(0,0,scene.width,scene.height));
    if (HasArea(node.clip)) bounds = Intersection(bounds,node.clip);
    Require(HasArea(bounds), "input fixture target is outside its viewport");
    return {(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2};
}

void CheckControlInput(SystemPanelModel& model, const std::shared_ptr<PreviewState>& state, float available)
{
    // Exercise the production shared-region adapter, not synthetic Click calls.
    // Only page navigation is dispatched; value/context results stay in the fixture.
    ui::Input input;
    auto point = VisibleCenter(model.View(),"audio.more");
    Require(input.Press(model.View(),point), "left mouse press did not bind a native button");
    auto action = input.Release(model.View(),point);
    Require(action.kind == ui::InputResult::Kind::Invoke && action.id == "audio.more" &&
        model.Invoke(action.id) && model.Page() == "audio", "left button did not activate its production page action");
    input.Sync(model.View());
    Require(input.Focus("back"), "native back button is not keyboard focusable");
    action = input.Key(model.View(),VK_RETURN,false);
    Require(action.kind == ui::InputResult::Kind::Invoke && action.id == "back" &&
        model.Invoke(action.id) && model.Page().empty(), "keyboard activation did not return to overview");

    point = VisibleCenter(model.View(),"wifi.more");
    Require(input.Press(model.View(),point,true), "right mouse press did not bind a native target");
    action = input.Release(model.View(),point,true);
    Require(action.kind == ui::InputResult::Kind::Context && action.id == "wifi.more" && input.Pressed().empty(),
        "right release was lost, invoked a left action, or retained capture");
    Require(input.Press(model.View(),point), "mismatched-button fixture could not press its target");
    Require(input.Release(model.View(),point,true).kind == ui::InputResult::Kind::None && input.Pressed().empty(),
        "mismatched mouse buttons incorrectly activated a target");

    const auto volumeId = "audio.output.volume:"+state->outputEndpoint;
    point = VisibleCenter(model.View(),volumeId);
    const D2D1_POINT_2F outside{model.View().width+40,point.y};
    Require(input.Press(model.View(),point), "native slider did not acquire its target");
    action = input.Move(model.View(),outside);
    Require(action.kind == ui::InputResult::Kind::Value && action.id == volumeId && action.value == 1.f,
        "captured slider did not clamp an outside drag to its upper endpoint");
    action = input.Release(model.View(),outside);
    Require(action.kind == ui::InputResult::Kind::Value && action.id == volumeId && action.value == 1.f && input.Pressed().empty(),
        "captured slider could not release outside its bounds");

    Require(input.Press(model.View(),point), "replacement fixture could not press the old endpoint");
    const auto originalEndpoint = state->outputEndpoint;
    state->outputEndpoint = "{offline-device-"+std::string(300,'a')+"}";
    model.Refresh(available);
    Require(input.Release(model.View(),point).kind == ui::InputResult::Kind::None && input.Pressed().empty(),
        "replacing an audio device redirected an in-progress gesture to the new endpoint");
    const auto longId = "audio.output.volume:"+state->outputEndpoint;
    point = VisibleCenter(model.View(),longId);
    input.Sync(model.View());
    const auto regions = input.AccessibilityRegions();
    const auto region = std::find_if(regions.begin(),regions.end(),[&](const auto& entry) { return input.Identity(entry.key) == longId; });
    Require(region != regions.end() && region->key.size() <= 128 && region->events.contains("change") &&
        region->events.at("change").id.size() <= 128, "long device identity did not fit the shared input action contract");
    Require(input.Press(model.View(),point), "a long device identity could not receive pointer input");
    action = input.Release(model.View(),{model.View().width+40,point.y});
    Require(action.kind == ui::InputResult::Kind::Value && action.id == longId && action.value == 1.f,
        "hashed input identity did not resolve back to the complete device identity");
    Require(input.Focus(longId), "long device slider is not keyboard focusable");
    action = input.Key(model.View(),VK_HOME,false);
    Require(action.kind == ui::InputResult::Kind::Value && action.id == longId && action.value == 0.f,
        "keyboard range input did not preserve the device identity");
    Require(input.Press(model.View(),point), "cancel fixture could not press its slider");
    input.Cancel();
    Require(input.Release(model.View(),point).kind == ui::InputResult::Kind::None && input.Pressed().empty(),
        "canceled native input dispatched a later release");
    state->outputEndpoint = originalEndpoint; model.Refresh(available);
}

void CheckTrayInput(const ui::Scene& scene)
{
    ui::Input input;
    const std::string id = "tray:preview-1";
    const auto point = VisibleCenter(scene,id);
    const D2D1_POINT_2F outside{scene.width+50,scene.height+50};
    Require(input.Press(scene,point), "tray fixture did not acquire pointer capture");
    input.Move(scene,outside);
    Require(input.Dragging(), "tray motion outside the panel did not cross the drag threshold");
    auto action = input.Release(scene,outside);
    Require(action.kind == ui::InputResult::Kind::Drag && action.id == id && !input.Dragging() && input.Pressed().empty(),
        "a captured tray drag was lost when released outside the panel");
    Require(input.Press(scene,point,true), "tray context fixture could not press its target");
    action = input.Release(scene,point,true);
    Require(action.kind == ui::InputResult::Kind::Context && action.id == id,
        "tray right release did not preserve its application identity");
    Require(input.Press(scene,point), "tray removal fixture could not press its target");
    input.Move(scene,outside);
    auto removed = scene;
    std::erase_if(removed.nodes,[&](const auto& node) { return node.id == id; });
    Require(input.Release(removed,outside).kind == ui::InputResult::Kind::None && input.Pressed().empty(),
        "a removed tray application retained a captured drag action");
}

void CheckTrayDropTargets(const tray::Snapshot& fixture)
{
    auto state=std::make_shared<PreviewState>();state->tray=fixture;
    for(int i=8;i<28;++i){auto icon=fixture.icons.front();icon.key=icon.persistentKey="preview-"+std::to_string(i);state->tray.icons.push_back(std::move(icon));}
    StatusBarSettings settings;settings.pinnedTrayItems={"preview-0"};
    SystemPanelModel model(FixtureSource(state),settings,StatusBarAction::Tray);model.Refresh(96);
    const auto clip=model.ScrollViewport();const ui::Node* last=nullptr;std::vector<std::string> after;
    for(const auto& n:model.View().nodes)if(n.id.starts_with("tray:"))
    {
        if(n.bounds.top<clip.bottom&&n.bounds.bottom>clip.top)last=&n;
        else if(last&&n.bounds.top>=clip.bottom)after.push_back(n.id.substr(5));
    }
    Require(last&&after.size()>1,"tray stale-target fixture has no offscreen insertion candidates");
    const auto lastKey=last->id.substr(5);const auto stale=after.front(),next=after[1];
    std::erase_if(state->tray.icons,[&](const auto& icon){return icon.key==stale;});
    const D2D1_POINT_2F point{model.View().width-1,clip.bottom-1};
    const auto indicator=model.TrayDropIndicator("preview-0",point);
    Require(indicator&&indicator->right-indicator->left==2&&state->trayChanges==0&&model.Settings().pinnedTrayItems==settings.pinnedTrayItems,
        "tray drop preview mutated settings or lost its exact insertion indicator");
    Require(!model.TrayDropIndicator("missing",point)&&!model.TrayDropIndicator("preview-7",point)&&
        !model.TrayDropIndicator("preview-0",{model.View().width+1,point.y}),"invalid tray identities or points were advertised as acceptable drops");
    Require(model.Drop("preview-0",point)&&state->trayChanges==1,"advertised tray insertion could not be committed");
    const auto& order=state->savedTraySettings.trayOrder;
    const auto at=[&](const std::string& key){return std::find(order.begin(),order.end(),key);};
    Require(at(lastKey)<at("preview-0")&&at("preview-0")<at(next),"removed offscreen tray target silently changed a local insertion to global append");
    auto empty=std::make_shared<PreviewState>();empty->tray=fixture;empty->tray.icons.resize(1);
    SystemPanelModel emptyModel(FixtureSource(empty),settings,StatusBarAction::Tray);
    const auto target=emptyModel.TrayDropIndicator("preview-0",{16,16});
    Require(target&&target->right-target->left>2&&emptyModel.Drop("preview-0",{16,16})&&empty->savedTraySettings.pinnedTrayItems.empty(),
        "empty overflow grid did not advertise and accept the same visible unpin target");
}

void CheckLogicalFocus()
{
    ui::Scene scene;scene.width=160;scene.height=80;
    ui::Node first;first.id="first";first.role=ui::Role::Button;first.bounds={8,8,152,36};first.clip={0,0,160,80};first.text=L"Visible";
    scene.nodes.push_back(first);
    auto disabled=first;disabled.id="disabled";disabled.enabled=false;disabled.bounds={8,88,152,116};scene.nodes.push_back(disabled);
    auto slider=first;slider.id="offscreen-slider";slider.role=ui::Role::Slider;slider.bounds={8,128,152,156};slider.value=.5f;
    slider.text.clear();slider.tooltip=L"50%";slider.accessibilityLabel=L"Speaker volume";scene.nodes.push_back(slider);
    // The logical list may exceed the shared pointer bank's 256-region limit.
    // Only the first row is visible, so this must remain usable without changing
    // any public component limit or making hidden rows receive pointer input.
    for(unsigned i=0;i<300;++i)
    {
        auto row=first;row.id="offscreen-"+std::to_string(i);const float top=168+40.f*static_cast<float>(i);
        row.bounds={8,top,152,top+28};scene.nodes.push_back(std::move(row));
    }
    ui::Input input;input.Sync(scene);
    input.Key(scene,VK_TAB,false);Require(input.Focused()=="first","Tab missed the first logical control");
    input.Key(scene,VK_TAB,false);Require(input.Focused()==slider.id,"Tab skipped an offscreen control or focused a disabled row");
    input.Sync(scene);Require(input.Focused()==slider.id,"clipping cleared logical keyboard focus");
    input.Key(scene,VK_TAB,true);Require(input.Focused()=="first","Shift+Tab did not follow the complete logical order");
    Require(input.Focus(slider.id)&&!input.Focus(disabled.id),"programmatic focus rejected an offscreen control or accepted a disabled row");
    const auto up=input.Key(scene,VK_UP,false),right=input.Key(scene,VK_RIGHT,false);
    const auto down=input.Key(scene,VK_DOWN,false),left=input.Key(scene,VK_LEFT,false);
    Require(up.kind==ui::InputResult::Kind::Value&&up.id==slider.id&&std::abs(up.value-.52f)<.0001f&&up.value==right.value&&
        down.kind==ui::InputResult::Kind::Value&&std::abs(down.value-.48f)<.0001f&&down.value==left.value,
        "vertical slider keys did not use the shared range-control step");
    auto endpoint=scene;endpoint.nodes[2].value=1;
    const auto upper=input.Key(endpoint,VK_UP,false);endpoint.nodes[2].value=0;const auto lower=input.Key(endpoint,VK_DOWN,false);
    Require(upper.kind==ui::InputResult::Kind::Value&&upper.value==1&&lower.kind==ui::InputResult::Kind::Value&&lower.value==0,
        "a slider limit returned an unhandled key that could scroll its parent");
    const auto regions=input.AccessibilityRegions();
    Require(regions.size()==scene.nodes.size(),"offscreen semantic nodes were limited to the pointer viewport or region cap");
    const auto region=std::find_if(regions.begin(),regions.end(),[&](const auto& r){return input.Identity(r.key)==slider.id;});
    Require(region!=regions.end()&&region->accessibilityLabel=="Speaker volume","explicit stable accessibility name lost to a changing value tooltip");
    std::vector<wr::ViewAccessibilityNode> nodes;std::string error;
    Require(wr::CollectInteractionAccessibilityNodes({*region},scene.width,scene.height,region->key,nodes,error)&&
        nodes.size()==1&&nodes.front().offscreen&&nodes.front().focusable&&nodes.front().focused,
        "offscreen logical focus did not retain its UIA offscreen state");
    auto renamed=scene;renamed.nodes[2].accessibilityLabel=L"Headphone volume";
    Require(!scene.SameContent(renamed),"accessibility-only name changes were omitted from scene invalidation");
    Require(!input.Press(scene,{20,140})&&input.Release(scene,{20,140}).kind==ui::InputResult::Kind::None,
        "a completely clipped slider received physical pointer input");
    Require(input.Focus("offscreen-299"),"the last control beyond 256 logical nodes cannot be focused");
    input.Key(scene,VK_TAB,false);Require(input.Focused()=="first","complete logical Tab order did not wrap");
    input.Key(scene,VK_TAB,true);Require(input.Focused()=="offscreen-299","reverse logical Tab order lost its last node");
    auto removed=scene;removed.nodes.pop_back();input.Sync(removed);
    Require(input.Focused().empty(),"a deleted logical target kept keyboard focus");
    Require(input.Focus(slider.id),"slider focus fixture failed");removed.nodes[2].enabled=false;input.Sync(removed);
    Require(input.Focused().empty()&&input.Key(removed,VK_UP,false).kind==ui::InputResult::Kind::None,
        "a disabled offscreen slider retained keyboard actions");
    auto captured=scene;captured.nodes[0].role=ui::Role::Slider;captured.nodes[0].value=.5f;
    Require(input.Press(captured,{20,20}),"capture clipping fixture could not press its visible slider");
    captured.nodes[0].bounds={8,-60,152,-32};input.Sync(captured);
    Require(input.Pressed().empty()&&input.Release(captured,{20,20}).kind==ui::InputResult::Kind::None,
        "scrolling a captured control out of view retained its pending pointer command");
}

void CheckModelScrolling()
{
    auto state=std::make_shared<PreviewState>();state->manySection="audio";state->emptyMedia=true;
    auto source=FixtureSource(state);const auto originalStart=source.start;std::vector<float> volumeRequests;
    source.start=[&volumeRequests,originalStart](system_control::Request request)->std::uint64_t {
        if(request.name!="audio.output.setVolume")return originalStart(std::move(request));
        volumeRequests.push_back(std::stof(request.arguments.at("volume")));
        return 1000+static_cast<std::uint64_t>(volumeRequests.size()); // Intentionally no completion/readback yet.
    };
    SystemPanelModel model(std::move(source),StatusBarSettings{},StatusBarAction::ControlCenter);
    model.Refresh(320);model.Select("audio");
    const auto& scene=model.View();
    for(const auto* direction:{"output","input"})
    {
        const std::string prefix=std::string("audio.")+direction+".volume:";
        const auto slider=std::find_if(scene.nodes.begin(),scene.nodes.end(),[&](const auto& node){return node.id.starts_with(prefix);});
        Require(slider!=scene.nodes.end()&&slider->bounds.top>=slider->clip.top&&slider->bounds.bottom<=slider->clip.bottom,
            "many audio devices pushed everyday speaker/microphone volume controls out of the initial viewport");
    }
    const auto offscreen=std::find_if(scene.nodes.rbegin(),scene.nodes.rend(),[](const auto& node){
        return node.Interactive()&&HasArea(node.clip)&&node.bounds.top>=node.clip.bottom;
    });
    Require(offscreen!=scene.nodes.rend()&&model.MaximumScroll()>0,"audio scroll fixture has no offscreen action");
    const auto target=offscreen->id;const auto limit=scene.nodes.size()+1;
    ui::Input input;
    for(std::size_t i=0;i<limit&&input.Focused()!=target;++i)
    {
        Require(model.HandleKey(input,VK_TAB,false).kind==ui::InputResult::Kind::None,"Tab dispatched a device action");
        if(!input.Focused().empty())
        {
            const auto& focused=Node(model.View(),input.Focused());
            Require(!HasArea(focused.clip)||(focused.bounds.top>=focused.clip.top-.01f&&focused.bounds.bottom<=focused.clip.bottom+.01f),
                "production Tab routing left logical focus outside the scroll viewport");
        }
    }
    Require(input.Focused()==target&&model.ScrollOffset()>0,"Tab never revealed the last offscreen audio action");
    const auto volumeId="audio.output.volume:"+state->outputEndpoint;
    model.Reveal(volumeId);input.Sync(model.View());Require(input.Focus(volumeId),"audio slider could not regain logical focus");
    const float offset=model.ScrollOffset(),value=Node(model.View(),volumeId).value;
    const auto up=model.HandleKey(input,VK_UP,false),down=model.HandleKey(input,VK_DOWN,false);
    Require(up.kind==ui::InputResult::Kind::Value&&down.kind==ui::InputResult::Kind::Value&&up.id==volumeId&&down.id==volumeId&&
        std::abs(up.value-std::clamp(value+.02f,0.f,1.f))<.0001f&&std::abs(down.value-std::clamp(value-.02f,0.f,1.f))<.0001f&&model.ScrollOffset()==offset,
        "production Up/Down routing scrolled instead of adjusting the focused slider");
    Require(input.Focus("back"),"scroll navigation fixture lost its fixed header");
    model.HandleKey(input,VK_END,false);Require(model.ScrollOffset()==model.MaximumScroll(),"End did not reach the device-list bottom");
    model.HandleKey(input,VK_HOME,false);Require(model.ScrollOffset()==0,"Home did not return to the device-list top");
    model.HandleKey(input,VK_NEXT,false);Require(model.ScrollOffset()>0,"PageDown did not scroll the device list");
    model.HandleKey(input,VK_HOME,false);
    const auto clip=model.ScrollViewport();model.Wheel({1,clip.top+1},-.25f);
    Require(model.ScrollOffset()>0&&model.ScrollOffset()<42,"a quarter wheel notch was discarded or treated as a full notch");
    model.Scroll(-model.MaximumScroll());const auto geometry=model.ScrollbarGeometry();
    Require(geometry.CanDrag(),"scrollbar fixture has no thumb travel");
    const auto back=VisibleCenter(model.View(),"back");Require(input.Press(model.View(),back),"scrollbar cancellation fixture could not press its header");
    input.Cancel();model.DragScrollbar(0,geometry.ThumbTravel());
    Require(std::abs(model.ScrollOffset()-model.MaximumScroll())<=1&&
        input.Release(model.View(),back).kind==ui::InputResult::Kind::None,
        "scrollbar endpoint/cancellation dispatched a previous control or missed the end");
    model.DragScrollbar(geometry.maximum,-geometry.ThumbTravel());
    Require(model.ScrollOffset()==0&&state->muteRequests.empty()&&state->scans==0&&volumeRequests.empty(),
        "scrollbar travel did not return to the start or dispatched an unrelated device command");
    model.Reveal(volumeId);input.Sync(model.View());
    const auto name=[&] {
        const auto regions=input.AccessibilityRegions();
        const auto found=std::find_if(regions.begin(),regions.end(),[&](const auto& r){return input.Identity(r.key)==volumeId;});
        Require(found!=regions.end(),"volume UIA node disappeared");return found->accessibilityLabel;
    };
    const auto stableName=name();const float previous=Node(model.View(),volumeId).value;
    model.Wheel(VisibleCenter(model.View(),volumeId),.25f);model.Refresh(320);input.Sync(model.View());
    const auto& pending=Node(model.View(),volumeId);
    const auto percentage=std::to_wstring(static_cast<int>(std::lround(pending.value*100)))+L"%";
    Require(volumeRequests.size()==1&&std::abs(volumeRequests.front()-(previous+.005f))<.0001f&&
        std::abs(pending.value-volumeRequests.front())<.0001f&&pending.tooltip.find(percentage)!=std::wstring::npos&&
        pending.tooltip.find(_LW("controlCenter.working"))==std::wstring::npos&&pending.accessibilityLabel==_LW("statusBar.volume")&&
        !stableName.empty()&&name()==stableName&&stableName.find('%')==std::string::npos,
        "fractional volume wheel input lost its pending percentage or replaced the stable UIA name with transient feedback");
}

void CheckControlRadioTransitions()
{
    // Drive the production pointer adapter and model while source completions
    // are withheld. No Windows device action or timing assumption is involved.
    const auto click=[](SystemPanelModel& model,const std::string& id) {
        ui::Input input;const auto point=VisibleCenter(model.View(),id);
        const bool pressed=input.Press(model.View(),point);
        const auto action=input.Release(model.View(),point);
        return pressed&&action.kind==ui::InputResult::Kind::Invoke&&action.id==id&&model.Invoke(id);
    };
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);const auto read=source.current;
        std::vector<system_control::Request> requests;std::vector<std::uint64_t> canceled;
        source.current=[read](std::string_view topic) {
            auto snapshot=read(topic);
            if(snapshot&&topic=="network.wifi")
            {
                auto& adapters=snapshot->value.object["interfaces"].array;
                auto other=adapters.front();other.object["id"]=j::Text("wifi-other");other.object["name"]=j::Text("Other Wi-Fi");
                other.object["networks"].array.front().object["id"]=j::Text("network-other");adapters.push_back(std::move(other));
            }
            return snapshot;
        };
        source.start=[&](system_control::Request request){requests.push_back(std::move(request));return static_cast<std::uint64_t>(requests.size());};
        source.cancel=[&](std::uint64_t id){canceled.push_back(id);return true;};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);model.Select("wifi");
        Require(click(model,"wifi.network:network-preview")&&click(model,"wifi.connect:network-preview"),"first adapter could not start a connection transition");
        const auto firstConnection=static_cast<std::uint64_t>(requests.size());
        model.Select("wifi-adapters");Require(model.Invoke("adapter:wifi-other"),"second Wi-Fi adapter cannot be selected");
        Require(click(model,"wifi.network:network-other")&&click(model,"wifi.connect:network-other"),"second adapter could not start its independent transition");
        const auto otherConnection=static_cast<std::uint64_t>(requests.size());
        model.Select("wifi-adapters");Require(model.Invoke("adapter:wifi-preview")&&click(model,"radio:wifi"),"first adapter radio cannot supersede its old transition");
        const auto radioTask=static_cast<std::uint64_t>(requests.size());const auto count=requests.size();
        Require(canceled.size()==2&&std::set<std::uint64_t>(canceled.begin(),canceled.end())==std::set<std::uint64_t>{1,firstConnection}&&
            std::find(canceled.begin(),canceled.end(),otherConnection)==canceled.end(),
            "radio transition failed to cancel its scan/connection or canceled another adapter's task");
        Require(click(model,"wifi.network:network-preview")&&!click(model,"wifi.connect:network-preview")&&
            !click(model,"wifi.scan")&&!click(model,"radio:wifi")&&!model.Invoke("wifi.hidden")&&requests.size()==count,
            "radio transition still accepted dependent pointer input, duplicates or hidden-network requests");
        // Even a late non-canceled failure from a canceled predecessor must
        // lose display ownership; the new radio request remains in progress.
        system_control::Completion late;late.id=firstConnection;late.ok=false;late.error="unavailable";
        state->completions.push_back(late);model.Refresh();
        Require(!model.View().Find("status")&&Node(model.View(),"wifi.network:network-preview").detail!=_LW("controlCenter.failed")&&
            Node(model.View(),"radio:wifi").busy,"superseded connection failure replaced current radio feedback");
        model.Select("wifi-adapters");Require(model.Invoke("adapter:wifi-other"),"radio transition prevented switching adapters");
        Require(Node(model.View(),"radio:wifi").enabled&&click(model,"wifi.network:network-other")&&
            !Node(model.View(),"wifi.connect:network-other").enabled&&requests.size()==count,
            "another adapter lost its own pending state or inherited the first adapter's radio guard");
        late.id=radioTask;late.ok=true;late.error.clear();state->completions.push_back(late);model.Refresh();
    }
    for(const bool multiple:{false,true})
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);const auto read=source.current;
        std::vector<system_control::Request> requests;std::vector<std::uint64_t> canceled;
        source.current=[read,multiple](std::string_view topic) {
            auto snapshot=read(topic);
            if(snapshot&&topic=="bluetooth.devices")
            {
                auto& radios=snapshot->value.object["radios"].array;radios.front().object["id"]=j::Text("radio-a");
                if(multiple){auto other=radios.front();other.object["id"]=j::Text("radio-b");radios.push_back(std::move(other));}
                auto& devices=snapshot->value.object["devices"].array;auto device=devices.front();devices.clear();
                device.object["id"]=j::Text("known-a");device.object["canConnect"]=j::Boolean(true);
                if(multiple)device.object["radioId"]=j::Text("radio-a");devices.push_back(device);
                if(multiple)
                {
                    device.object["id"]=j::Text("known-b");device.object["radioId"]=j::Text("radio-b");devices.push_back(device);
                    device.object.erase("radioId");device.object["id"]=j::Text("unknown-old");devices.push_back(device);
                    device.object["id"]=j::Text("unknown-new");devices.push_back(device);
                }
            }
            return snapshot;
        };
        source.start=[&](system_control::Request request){requests.push_back(std::move(request));return static_cast<std::uint64_t>(requests.size());};
        source.cancel=[&](std::uint64_t id){canceled.push_back(id);return true;};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);model.Select("bluetooth");
        Require(click(model,"bluetooth.device:known-a")&&click(model,"bluetooth.connect:known-a"),"Bluetooth fixture could not start its owned connection");
        if(multiple)Require(click(model,"bluetooth.device:unknown-old")&&click(model,"bluetooth.connect:unknown-old"),"unknown-ownership predecessor could not start");
        Require(click(model,"radio:bluetooth")&&canceled==std::vector<std::uint64_t>{1},"Bluetooth radio canceled a task without confirmed ownership");
        const auto count=requests.size();
        Require((model.View().Find("bluetooth.connect:known-a")||click(model,"bluetooth.device:known-a"))&&
            !click(model,"bluetooth.connect:known-a")&&requests.size()==count,
            "Bluetooth radio transition accepted a dependent connection");
        if(multiple)
        {
            Require(click(model,"bluetooth.device:unknown-new")&&!click(model,"bluetooth.connect:unknown-new")&&requests.size()==count,
                "unknown-ownership Bluetooth device raced a radio transition");
            Require(click(model,"bluetooth.device:known-b")&&click(model,"bluetooth.connect:known-b")&&canceled.size()==1,
                "Bluetooth radio transition disabled another explicitly owned radio's device");
            system_control::Completion failure;failure.id=2;failure.ok=false;failure.error="accessDenied";
            state->completions.push_back(failure);model.Refresh();
            Require(Node(model.View(),"bluetooth.device:unknown-old").detail==_LW("controlCenter.accessDenied"),
                "conservative unknown-ownership guard hid an existing task's real failure");
        }
    }
}

void CheckControlVisibleFeedback()
{
    // A rejected or generically failed slider command must restore its actual
    // value without inserting an error row or leaving an optimistic value.
    for(const bool accepted:{false,true})
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);
        source.start=[accepted](system_control::Request)->std::uint64_t{return accepted?1:0;};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
        const auto slider="audio.output.volume:"+state->outputEndpoint;
        const auto actual=Node(model.View(),slider).value,height=model.View().height;
        Require(model.Invoke(slider,.75f),"generic-failure fixture could not adjust its volume");
        if(accepted)
        {
            system_control::Completion completion;completion.id=1;completion.error="unavailable";
            state->completions.push_back(completion);model.Refresh();
        }
        Require(!model.View().Find("status")&&model.View().height==height&&Node(model.View(),slider).value==actual,
            "generic control failure inserted a banner or retained a value not confirmed by the device");
    }
    auto state=std::make_shared<PreviewState>();state->emptyMedia=true;state->manySection="wifi";
    auto source=FixtureSource(state);std::uint64_t task=0;
    source.start=[&](system_control::Request){return ++task;};
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);model.Select("wifi");model.Refresh(300);
    const auto scan=Node(model.View(),"wifi.scan").bounds;
    Require(model.MaximumScroll()>0&&Node(model.View(),"wifi.scan").busy&&!HasArea(Node(model.View(),"wifi.scan").clip),
        "long Wi-Fi list hid its initial scan feedback inside the scroll viewport");
    model.Scroll(model.MaximumScroll());CheckLayout(model.View());
    Require(Node(model.View(),"wifi.scan").bounds.top==scan.top&&Node(model.View(),"wifi.scan").bounds.bottom==scan.bottom,
        "scrolling a long Wi-Fi list moved its scan action out of view");
    system_control::Completion completion;completion.id=task;completion.ok=false;completion.error="timeout";
    state->completions.push_back(completion);model.Refresh(300);CheckLayout(model.View());
    const auto error=Node(model.View(),"status").bounds;
    Require(Node(model.View(),"status").text==_LW("controlCenter.timeout")&&!HasArea(Node(model.View(),"status").clip)&&error.bottom<=model.ScrollViewport().top,
        "long-list scan failure was placed after the devices or clipped by their scroll viewport");
    model.Scroll(-model.MaximumScroll());
    Require(Node(model.View(),"status").bounds.top==error.top&&Node(model.View(),"status").bounds.bottom==error.bottom,
        "scrolling discarded fixed radio/scan failure feedback");
    Require(model.Invoke("wifi.scan")&&!model.View().Find("status"),"scan retry retained the previous failure");
    completion.id=task;completion.ok=true;completion.error.clear();state->completions.push_back(completion);model.Refresh(300);
    Require(model.Invoke("wifi.network:network-preview"),"long-list failure fixture cannot expand its first network");
    ui::Input input;const auto point=VisibleCenter(model.View(),"wifi.connect:network-preview");
    Require(input.Press(model.View(),point),"long-list connection cannot receive pointer input");
    const auto action=input.Release(model.View(),point);
    Require(action.kind==ui::InputResult::Kind::Invoke&&model.Invoke(action.id),"long-list connection was not dispatched by pointer input");
    const auto offset=model.ScrollOffset(),height=model.View().height;
    completion.id=task;completion.ok=false;completion.error="accessDenied";state->completions.push_back(completion);model.Refresh(300);
    const auto& row=Node(model.View(),"wifi.network:network-preview");
    Require(row.detail==_LW("controlCenter.accessDenied")&&row.tooltip.find(row.detail)!=std::wstring::npos&&
        row.bounds.top>=row.clip.top&&row.bounds.bottom<=row.clip.bottom&&!model.View().Find("status")&&
        model.ScrollOffset()==offset&&model.View().height==height,
        "a first-row connection failure disappeared below a long list or moved its viewport");
    // Independent negative geometry: the old footer position is provably off
    // screen in this same fixture; checking only text presence would miss it.
    const auto& footer=Node(model.View(),"wifi.hidden");
    Require(footer.bounds.top>=footer.clip.bottom,"long-list feedback oracle has no offscreen footer counterexample");
}

void CheckControlUnavailableStates()
{
    for(int mode=0;mode<5;++mode)
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);const auto read=source.current;
        source.current=[read,mode](std::string_view topic)->std::optional<system_control::Snapshot> {
            auto snapshot=read(topic);if(topic!="network.wifi")return snapshot;
            if(mode==0)return std::nullopt;
            if(mode==1){snapshot->available=false;snapshot->error="unavailable";return snapshot;}
            auto& adapters=snapshot->value.object["interfaces"].array;
            if(mode==2)adapters.clear();
            else {auto& adapter=adapters.front();adapter.object["networks"].array.clear();adapter.object["enabled"]=j::Boolean(false);
                if(mode==4){adapter.object["available"]=j::Boolean(false);adapter.object["error"]=j::Text("accessDenied");}}
            return snapshot;
        };
        unsigned starts=0;source.start=[&](system_control::Request){++starts;return std::uint64_t{1};};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
        Require(bool(model.View().Find("wifi.more"))==(mode==3||mode==4),
            "missing Wi-Fi kept an empty detail entry, or permission recovery lost its entry");
        if(mode<3)Require(!Node(model.View(),"radio:wifi").Interactive()&&!model.Invoke("radio:wifi"),
            "unavailable Wi-Fi card remained an actionable switch");
        model.Select("wifi");
        const auto& radio=Node(model.View(),"radio:wifi");
        Require(starts==0&&!Node(model.View(),"wifi.scan").enabled&&radio.enabled==(mode==3),
            "unknown/missing/off Wi-Fi state scanned or advertised a false radio capability");
        if(mode==4)Require(model.View().Find("wifi.denied")&&!model.View().Find("wifi.empty")&&
            radio.tooltip.find(_LW("controlCenter.off"))==std::wstring::npos,"permission failure was rendered as a switched-off radio");
        else
        {
            const auto expected=_LW(mode<2?"controlCenter.unavailable":mode==2?"controlCenter.noHardware":"controlCenter.off");
            Require(Node(model.View(),"wifi.empty").text==expected&&radio.tooltip.find(expected)!=std::wstring::npos,
                "Wi-Fi detail conflated unknown sampling, missing hardware and a known off radio");
        }
    }
    // A desktop PC without brightness/battery support must not advertise fake
    // adjustment controls. Win10 has no native Quick Settings destination.
    auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
    auto source=FixtureSource(state);const auto read=source.current;
    source.nativeControls={};bool supported=false;
    source.current=[&](std::string_view topic) {
        auto snapshot=read(topic);if(!snapshot||supported)return snapshot;
        if(topic=="system.power.plans")snapshot->value.object["batteryPresent"]=j::Boolean(false);
        if(topic=="system.display.brightness")for(auto& monitor:snapshot->value.object["monitors"].array)
            monitor.object["available"]=j::Boolean(false);
        if(topic=="audio.output.volume")snapshot->available=false;
        return snapshot;
    };
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
    Require(!model.View().Find("battery")&&!model.View().Find("system.settings")&&
        std::none_of(model.View().nodes.begin(),model.View().nodes.end(),[](const auto& n){return n.id.starts_with("brightness.");})&&
        model.View().Find("audio.output.unavailable")&&
        std::none_of(model.View().nodes.begin(),model.View().nodes.end(),[](const auto& n){return n.role==ui::Role::Slider;}),
        "unsupported desktop controls retained a brightness row, bogus battery, sliders or native-panel action");
    const auto& power=Node(model.View(),"power.more");
    Require(power.bounds.right==model.View().width-16,
        "Win10 footer reserved an empty native-panel slot");
    supported=true;model.Refresh();
    Require(model.View().Find("battery")&&model.View().Find("brightness.more")&&
        !model.View().Find("brightness.unavailable")&&!model.View().Find("audio.output.unavailable"),
        "recovering hardware did not restore real controls");
}

void CheckControlPowerSections()
{
    auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
    auto source=FixtureSource(state);const auto read=source.current;
    bool multiple=false,selectedOther=false,modes=true;unsigned starts=0;
    source.current=[&](std::string_view topic) {
        auto snapshot=read(topic);
        if(snapshot&&topic=="system.power.plans")
        {
            auto& value=snapshot->value;value.object["modeSupported"]=j::Boolean(modes);
            auto& plans=value.object["plans"].array;plans.front().object["active"]=j::Boolean(!selectedOther);
            if(multiple){auto other=plans.front();other.object["id"]=j::Text("power-other");other.object["name"]=j::Text("Other power plan");
                other.object["active"]=j::Boolean(selectedOther);plans.push_back(std::move(other));}
        }
        return snapshot;
    };
    source.start=[&](system_control::Request request) {
        Require(request.name=="system.power.setPlan"&&request.arguments.at("planId")=="power-other",
            "power plan selection was dispatched as a similarly named power mode");
        selectedOther=true;++starts;system_control::Completion done;done.id=starts;done.ok=true;state->completions.push_back(done);
        return static_cast<std::uint64_t>(starts);
    };
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);model.Select("power");CheckLayout(model.View());
    Require(!model.View().Find("power.plans.heading")&&!model.View().Find("power.plan:power-plan-preview")&&
        Node(model.View(),"power.modes.heading").text==_LW("controlCenter.powerMode")&&Node(model.View(),"power.mode:balanced").selected,
        "single-plan power page retained its redundant plan row or omitted the mode group heading");
    multiple=true;model.Refresh();CheckLayout(model.View());
    Require(Node(model.View(),"power.plans.heading").text==_LW("controlCenter.powerPlan")&&
        Node(model.View(),"power.plans.heading").bounds.bottom<Node(model.View(),"power.plan:power-plan-preview").bounds.top&&
        Node(model.View(),"power.plan:power-other").bounds.bottom<Node(model.View(),"power.modes.heading").bounds.top&&
        Node(model.View(),"power.modes.heading").bounds.bottom<Node(model.View(),"power.mode:efficiency").bounds.top,
        "multiple power plans and power modes were mixed into an unlabeled selection list");
    ui::Input input;const auto point=VisibleCenter(model.View(),"power.plan:power-other");
    Require(input.Press(model.View(),point),"second power plan cannot receive pointer input");
    const auto action=input.Release(model.View(),point);
    Require(action.kind==ui::InputResult::Kind::Invoke&&model.Invoke(action.id)&&starts==1&&
        Node(model.View(),"power.plan:power-other").selected&&Node(model.View(),"power.mode:balanced").selected,
        "selecting a power plan failed to update its own group or changed the independent power mode");
    modes=false;model.Refresh();
    Require(!model.View().Find("power.modes.heading")&&!model.View().Find("power.mode:balanced")&&model.View().Find("power.plans.heading"),
        "unsupported power modes left an empty heading or removed valid plan choices");
}

void CheckPendingActions()
{
    // Delay the production task boundary rather than sleeping. The model must
    // immediately acknowledge input while every reported device state is old.
    for(const auto* page:{"wifi","bluetooth"})
    {
        const bool wifi=std::string_view(page)=="wifi";
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);const auto read=source.current;
        bool connected=false,present=true,radioOn=true;std::uint64_t nextId=100;
        struct Started {std::uint64_t id;std::string name;};std::vector<Started> started;
        source.current=[&](std::string_view topic) {
            auto snapshot=read(topic);
            if(snapshot&&topic==(wifi?"network.wifi":"bluetooth.devices"))
            {
                snapshot->value.object[wifi?"interfaces":"radios"].array.front().object["enabled"]=j::Boolean(radioOn);
                auto& entries=wifi?snapshot->value.object["interfaces"].array.front().object["networks"].array:
                    snapshot->value.object["devices"].array;
                if(!present)entries.clear();else entries.front().object["connected"]=j::Boolean(connected);
            }
            return snapshot;
        };
        source.start=[&](system_control::Request request) {const auto id=++nextId;started.push_back({id,request.name});return id;};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);model.Select(page);
        const auto complete=[&](std::uint64_t id,bool ok) {
            system_control::Completion completion;completion.id=id;completion.ok=ok;completion.error=ok?"":"unavailable";
            state->completions.push_back(std::move(completion));model.Refresh();
        };
        if(wifi&&!started.empty())
        {
            const auto scans=started.size();
            Require(Node(model.View(),"title").detail.empty()&&Node(model.View(),"wifi.scan").text.empty()&&Node(model.View(),"wifi.scan").busy&&
                Node(model.View(),"wifi.scan").tooltip.find(_LW("controlCenter.working"))!=std::wstring::npos&&!Node(model.View(),"wifi.scan").enabled&&
                !model.Invoke("wifi.scan")&&started.size()==scans,"page-entry scan lacked feedback or allowed a duplicate scan");
            complete(started.back().id,true);
        }
        const auto suffix=wifi?"network-preview":"bluetooth-device-preview";
        const auto row=std::string(page)+(wifi?".network:":".device:")+suffix;
        const auto command=std::string(page)+".connect:"+suffix;
        const auto card=std::string(page)+".card:"+suffix;
        const auto expand=[&] {if(!model.View().Find(command))Require(model.Invoke(row),"pending fixture could not expand its device");};
        expand();const auto height=model.View().height;const auto initialDetail=Node(model.View(),row).detail;
        Require(!Node(model.View(),card).outlined,"mouse-expanded device card retained its accent outline");
        const auto count=started.size();
        Require(model.Invoke(command)&&started.size()==count+1,"connect button did not start exactly one device task");
        const auto task=started.back().id;
        Require(!Node(model.View(),command).enabled&&Node(model.View(),command).text==_LW("controlCenter.working")&&
            Node(model.View(),row).detail.find(_LW("controlCenter.working"))==std::wstring::npos&&Node(model.View(),"title").detail.empty()&&
            model.View().height==height&&!model.View().Find("status.pending"),
            "connect input did not show immediate in-place feedback or shifted the whole device list");
        Require(!model.Invoke(command)&&started.size()==count+1,"repeated clicks dispatched duplicate connection tasks");
        model.Refresh();Require(!Node(model.View(),command).enabled,"ordinary refresh cleared the in-flight device guard");
        Require(model.Invoke(row)&&!model.View().Find(command)&&Node(model.View(),row).detail==_LW("controlCenter.working"),
            "collapsing an in-flight device hid all pending feedback");
        expand();Require(Node(model.View(),command).text==_LW("controlCenter.working")&&
            Node(model.View(),row).detail.find(_LW("controlCenter.working"))==std::wstring::npos,
            "re-expanded device duplicated pending text instead of restoring its button feedback");
        complete(task,false);
        Require(Node(model.View(),command).enabled&&Node(model.View(),command).text==_LW("controlCenter.connect")&&
            Node(model.View(),row).detail==initialDetail&&!model.View().Find("status"),
            "generic connection failure replaced the true device state with an error message");
        Require(model.Invoke(command),"failed connection could not be retried");const auto successful=started.back().id;
        connected=true;complete(successful,true);
        Require(Node(model.View(),command).enabled&&Node(model.View(),command).text==_LW("controlCenter.disconnect")&&
            !model.View().Find("status"),"successful connection did not use the latest device readback");
        Require(model.Invoke(command),"disconnect fixture could not start");const auto stale=started.back().id;
        model.Select("audio");Require(!model.View().Find("status.pending"),"another page inherited unrelated pending feedback");
        const auto beforeReturn=started.size();model.Select(page);expand();
        Require(started.size()==beforeReturn,"returning to an adapter with an in-flight connection queued an automatic scan");
        const auto returningCount=started.size();
        Require(!Node(model.View(),command).enabled&&!model.Invoke(command)&&started.size()==returningCount,
            "leaving and returning to the page bypassed the in-flight device guard");
        present=false;model.Refresh();Require(!model.View().Find(row),"removed pending device remained visible");
        complete(stale,false);Require(!model.View().Find("status")&&!model.View().Find("status.pending"),
            "a removed device's delayed failure leaked into the current panel");
        present=true;model.Refresh();expand();
        for(const auto& scanTask:started)if(scanTask.name=="network.wifi.scan")complete(scanTask.id,true);
        const auto radio=std::string("radio:")+page;const auto actualRadio=Node(model.View(),radio).selected;
        const auto radioHeight=model.View().height;
        Require(model.Invoke(radio)&&Node(model.View(),"title").detail.empty()&&Node(model.View(),radio).busy&&
            Node(model.View(),radio).selected==actualRadio&&!Node(model.View(),radio).enabled&&model.View().height==radioHeight,
            "compact switch lacked immediate visible feedback, shifted layout or optimistically changed device state");
        const auto radioTask=started.back().id;const auto radioCount=started.size();
        Require(!model.Invoke(radio)&&started.size()==radioCount,"compact switch dispatched a duplicate radio task");
        radioOn=false;complete(radioTask,true);
        Require(Node(model.View(),radio).enabled&&!Node(model.View(),radio).selected&&!Node(model.View(),radio).busy&&Node(model.View(),"title").detail.empty(),
            "completed compact switch retained pending feedback or failed to show actual radio readback");
        radioOn=true;model.Refresh(); // A later external re-enable makes connection available again.
        Require(model.Invoke(command),"reappeared device could not start a new task");
        const auto afterClose=started.back().id;model.Close();const auto reads=state->reads;
        complete(afterClose,false);
        Require(state->reads==reads&&!model.Invoke(command)&&state->closes==1,
            "a completion after close revived the panel or dispatched a stale action");
    }
    CheckControlRadioTransitions();
    CheckControlVisibleFeedback();
    CheckControlUnavailableStates();
    CheckControlPowerSections();
}

void CheckCalendarNames(const ui::Scene& scene)
{
    const std::string september="date:2026-09-01",october="date:2026-10-01";
    Require(Node(scene,september).text==Node(scene,october).text,"calendar name fixture does not share a displayed day number");
    ui::Input input;input.Sync(scene);std::vector<wr::ViewAccessibilityNode> nodes;std::string error;
    Require(wr::CollectInteractionAccessibilityNodes(input.AccessibilityRegions(),scene.width,scene.height,{},nodes,error),
        "calendar semantic nodes could not be collected");
    const auto name=[&](const std::string& id) {
        const auto found=std::find_if(nodes.begin(),nodes.end(),[&](const auto& n){return input.Identity(n.key)==id;});
        Require(found!=nodes.end(),"calendar date is missing from UIA");return found->name;
    };
    Require(name(september).starts_with("2026-09-01")&&name(october).starts_with("2026-10-01")&&
        Node(scene,september).tooltip==Node(scene,september).accessibilityLabel,
        "same-number calendar days lack distinct full year/month/date accessibility names");
}

void CheckTimePickerInput()
{
    auto source=FixtureSource(std::make_shared<PreviewState>());int saves=0;
    source.calendar.mutations.save=[&](const auto&){++saves;return calendar::MutationResult{};};
    SystemPanelModel model(std::move(source),{},StatusBarAction::Calendar);model.Refresh(800,720);
    ui::Input input;
    const auto click=[&](std::string_view id){
        model.Reveal(id);const auto point=VisibleCenter(model.View(),id);
        Require(input.Press(model.View(),point),"time picker rejected a visible pointer target");
        const auto result=input.Release(model.View(),point);
        Require(result.kind==ui::InputResult::Kind::Invoke&&result.id==id&&model.Invoke(result.id),
            "time picker pointer action did not reach the production model");input.Sync(model.View());
        const auto focus=model.CalendarFocusTarget();if(!focus.empty())input.Focus(focus,false);
    };
    const auto key=[&](unsigned value,bool shift=false){
        const auto result=model.HandleKey(input,value,shift);
        if(result.kind==ui::InputResult::Kind::Invoke)Require(model.Invoke(result.id),"time picker keyboard activation was lost");
        input.Sync(model.View());
    };
    Require(model.Invoke("calendar.add")&&model.SetCalendarInput("calendar.edit.title",L"Time draft"),"time picker draft setup failed");
    click("calendar.edit.date");
    Require(!model.View().Find("scrollbar")&&
        Node(model.View(),"picker.confirm").bounds.bottom<=model.View().height,
        "calendar date picker did not fit its actions in the panel");
    click("picker.cancel");
    click("calendar.edit.start");
    const auto hour=Node(model.View(),"picker.hour:current").bounds,minute=Node(model.View(),"picker.minute:current").bounds;
    Require(hour.right<minute.left&&hour.top==minute.top&&model.View().height<440&&
        !model.View().Find("scrollbar")&&
        Node(model.View(),"picker.confirm").bounds.bottom<=model.View().height&&
        !model.View().Find("picker.hours")&&!model.View().Find("picker.minutes")&&model.CalendarInputFields().empty(),
        "shared time picker is not a compact simultaneous two-column control");
    click("picker.hour:11");
    Require(Node(model.View(),"picker.time").text==L"11:00"&&model.CalendarFocusTarget()=="picker.hour:current",
        "hour choice changed the minute or jumped to another page");
    Require(input.Focus(model.CalendarFocusTarget(),true),"time value lost its stable focus target");
    key(VK_RIGHT);Require(input.Focused()=="picker.minute:current","Right did not focus the minute value");
    key(VK_DOWN);Require(Node(model.View(),"picker.time").text==L"11:01"&&input.Focused()=="picker.minute:current",
        "Down failed to update the focused minute without losing focus");
    const auto minutePoint=VisibleCenter(model.View(),"picker.minute:current");
    for(int i=0;i<3;++i)model.Wheel(minutePoint,-.25f);
    Require(Node(model.View(),"picker.time").text==L"11:01","fractional wheel deltas rounded up before a complete step");
    model.Wheel(minutePoint,-.25f);
    Require(Node(model.View(),"picker.time").text==L"11:02","minute wheel did not accumulate precise input");
    model.Wheel(VisibleCenter(model.View(),"picker.minute:label"),1);
    const auto minuteColumn=Node(model.View(),"picker.minute:column").bounds;
    model.Wheel({minuteColumn.left+1,minuteColumn.top+1},-1);
    Require(Node(model.View(),"picker.time").text==L"11:02","time label/card gaps failed to route wheel input to their own column");
    key(VK_END);Require(Node(model.View(),"picker.time").text==L"11:59","End did not reach the last minute");
    const auto before=model.ScrollOffset();key(VK_DOWN);model.Wheel(minutePoint,-1);
    Require(Node(model.View(),"picker.time").text==L"11:59"&&model.ScrollOffset()==before,
        "minute boundary carried into hours or scrolled the enclosing panel");
    key(VK_HOME);key(VK_LEFT);key(VK_HOME);key(VK_PRIOR);
    Require(Node(model.View(),"picker.time").text==L"00:00"&&input.Focused()=="picker.hour:current","hour lower boundary or Left focus failed");
    key(VK_END);key(VK_NEXT);
    Require(Node(model.View(),"picker.time").text==L"23:00","hour upper boundary escaped the day");
    key(VK_TAB);Require(input.Focused()=="picker.minute:current","Tab visited every hour instead of the minute column");
    key(VK_TAB);Require(input.Focused()=="picker.cancel","Tab could not leave the time columns");
    key(VK_TAB,true);Require(input.Focused()=="picker.minute:current","Shift+Tab did not return to the minute column");
    click("picker.cancel");
    Require(Node(model.View(),"calendar.edit.start").text==L"09:00"&&model.CalendarFocusTarget()=="calendar.edit.start"&&saves==0,
        "canceling a time draft committed its value or lost the originating field");
    click("calendar.edit.start");click("picker.hour:10");
    model.Wheel(VisibleCenter(model.View(),"picker.minute:current"),-15);
    click("picker.confirm");
    Require(Node(model.View(),"calendar.edit.start").text==L"10:15"&&Node(model.View(),"calendar.edit.end").text==L"10:00"&&saves==0,
        "time confirmation changed another field or persisted the calendar draft");
    const auto fields=model.CalendarInputFields();
    Require(std::any_of(fields.begin(),fields.end(),[](const auto& field){return field.id=="calendar.edit.title"&&field.text==L"Time draft";}),
        "time editing lost the native text draft");
    click("calendar.edit.start");key(VK_UP);Require(model.CalendarBack()&&Node(model.View(),"calendar.edit.start").text==L"10:15",
        "Escape committed the unconfirmed time draft");
    click("calendar.edit.start");model.Select("audio");
    Require(!model.CalendarEditing()&&!model.Invoke("picker.confirm")&&saves==0,"page navigation kept a stale time picker alive");
    model.Select("");Require(model.Invoke("calendar.add")&&model.Invoke("calendar.edit.start"),"closing time fixture did not reopen");
    model.Close();Require(!model.Invoke("picker.confirm")&&!model.CalendarEditing()&&saves==0,"closed time picker retained a commit path");
}

void CheckCalendarManagement()
{
    const auto state=std::make_shared<PreviewState>();auto source=FixtureSource(state);
    std::vector<calendar::CalendarEvent> events;int saves=0,deletes=0;bool failSave=false,closeOnSave=false;
    SystemPanelModel* active=nullptr;
    source.calendar.events=[&](const std::string& date){std::vector<calendar::CalendarEvent> result;for(const auto& event:events)if(event.date==date)result.push_back(event);return result;};
    source.calendar.mutations.current=[&](const calendar::CalendarEvent& event)->std::optional<calendar::CalendarEvent>{
        for(const auto& stored:events)if(stored.id==event.id)return stored;return {};
    };
    source.calendar.mutations.save=[&](const calendar::CalendarEvent& draft){
        ++saves;if(closeOnSave){active->Close();return calendar::MutationResult{true,"closed",1,{}};}
        if(failSave)return calendar::MutationResult{false,{},0,"save_failed"};
        if(draft.title.empty())return calendar::MutationResult{false,{},0,"title_required"};
        auto next=draft;
        if(draft.id.empty()){next.id="direct-event";next.revision=1;events.push_back(next);}
        else
        {
            const auto found=std::find_if(events.begin(),events.end(),[&](const auto& e){return e.id==draft.id;});
            if(found==events.end()||found->revision!=draft.revision)return calendar::MutationResult{false,{},0,"conflict"};
            ++next.revision;*found=next;
        }
        return calendar::MutationResult{true,next.id,next.revision,{}};
    };
    source.calendar.mutations.remove=[&](const std::string& id){
        ++deletes;std::erase_if(events,[&](const auto& event){return event.id==id;});
        return calendar::MutationResult{true,id,0,{}};
    };
    SystemPanelModel model(std::move(source),{},StatusBarAction::Calendar);active=&model;model.Refresh(800,720);
    const float calendarHeight=model.View().height;const auto monthBounds=Node(model.View(),"calendar.month").bounds;
    const auto field=[&](std::string_view id){
        const auto fields=model.CalendarInputFields();const auto found=std::find_if(fields.begin(),fields.end(),[&](const auto& item){return item.id==id;});
        Require(found!=fields.end(),"calendar embedded input descriptor is missing");return *found;
    };
    Require(model.Invoke("calendar.add")&&model.CalendarEditing()&&model.View().width==720&&
        model.CalendarInputFields().size()==2&&Node(model.View(),"calendar.edit.date").text==L"2026-09-26"&&
        Node(model.View(),"calendar.month").bounds.left==monthBounds.left&&Node(model.View(),"calendar.month").bounds.top==monthBounds.top&&
        model.View().height>=520.f&&model.View().height<=560.f&&model.View().height>calendarHeight&&
        field("calendar.edit.title").bounds.left>=model.ScrollViewport().left,
        "calendar add did not expand its form while retaining the fixed month column");
    Require(model.SelectCalendarChoice("calendar.edit.mode",3)&&model.View().height<=560.f&&
        model.MaximumScroll()>0&&model.Reveal("calendar.edit.save")&&
        Node(model.View(),"calendar.edit.save").bounds.bottom<=model.ScrollViewport().bottom&&
        model.SelectCalendarChoice("calendar.edit.mode",0),
        "monthly editor escaped its height cap or made save unreachable");
    Require(model.SetCalendarInput("calendar.edit.title",L"Unsaved draft")&&model.Invoke("calendar.next")&&
        field("calendar.edit.title").text==L"Unsaved draft"&&Node(model.View(),"calendar.edit.date").text==L"2026-09-26"&&model.Invoke("calendar.previous"),
        "browsing the retained month discarded or changed the unsaved event draft");
    model.Reveal("calendar.edit.notes");
    const auto notes=field("calendar.edit.notes");const auto editorOffset=model.ScrollOffset();
    Require(notes.bounds.top>=notes.clip.top&&notes.bounds.bottom<=notes.clip.bottom&&notes.clip.left==model.ScrollViewport().left,
        "native calendar input did not receive the translated agenda clip");
    model.Wheel(VisibleCenter(model.View(),"date:2026-09-26"),-1);
    ui::Input calendarInput;calendarInput.Sync(model.View());Require(calendarInput.Focus("date:2026-09-26",true),"retained month lost keyboard focus");
    model.HandleKey(calendarInput,VK_NEXT,false);model.Reveal("date:2026-09-26");
    Require(model.ScrollOffset()==editorOffset&&Node(model.View(),"calendar.month").bounds.top==monthBounds.top,
        "fixed month wheel, keyboard or focus changed the agenda scroll");
    Require(calendarInput.Focus("calendar.edit.notes",true),"native notes lost their semantic focus target");
    for(int step=0;step<32&&calendarInput.Focused()!="calendar.edit.save";++step)
        model.HandleKey(calendarInput,VK_TAB,false);
    Require(calendarInput.Focused()=="calendar.edit.save"&&Node(model.View(),"calendar.edit.save").bounds.bottom<=model.ScrollViewport().bottom,
        "Tab could not reveal the save action below the scrollable native form");
    Require(model.Invoke("calendar.edit.date")&&
        model.CalendarInputFields().empty()&&model.CalendarEditing()&&model.View().Find("picker.day:2026-09-26")&&
        model.View().Find("calendar.month")&&model.View().height>=calendarHeight&&
        model.View().height<=520.f&&!model.View().Find("scrollbar")&&
        Node(model.View(),"picker.confirm").bounds.bottom<=model.View().height&&
        Node(model.View(),"picker.month").bounds.left>=model.ScrollViewport().left&&!model.Invoke("calendar.edit.save"),
        "date selection displaced the month, clipped its actions or exposed an unrelated save action");
    Require(model.Invoke("picker.day:2026-09-28")&&model.CalendarBack()&&
        Node(model.View(),"calendar.edit.date").text==L"2026-09-26"&&field("calendar.edit.title").text==L"Unsaved draft"&&
        model.CalendarFocusTarget()=="calendar.edit.date","canceling a date choice lost text or changed the form value");
    Require(model.Invoke("calendar.edit.start")&&model.Invoke("picker.hour:11"),"shared time control did not expose its hour column");
    model.Wheel(VisibleCenter(model.View(),"picker.minute:current"),-30.f);
    Require(
        model.Invoke("picker.confirm")&&Node(model.View(),"calendar.edit.start").text==L"11:30"&&
        model.CalendarFocusTarget()=="calendar.edit.start","shared time control did not return its confirmed value and focus");
    model.Refresh(800,720);
    Require(model.Invoke("calendar.edit.save")&&saves==0&&field("calendar.edit.title").text==L"Unsaved draft"&&
        model.CalendarEditing()&&model.View().Find("calendar.edit.error"),"reversed time range was lost or reached persistence");
    Require(model.CalendarBack()&&!model.CalendarEditing()&&events.empty()&&
        Node(model.View(),"calendar.selected").text==L"2026-09-26","back changed the calendar selection or saved a discarded draft");

    Require(model.Invoke("calendar.add")&&model.SetCalendarInput("calendar.edit.title",L"Direct event"),"calendar create setup failed");
    Require(model.Invoke("calendar.edit.allDay")&&!Node(model.View(),"calendar.edit.start").enabled&&
        !model.Invoke("calendar.edit.start")&&!model.SetCalendarInput("calendar.edit.start",L"broken"),"all-day disabled times retained editable input");
    Require(model.Invoke("calendar.edit.allDay")&&Node(model.View(),"calendar.edit.start").text==L"09:00","all-day toggle discarded the time draft");
    Require(model.CalendarChoices("calendar.edit.reminder").size()==7&&
        model.SelectCalendarChoice("calendar.edit.reminder",3)&&
        model.CalendarChoices("calendar.edit.reminder")[3].selected&&
        Node(model.View(),"calendar.edit.reminder").text==_LW("settings.calendar.reminder.15"),
        "same-page reminder dropdown lost its value");
    Require(model.Invoke("calendar.edit.save")&&!model.CalendarEditing()&&events.size()==1&&events.front().reminderMinutes==15&&
        model.View().Find("event:2026-09-26:direct-event"),"calendar save did not refresh the existing agenda");

    const std::string originalNode="event:2026-09-26:direct-event";
    Require(!model.CalendarEventCommand("event:2026-09-26:removed",false)&&!model.CalendarEditing()&&
        Node(model.View(),"calendar.notice").text==_LW("settings.calendar.conflict"),"stale context identity opened another event or failed silently");
    const auto bounds=Node(model.View(),originalNode).bounds;ui::Input context;const D2D1_POINT_2F point{bounds.left+12,bounds.top+12};
    Require(context.Press(model.View(),point,true),"calendar event rejected a context press");
    const auto contextResult=context.Release(model.View(),point,true);
    Require(contextResult.kind==ui::InputResult::Kind::Context&&contextResult.id==originalNode&&
        model.CalendarEventCommand(contextResult.id,false),"right-click context lost its stable calendar event identity");
    Require(model.View().height>=520.f&&model.View().height<=560.f,"calendar edit did not respect its expanded height range");
    Require(model.SetCalendarInput("calendar.edit.title",L"Changed here")&&model.Invoke("calendar.edit.date")&&
        model.Invoke("picker.day:2026-09-28")&&model.Invoke("picker.confirm")&&
        model.SetCalendarInput("calendar.edit.notes",L"Keep this draft"),"calendar edit did not accept its existing event draft");
    failSave=true;
    Require(model.Invoke("calendar.edit.save")&&model.CalendarEditing()&&field("calendar.edit.notes").text==L"Keep this draft"&&
        events.front().date=="2026-09-26","failed persistence discarded the draft or modified stored data");
    failSave=false;
    Require(model.Invoke("calendar.edit.save")&&!model.CalendarEditing()&&events.front().revision==2&&
        Node(model.View(),"calendar.selected").text==L"2026-09-28"&&Node(model.View(),"event:2026-09-28:direct-event").text==L"Changed here",
        "editing in the popup did not follow the saved event date");

    const std::string changedNode="event:2026-09-28:direct-event";
    ++events.front().revision;
    Require(!model.CalendarEventCommand(changedNode,false)&&!model.CalendarEditing()&&
        Node(model.View(),"calendar.notice").text==_LW("settings.calendar.conflict"),"context command silently adopted an event changed while its menu was open");
    model.Refresh(800,720);
    Require(model.CalendarEventCommand(changedNode,true)&&model.View().Find("calendar.edit.confirmDelete")&&deletes==0,
        "right-click delete skipped its in-panel confirmation");
    Require(model.CalendarInputFields().empty()&&!model.View().Find("calendar.edit.back")&&!model.View().Find("header.settings"),
        "context delete confirmation exposed editor fields or an unrelated parent route");
    Require(model.View().Find("calendar.month")&&model.View().height==calendarHeight&&
        Node(model.View(),"calendar.delete.target").bounds.left>=model.ScrollViewport().left,
        "context delete confirmation replaced the month or escaped the agenda column");
    ui::Input cancelDelete;const auto cancelPoint=VisibleCenter(model.View(),"calendar.edit.cancelDelete");
    Require(!model.SetCalendarInput("calendar.edit.title",L"unexpected")&&cancelDelete.Press(model.View(),cancelPoint)&&
        model.Invoke(cancelDelete.Release(model.View(),cancelPoint).id)&&!model.CalendarEditing()&&model.View().Find(changedNode)&&
        model.CalendarFocusTarget()==changedNode&&deletes==0,
        "canceling context delete did not return to its original agenda");
    Require(model.CalendarEventCommand(changedNode,true)&&model.CalendarBack()&&!model.CalendarEditing()&&model.View().Find(changedNode),
        "Escape from context delete opened the editor instead of restoring the agenda");
    Require(model.CalendarEventCommand(changedNode,false)&&model.SetCalendarInput("calendar.edit.notes",L"Conflict draft"),"calendar conflict draft setup failed");
    ++events.front().revision;
    Require(model.Invoke("calendar.edit.save")&&model.CalendarEditing()&&field("calendar.edit.notes").text==L"Conflict draft"&&
        Node(model.View(),"calendar.edit.error").text==_LW("settings.calendar.conflict"),"revision conflict lost the user's draft");
    Require(model.Invoke("calendar.edit.delete")&&model.Invoke("calendar.edit.confirmDelete")&&deletes==0&&model.CalendarEditing(),
        "delete removed an event that changed after it was opened");
    Require(model.CalendarBack()&&field("calendar.edit.notes").text==L"Conflict draft"&&model.CalendarBack()&&!model.CalendarEditing(),
        "delete cancellation discarded the draft or failed to return to the calendar");
    Require(model.CalendarEventCommand(changedNode,true)&&model.Invoke("calendar.edit.confirmDelete")&&deletes==1&&events.empty()&&
        model.View().Find("calendar.empty")&&!model.CalendarEditing(),"confirmed delete failed to return to the agenda empty state");

    Require(model.Invoke("calendar.add")&&model.SetCalendarInput("calendar.edit.title",L"Closing"),"calendar close fixture setup failed");
    closeOnSave=true;
    Require(!model.Invoke("calendar.edit.save")&&!model.CalendarEditing()&&state->closes==1&&
        !model.SetCalendarInput("calendar.edit.title",L"late")&&!model.CalendarBack(),"late save completion revived a closed calendar page");

    auto revokedSource=FixtureSource(std::make_shared<PreviewState>());calendar::CalendarEvent existing;
    existing.id="revoked";existing.date="2026-09-26";existing.title="Revoked";existing.revision=1;
    revokedSource.calendar.events=[existing](const std::string& date){return date==existing.date?std::vector{existing}:std::vector<calendar::CalendarEvent>{};};
    revokedSource.calendar.mutations.save=[](const auto&)->calendar::MutationResult{throw std::runtime_error("revoked context wrote data");};
    revokedSource.calendar.mutations.current=[&](const auto& event){active->Close();return std::optional(event);};
    SystemPanelModel revoked(std::move(revokedSource),{},StatusBarAction::Calendar);active=&revoked;
    Require(!revoked.CalendarEventCommand("event:2026-09-26:revoked",true)&&!revoked.CalendarEditing(),"context lookup revived a synchronously closed calendar");

    auto scrolling=FixtureSource(std::make_shared<PreviewState>());
    scrolling.calendar.mutations.save=[](const auto&){return calendar::MutationResult{};};
    SystemPanelModel narrow(std::move(scrolling),{},StatusBarAction::Calendar);
    for(const float width:{720.f,420.f,260.f})
    {
        narrow.Refresh(240,width);Require(narrow.Invoke("calendar.add"),"constrained editor did not open");CheckLayout(narrow.View());
        for(const auto& input:narrow.CalendarInputFields())
            Require(input.bounds.left>=0&&input.bounds.right<=narrow.View().width&&!input.label.empty(),"calendar input escaped its monitor width or lost its accessible label");
        Require(narrow.MaximumScroll()>0&&narrow.Reveal("calendar.edit.save"),"short editor lost scroll access to saving");
        const auto& save=Node(narrow.View(),"calendar.edit.save");
        Require(save.bounds.top>=save.clip.top&&save.bounds.bottom<=save.clip.bottom,"calendar save could not be revealed");
        Require(narrow.Invoke("calendar.edit.start"),"constrained shared time picker did not open");
        CheckLayout(narrow.View());ui::Input timeInput;timeInput.Sync(narrow.View());
        Require(timeInput.Focus("picker.minute:current",true),"short time picker lost logical minute focus");
        narrow.HandleKey(timeInput,VK_END,false);
        const auto& minute=Node(narrow.View(),"picker.minute:current");
        Require(minute.text==L"59"&&minute.bounds.top>=minute.clip.top&&minute.bounds.bottom<=minute.clip.bottom&&narrow.CalendarBack(),
            "keyboard could not reach the last minute inside a short viewport");
        Require(narrow.CalendarBack(),"short editor lost its back route");
    }
    const auto leapDays=ui::DateTimePicker::MonthDates("2024-02-01");
    Require(std::find(leapDays.begin(),leapDays.end(),std::optional<std::string>("2024-02-29"))!=leapDays.end(),
        "shared calendar selection lost a leap day");
    ui::DateTimePicker first(std::string("0001-01-01")),last(std::string("9999-12-31"));
    Require(first.Invoke("picker.previous",{})==ui::DateTimePicker::Result::None&&
        last.Invoke("picker.next",{})==ui::DateTimePicker::Result::None,"shared calendar selection escaped the service date range");
}
void CheckCalendarSeriesManagement()
{
    const auto path=std::filesystem::temp_directory_path()/
        (L"SnowDesktopCalendarPanelSeries-"+std::to_wstring(GetCurrentProcessId()))/
        L"SnowDesktop.calendar.json";
    std::error_code error;std::filesystem::remove_all(path.parent_path(),error);
    calendar::CalendarService service(path);
    Require(service.Load(),"series panel fixture could not load its calendar store");
    auto source=FixtureSource(std::make_shared<PreviewState>());
    source.calendar.events=[&](const std::string& date){return service.Events(date,date);};
    source.calendar.mutations.save=[&](const calendar::CalendarEvent& event){
        return event.id.empty()?service.Create(event):service.Update(event.id,event.revision,event);};
    source.calendar.mutations.current=[&](const calendar::CalendarEvent& event){return service.EventById(event.id);};
    source.calendar.mutations.remove=[&](const std::string& id){return service.Remove(id);};
    source.calendar.mutations.seriesById=[&](const std::string& id){return service.SeriesById(id);};
    source.calendar.mutations.saveSeries=[&](calendar::CalendarSeries item){
        const auto id=item.id;
        const int revision=item.revision;
        return id.empty()?service.CreateSeries(std::move(item)):
            service.UpdateSeries(id,revision,std::move(item));};
    source.calendar.mutations.removeSeries=[&](const std::string& id,int revision){
        return service.RemoveSeries(id,revision);};
    SystemPanelModel model(std::move(source),{},StatusBarAction::Calendar);
    model.Refresh(300,420);
    Require(model.Invoke("calendar.add")&&model.SetCalendarInput("calendar.edit.title",L"Series")&&
        model.SelectCalendarChoice("calendar.edit.mode",1)&&model.Invoke("calendar.edit.date")&&
        Node(model.View(),"picker.day:2026-09-26").selected&&
        model.Invoke("picker.day:2026-09-28")&&
        Node(model.View(),"picker.day:2026-09-28").selected&&model.Invoke("picker.confirm")&&
        Node(model.View(),"calendar.edit.date").text.find(L"2026-09-26")!=std::wstring::npos&&
        Node(model.View(),"calendar.edit.date").text.find(L"2026-09-28")!=std::wstring::npos&&
        !model.View().Find("calendar.edit.dates.add"),
        "multiple-date panel editor did not show its checked dates in the date field");
    Require(model.Invoke("calendar.edit.date")&&model.Invoke("picker.day:2026-09-26")&&
        !Node(model.View(),"picker.day:2026-09-26").selected&&model.Invoke("picker.cancel")&&
        Node(model.View(),"calendar.edit.date").text.find(L"2026-09-26")!=std::wstring::npos,
        "cancelling multiple-date selection changed the saved draft");
    CheckLayout(model.View());
    Require(model.Reveal("calendar.edit.save")&&model.Invoke("calendar.edit.save")&&
        !model.CalendarEditing()&&service.Series().size()==1&&
        service.Series().front().rule.dates.size()==2,
        "multiple-date panel editor did not create one series");
    const auto id=service.Series().front().id;
    auto moved=service.EventById(id+"/2026-09-28");
    Require(moved.has_value(),"series fixture did not create its second occurrence");
    moved->date="2026-09-29";
    Require(service.Update(moved->id,moved->revision,*moved).ok,
        "series fixture could not move one occurrence");
    model.Refresh(300,420);
    const auto node="event:2026-09-26:"+id+"/2026-09-26";
    Require(model.CalendarEventCommand(node,false,true)&&
        model.View().Find("calendar.edit.scope")&&
        model.SelectCalendarChoice("calendar.edit.mode",1)&&
        model.SelectCalendarChoice("calendar.edit.endType",1)&&
        model.CalendarChoices("calendar.edit.endType")[1].selected&&
        model.View().Find("calendar.edit.endDate")&&
        model.Reveal("calendar.edit.save"),
        "series panel did not expose its whole-series rule controls");
    CheckLayout(model.View());
    Require(model.Invoke("calendar.edit.save")&&model.CalendarEditing()&&
        Node(model.View(),"calendar.edit.error").text==_LW("settings.calendar.confirmExceptions"),
        "changing a series rule did not ask before discarding an unrelated override");
    Require(model.Invoke("calendar.edit.save")&&!model.CalendarEditing()&&
        service.Series().front().rule.kind=="weekly"&&
        service.Series().front().exceptions.empty()&&
        !service.EventById(id+"/2026-09-28"),
        "confirmed series rule change did not discard only obsolete overrides");
    model.Refresh(300,420);
    Require(model.CalendarEventCommand(node,true,true)&&
        model.View().Find("calendar.edit.confirmDelete")&&
        model.Invoke("calendar.edit.confirmDelete")&&service.Series().empty(),
        "whole-series context removal did not delete the definition");
    std::filesystem::remove_all(path.parent_path(),error);
}
void CheckFeedbackLayouts()
{
    auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
    auto source=FixtureSource(state);const auto read=source.current;
    std::wstring openedSettings;source.settings=[&](const wchar_t* uri){openedSettings=uri;};
    source.current=[read](std::string_view topic) {
        auto result=read(topic);
        if(result&&topic=="audio.devices")
        {
            auto& devices=result->value.object["devices"].array;const auto original=devices.front();
            devices.push_back(original);
            auto historical=original;historical.object["id"]=j::Text("historical");historical.object["state"]=j::Text("notPresent");devices.push_back(historical);
            auto unnamed=original;unnamed.object["id"]=j::Text("unnamed");unnamed.object["name"]=j::Text("  ");devices.push_back(unnamed);
            auto virtualDevice=original;virtualDevice.object["id"]=j::Text("virtual-output");virtualDevice.object["name"]=j::Text("Virtual Output");devices.push_back(virtualDevice);
        }
        return result;
    };
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
    Require(model.Invoke("system.settings")&&state->nativeControls==1,"overview settings button did not use the native control-center boundary");
    model.Select("audio");CheckLayout(model.View());
    for(const auto* direction:{"output","input"})
    {
        const auto prefix="audio."+std::string(direction);const auto& number=Node(model.View(),prefix+".value");
        const auto& mute=Node(model.View(),prefix+".mute");const auto& label=Node(model.View(),prefix+".label");
        const auto slider=std::find_if(model.View().nodes.begin(),model.View().nodes.end(),[&](const auto& n){return n.id.starts_with(prefix+".volume:");});
        Require(slider!=model.View().nodes.end()&&number.trailing&&number.bounds.right==slider->bounds.right-10&&label.bounds.right+8<=number.bounds.left&&
            (mute.bounds.left+mute.bounds.right)/2==40&&slider->bounds.bottom<Node(model.View(),"output.heading").bounds.top,
            "audio adjustments did not align their value/icon columns or remain before the endpoint lists");
    }
    Require(model.View().Find("audio.output.device:virtual-output")&&!model.View().Find("audio.output.device:historical")&&
        !model.View().Find("audio.output.device:unnamed"),"audio presentation exposed stale/unnamed devices or filtered a real virtual endpoint");
    for(const auto* page:{"wifi","bluetooth"})
    {
        model.Select(page);const auto radio="radio:"+std::string(page);const auto& toggle=Node(model.View(),radio);
        if(std::string_view(page)=="wifi")Require(Node(model.View(),"wifi.scan").role==ui::Role::Icon&&Node(model.View(),"wifi.scan").text.empty()&&
            !Node(model.View(),"wifi.scan").tooltip.empty()&&!Node(model.View(),"wifi.scan").accessibilityLabel.empty(),
            "scan action retained a truncated fixed-width label or lost its accessible tooltip");
        Require(toggle.switchStyle&&toggle.role==ui::Role::Toggle,"detail page omitted its real switch control");
        ui::Input input;input.Sync(model.View());Require(input.Focus(radio),"detail switch lost keyboard focus");
        const auto action=model.HandleKey(input,VK_SPACE,false);
        Require(action.kind==ui::InputResult::Kind::Invoke&&action.id==radio,"detail switch lost keyboard activation identity");
        const auto regions=input.AccessibilityRegions();const auto semantic=std::find_if(regions.begin(),regions.end(),[&](const auto& r){return input.Identity(r.key)==radio;});
        Require(semantic!=regions.end()&&semantic->accessibilityRole=="switch"&&semantic->checked==toggle.selected,"detail switch lost its UIA role or checked state");
        const auto prefix=std::string(page)=="wifi"?"wifi.network:":"bluetooth.device:";
        const auto row=std::find_if(model.View().nodes.begin(),model.View().nodes.end(),[prefix](const auto& n){return n.id.starts_with(prefix);});
        Require(row!=model.View().nodes.end(),"expandable device fixture has no row");
        const auto id=row->id;const auto key=id.substr(std::string_view(prefix).size());const auto command=std::string(page)+".connect:"+key;
        Require(!model.View().Find(command)&&model.Invoke(id)&&model.View().Find(command),"device row did not expand without dispatching a device mutation");
        CheckLayout(model.View());
        Require(model.Invoke(id)&&!model.View().Find(command),"device row could not collapse its actions");
        input.Sync(model.View());Require(input.Focus("header.settings"),"header settings icon lost keyboard focus");
        const auto settings=model.HandleKey(input,VK_RETURN,false);
        Require(settings.kind==ui::InputResult::Kind::Invoke&&settings.id=="header.settings","header settings icon lost its action identity");
    }
    for(const auto& [page,uri]:std::vector<std::pair<std::string,const wchar_t*>>{
        {"audio",L"ms-settings:sound"},{"brightness",L"ms-settings:display"},{"wifi",L"ms-settings:network-wifi"},
        {"wifi-adapters",L"ms-settings:network-wifi"},{"bluetooth",L"ms-settings:bluetooth"},{"power",L"ms-settings:powersleep"}})
    {
        model.Select(page);CheckLayout(model.View());const auto& scene=model.View();
        const auto& settings=Node(scene,"header.settings");const auto& title=Node(scene,"title");
        Require(!scene.Find("footer.settings")&&settings.role==ui::Role::Icon&&settings.text.empty()&&
            settings.bounds.right-settings.bounds.left==36&&settings.bounds.top==title.bounds.top&&
            settings.bounds.left>=title.bounds.right&&!settings.tooltip.empty()&&!settings.accessibilityLabel.empty(),
            "settings entry is not a labeled compact icon to the right of its title");
        if(const auto* radio=scene.Find("radio:"+page))Require(settings.bounds.right+8<=radio->bounds.left,
            "header settings icon overlaps its radio switch");
        openedSettings.clear();Require(model.Invoke("header.settings")&&openedSettings==uri,
            "header settings icon lost its page-specific system destination");
    }
}

void CheckCalendarResponsive()
{
    auto state=std::make_shared<PreviewState>();state->agenda=true;
    SystemPanelModel model(FixtureSource(state),{},StatusBarAction::Calendar);
    for(const float width:{900.f,700.f,420.f,260.f})
    {
        model.Refresh(300,width);model.Scroll(-model.MaximumScroll());CheckLayout(model.View());
        const auto& scene=model.View();Require(scene.width<=width,"calendar exceeded its monitor width budget");
        for(const auto& node:scene.nodes)Require(node.bounds.left>=0&&node.bounds.right<=width,"responsive calendar content overflowed horizontally");
        const auto month=Node(scene,"calendar.month").bounds,agenda=Node(scene,"calendar.selected").bounds;
        Require(!scene.Find("calendar.day")&&!scene.Find("calendar.todayLabel")&&month.left==16&&month.top==16,
            "calendar retained the removed today summary or its reserved space");
        if(width>=560)Require(agenda.left>Node(scene,"date:2026-09-27").bounds.right&&agenda.top==month.top&&
            agenda.left-32==scene.width-agenda.left-16,"wide calendar did not use equal month/agenda columns");
        else Require(agenda.top>Node(scene,"date:2026-09-30").bounds.bottom,"narrow calendar did not move its agenda below the month");
        Require(model.MaximumScroll()>0,"short calendar viewport lost scroll access to agenda");
        model.Reveal("calendar.manage");const auto& manage=Node(model.View(),"calendar.manage");
        Require(!HasArea(manage.clip)||(manage.bounds.top>=manage.clip.top&&manage.bounds.bottom<=manage.clip.bottom),"calendar action could not be revealed in a short viewport");
        if(width>=560)
        {
            const auto originalDay=Node(model.View(),"date:2026-09-27").bounds;
            model.Scroll(model.MaximumScroll());const auto movedDay=Node(model.View(),"date:2026-09-27").bounds;
            const float offset=model.ScrollOffset();ui::Input input;input.Sync(model.View());Require(input.Focus("date:2026-09-27"),"fixed month lost keyboard focus");
            model.Reveal(input.Focused());
            Require(originalDay.top==movedDay.top&&originalDay.bottom==movedDay.bottom&&model.ScrollOffset()==offset&&
                model.ScrollViewport().left>originalDay.right,"wide agenda scrolling moved the month or focusing the fixed month reset agenda scroll");
        }
        // Fixed-height output must retain access to the final event, rather
        // than merely clipping a list that can never scroll fully into view.
        model.Scroll(model.MaximumScroll());
        constexpr auto lastEvent="event:2026-09-28:preview-weekend";
        const auto& last=Node(model.View(),lastEvent);const auto visible=model.ScrollViewport();
        Require(last.bounds.left>=visible.left&&last.bounds.right<=visible.right&&
            last.bounds.top>=visible.top&&last.bounds.bottom<=visible.bottom,
            "the final agenda event is clipped at maximum scroll");
        ui::Input agendaInput;const auto eventPoint=VisibleCenter(model.View(),lastEvent);
        Require(agendaInput.Press(model.View(),eventPoint),"the final agenda event cannot receive a pointer press");
        const auto eventAction=agendaInput.Release(model.View(),eventPoint);
        Require(eventAction.kind==ui::InputResult::Kind::Invoke&&eventAction.id==lastEvent,
            "the final agenda event cannot be activated after scrolling");
        model.Refresh(300);Require(model.View().width<=width,"internal refresh forgot its monitor width budget");
    }
    auto stackedSource=FixtureSource(state);
    stackedSource.calendar.mutations.save=[](const auto&) -> calendar::MutationResult {
        throw std::runtime_error("stacked layout check must not persist the draft");
    };
    SystemPanelModel stacked(std::move(stackedSource),{},StatusBarAction::Calendar,true);
    stacked.Refresh(780,1920);CheckLayout(stacked.View());
    const auto calendarTop=Node(stacked.View(),"calendar.month").bounds.top;
    Require(stacked.View().width==384 && stacked.View().cards.size()==2&&
        Node(stacked.View(),"calendar.selected").bounds.top<calendarTop&&
        stacked.View().cards[0].bottom+8==stacked.View().cards[1].top&&
        stacked.View().cards[0].bottom>=320,
        "right-side clock did not place a taller agenda card above the month");
    stacked.Scroll(stacked.MaximumScroll());
    Require(Node(stacked.View(),"calendar.month").bounds.top==calendarTop &&
        stacked.ScrollViewport().bottom<stacked.View().cards[1].top,
        "scrolling the upper agenda moved the month or crossed into its card");
    Require(stacked.Invoke("calendar.add") && stacked.SetCalendarInput("calendar.edit.title",L"Stacked draft"),
        "stacked agenda could not open its editor");
    stacked.Reveal("calendar.edit.save");CheckLayout(stacked.View());
    Require(Node(stacked.View(),"calendar.month").bounds.top==calendarTop &&
        VisibleCenter(stacked.View(),"calendar.edit.save").y<stacked.View().cards[0].bottom,
        "stacked editor escaped its upper viewport or made save unreachable");
    Require(stacked.Invoke("calendar.edit.start"),"stacked editor could not open its time picker");
    stacked.Reveal("picker.confirm");
    Require(Node(stacked.View(),"calendar.month").bounds.top==calendarTop &&
        VisibleCenter(stacked.View(),"picker.confirm").y<stacked.View().cards[0].bottom,
        "stacked time picker moved the month or clipped its confirm action");
    Require(stacked.CalendarBack(),"stacked picker back failed");
    const auto draft=stacked.CalendarInputFields();
    Require(std::any_of(draft.begin(),draft.end(),[](const auto& field){return field.id=="calendar.edit.title"&&field.text==L"Stacked draft";}),
        "stacked picker discarded the agenda draft");
    Require(stacked.CalendarBack(),"stacked editor back failed");
    stacked.Refresh(300,1920);stacked.Reveal("calendar.manage");
    Require(stacked.MaximumScroll()>0 && VisibleCenter(stacked.View(),"calendar.manage").y<300,
        "short display lost access to the stacked agenda controls");
    state->agenda=false;model.Refresh(300,900);
    Require(model.MaximumScroll()==0&&!model.View().Find("scrollbar"),"a fixed month created scrolling for an empty short agenda");
    const auto& empty=Node(model.View(),"calendar.empty");const auto viewport=model.ScrollViewport();
    Require(empty.centered&&empty.bounds.left==Node(model.View(),"calendar.selected").bounds.left&&
        empty.bounds.right==model.View().width-16&&empty.bounds.top==viewport.top&&empty.bounds.bottom==viewport.bottom,
        "empty agenda is not centered in the visible content area to the right of the month");
    state->agenda=true;model.Refresh(400,900);
    const auto fullSecondary=Node(model.View(),"calendar.selectedSecondary").text;
    Require(Node(model.View(),"date:2026-09-26").tooltip.find(fullSecondary)!=std::wstring::npos,
        "date hover omitted the full secondary calendar date");
    const auto gregorianMonth=Node(model.View(),"calendar.month").text;
    const auto effectiveLanguage=Locale::Instance().GetEffectiveLanguage();
    const bool chineseLanguage=effectiveLanguage=="zh-CN"||effectiveLanguage=="zh-TW";
    Require(Node(model.View(),"calendar.toggleCalendar").role==ui::Role::Button&&
        Node(model.View(),"calendar.toggleCalendar").text==_LW("statusBar.calendarChineseShort"),
        "secondary calendar switch did not use a labeled button");
    Require(model.Invoke("calendar.toggleCalendar")&&
        Node(model.View(),"calendar.month").text!=gregorianMonth&&
        (!chineseLanguage||Node(model.View(),"calendar.month").text.find(L"丙午年")!=std::wstring::npos)&& // l10n-allow: intrinsic sexagenary calendar fixture
        Node(model.View(),"calendar.toggleCalendar").text==_LW("statusBar.calendarGregorianShort")&&
        Node(model.View(),"date:2026-09-26").text!=L"26"&&
        !model.View().Find("date:2026-08-31")&&
        Node(model.View(),"date:2026-09-11").text==L"1"&&
        !Node(model.View(),"date:2026-09-11").secondary&&
        Node(model.View(),"date:2026-09-10").secondary&&
        Node(model.View(),"calendar.selected").text==fullSecondary&&
        Node(model.View(),"calendar.selectedSecondary").text==L"2026-09-26",
        "calendar switch did not rebuild the month boundary, heading and selected date together");
    Require(model.Invoke("calendar.next")&&!model.View().Find("date:2026-09-29")&&
        model.Invoke("calendar.previous")&&Node(model.View(),"date:2026-09-11").text==L"1",
        "secondary month navigation still advanced Gregorian months");
    Require(model.Invoke("calendar.toggleCalendar")&&Node(model.View(),"calendar.month").text==gregorianMonth&&
        Node(model.View(),"date:2026-09-26").text==L"26",
        "calendar switch did not restore Gregorian display");
    const auto batches=state->calendarBatches;
    model.Refresh(400);Require(model.Invoke("date:2026-09-27")&&state->calendarBatches==batches,
        "unchanged month or selection rebuilt the same secondary calendar annotations");
    state->agenda=false;model.Refresh(400);
    Require(state->calendarBatches==batches+1&&!model.View().Find("calendar.selectedSecondary")&&
        Node(model.View(),"date:2026-09-27").tooltip.find(L'\n')==std::wstring::npos,
        "changed calendar preferences retained cached secondary date text");
    for(const auto* date:{"0001-01-01","9999-12-31"})
    {
        auto boundary=FixtureSource(std::make_shared<PreviewState>());boundary.calendar.today=[date]{return date;};
        SystemPanelModel edge(std::move(boundary),{},StatusBarAction::Calendar);
        Require(!Node(edge.View(),date[0]=='0'?"calendar.previous":"calendar.next").enabled,"calendar navigation escaped the supported date range");
    }
    auto markedSource=FixtureSource(std::make_shared<PreviewState>());
    std::vector<calendar::CalendarEvent> markedEvents;
    for(const auto* date:{"2026-08-31","2026-09-26","2026-10-01"})
    {calendar::CalendarEvent event;event.id=date;event.date=date;markedEvents.push_back(std::move(event));}
    unsigned eventBatches=0;
    markedSource.calendar.events=[](const std::string&)->std::vector<calendar::CalendarEvent>{throw std::runtime_error("batch calendar must not fall back to per-day reads");};
    markedSource.calendar.eventsInRange=[&](const std::string& from,const std::string& to){
        ++eventBatches;std::vector<calendar::CalendarEvent> result;
        for(const auto& event:markedEvents)if(event.date>=from&&event.date<=to)result.push_back(event);return result;
    };
    SystemPanelModel marked(std::move(markedSource),{},StatusBarAction::Calendar);
    Require(eventBatches==1&&Node(marked.View(),"date:2026-08-31").marked&&Node(marked.View(),"date:2026-09-26").marked&&
        Node(marked.View(),"date:2026-10-01").marked&&!Node(marked.View(),"date:2026-09-27").marked,
        "one calendar batch did not mark the real event dates across month boundaries");
    markedEvents[1].date="2026-09-27";markedEvents.erase(markedEvents.begin());marked.Refresh();
    Require(eventBatches==2&&!Node(marked.View(),"date:2026-08-31").marked&&!Node(marked.View(),"date:2026-09-26").marked&&
        Node(marked.View(),"date:2026-09-27").marked,"event edits or deletions left stale calendar dots");
}

std::vector<std::uint32_t> Render(ID2D1Device* device, IDWriteFactory* text,
    const native_component_preview::Request& request, const ui::Scene& scene,
    const PersonalizationSettings& appearance, const SystemPanel::Background& background,
    const widget_preview::Wallpaper& stage, int left, int top,const ui::Palette* palette=nullptr,
    std::string_view focused={},std::string_view hovered={},std::string_view pressed={})
{
    const float scale = static_cast<float>(request.dpi) / 96.f;
    ComPtr<ID2D1DeviceContext> context; Require(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&context));
    const auto size = D2D1::SizeU(static_cast<UINT>(request.canvasWidth),static_cast<UINT>(request.canvasHeight));
    const auto format = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> target;
    Require(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,format,96,96),&target));
    context->SetTarget(target.Get()); context->SetDpi(96,96); context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    context->BeginDraw(); context->Clear(D2D1::ColorF(0,0.f));
    if (!request.transparent && !request.contentOnly)
    {
        ComPtr<ID2D1Bitmap> wallpaper;
        Require(context->CreateBitmap(size,stage.pixels.data(),static_cast<UINT>(stage.width*4),D2D1::BitmapProperties(format),&wallpaper));
        context->DrawBitmap(wallpaper.Get());
    }
    context->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(left),static_cast<float>(top)));
    if (!request.contentOnly)
        for (const auto& card : scene.cards)
            background(context.Get(),{static_cast<LONG>(std::lround(card.left*scale)),static_cast<LONG>(std::lround(card.top*scale)),
                static_cast<LONG>(std::lround(card.right*scale)),static_cast<LONG>(std::lround(card.bottom*scale))},appearance,scale);
    context->SetTransform(D2D1::Matrix3x2F::Scale(scale,scale)*
        D2D1::Matrix3x2F::Translation(static_cast<float>(left),static_cast<float>(top)));
    const auto contentResult = ui::Draw(context.Get(),text,scene,palette?*palette:SystemPanelPalette(appearance),hovered,focused,pressed);
    const auto drawResult = context->EndDraw(); context->SetTarget(nullptr); Require(contentResult); Require(drawResult);
    ComPtr<ID2D1Bitmap1> readback;
    Require(context->CreateBitmap(size,nullptr,0,D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,format,96,96),&readback));
    Require(readback->CopyFromBitmap(nullptr,target.Get(),nullptr));
    D2D1_MAPPED_RECT mapped{}; Require(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped));
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(request.canvasWidth)*request.canvasHeight);
    for (int y = 0; y < request.canvasHeight; ++y)
        std::memcpy(pixels.data()+static_cast<std::size_t>(y)*request.canvasWidth,
            mapped.bits+static_cast<std::size_t>(y)*mapped.pitch,static_cast<std::size_t>(request.canvasWidth)*4);
    Require(readback->Unmap());
    return pixels;
}

void CheckPowerConfirmationOrigin()
{
    for(const bool standalone:{false,true})
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);std::uint64_t started=0;std::vector<std::uint64_t> canceled;
        source.start=[&](system_control::Request request){
            Require(request.hostConfirmed&&request.name=="system.power.restart","power confirmation changed its requested action");return ++started;};
        source.cancel=[&](std::uint64_t task){canceled.push_back(task);return true;};
        SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
        const auto click=[&](std::string_view id){model.Reveal(id);ui::Input input;const auto point=VisibleCenter(model.View(),id);
            Require(input.Press(model.View(),point),"power confirmation rejected a pointer target");const auto result=input.Release(model.View(),point);
            Require(result.kind==ui::InputResult::Kind::Invoke&&result.id==id&&model.Invoke(result.id),"power confirmation pointer action was lost");};
        const auto open=[&]{
            if(standalone)Require(model.BeginPowerConfirmation("system.power.restart",true),"system-menu confirmation origin setup failed");
            else{model.Select("power");click("power.restart");}
            Require(model.View().Find("control.confirm")&&(model.View().Find("back")!=nullptr)==!standalone&&
                (model.View().Find("header.settings")!=nullptr)==!standalone,
                "power confirmation inherited an unrelated back/settings route");};
        const auto returned=[&]{Require(model.TakeDismissRequest()==standalone&&!model.TakeDismissRequest(),
                "power confirmation did not request exactly the source-specific dismissal");
            if(!standalone)Require(model.Page()=="power"&&!model.View().Find("control.confirm")&&model.View().Find("back"),
                "control-panel confirmation failed to restore the power page");};
        open();click("control.cancel");returned();Require(started==0,"canceling power confirmation performed an operation");
        open();Require(model.ControlBack(),"power confirmation lost its Escape route");returned();
        open();click("control.confirm");
        system_control::Completion denied;denied.id=started;denied.error="accessDenied";state->completions.push_back(denied);model.Refresh();
        Require(!model.TakeDismissRequest()&&model.View().Find("control.error")&&Node(model.View(),"control.confirm").enabled,
            "failed power operation dismissed its retryable confirmation");
        click("control.cancel");returned();
        for(const bool succeeded:{true,false})
        {
            open();click("control.confirm");system_control::Completion completion;completion.id=started;
            completion.ok=succeeded;if(!succeeded)completion.error="canceled";state->completions.push_back(completion);model.Refresh();returned();
        }
        open();click("control.confirm");const auto task=started;click("control.cancel");returned();
        Require(canceled==std::vector<std::uint64_t>{task},"canceling pending power confirmation failed to cancel its own task once");
        model.Close();system_control::Completion late;late.id=task;late.ok=true;state->completions.push_back(late);model.Refresh();
        Require(!model.TakeDismissRequest()&&!model.Invoke("control.confirm"),"late power completion revived a closed confirmation");
    }
}

void CheckInlineControlForms(ID2D1Device* device,IDWriteFactory* text,
    const native_component_preview::Request& request,native_component_preview::Result& result,
    const PersonalizationSettings& appearance,const SystemPanel::Background& background,const widget_preview::Wallpaper& stage)
{
    CheckSystemControlPasswordInput();CheckPowerConfirmationOrigin();
    const float scale=static_cast<float>(request.dpi)/96.f;
    for(int mode=0;mode<4;++mode)
    {
        auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        auto source=FixtureSource(state);const auto read=source.current;
        const auto start=source.start;std::vector<system_control::Request> requests;std::vector<std::uint64_t> canceled;
        source.current=[read,mode](std::string_view topic){auto snapshot=read(topic);if(snapshot&&topic=="network.wifi"){
            auto& network=snapshot->value.object["interfaces"].array.front().object["networks"].array.front();
            network.object["connected"]=j::Boolean(false);network.object["connectable"]=j::Boolean(true);network.object["security"]=j::Text("wpa2");
            network.object["profileName"]=j::Text(mode==2?"Saved preview network":"");}return snapshot;};
        source.start=[&](system_control::Request value)->std::uint64_t{if(value.name=="network.wifi.scan")return start(std::move(value));
            Require(system_control::ValidateRequest(value)&&value.hostConfirmed,"inline form submitted an invalid or unconfirmed request");
            requests.push_back(std::move(value));return std::uint64_t{1000}+requests.size();};
        source.cancel=[&](std::uint64_t id){canceled.push_back(id);return true;};
        StatusBarSettings settings;if(mode==3)settings.powerControls=false;
        SystemPanelModel model(std::move(source),settings,StatusBarAction::ControlCenter);
        if(mode==3)Require(model.BeginPowerConfirmation("system.power.restart",true),"system-menu power action could not enter the shared confirmation page");
        else
        {
            model.Select("wifi");
            if(mode==1)Require(model.Invoke("wifi.hidden")&&model.Page()=="wifi-hidden","hidden network did not enter its third-level panel page");
            else Require(model.Invoke("wifi.network:network-preview")&&model.Invoke(mode==2?"wifi.forget:network-preview":"wifi.connect:network-preview"),
                "network did not expand its own input/confirmation");
        }
        model.Refresh(static_cast<float>(request.canvasHeight-2*request.padding)/scale,
            static_cast<float>(request.canvasWidth-2*request.padding)/scale);
        Require(requests.empty()&&model.View().Find("control.confirm"),"opening a panel form executed its action");
        if(mode==0)
        {
            const auto fields=model.ControlInputFields();Require(fields.size()==1&&fields.front().password&&fields.front().text.empty(),"Wi-Fi card omitted its protected inline field");
            const auto& card=Node(model.View(),"wifi.card:network-preview");
            Require(fields.front().bounds.top>=card.bounds.top&&fields.front().bounds.bottom<=card.bounds.bottom&&
                Node(model.View(),"control.confirm").bounds.bottom<=card.bounds.bottom,"Wi-Fi password escaped its expanded network card");
        }
        if(mode==1)
        {
            std::wstring tooLong(20,L'\u4e2d');model.SetControlInput("control.ssid",tooLong);model.Refresh();
            Require(!Node(model.View(),"control.confirm").enabled,"hidden SSID limit counted characters instead of UTF-8 bytes");
            std::wstring ssid=L"SnowDesktop hidden preview";model.SetControlInput("control.ssid",ssid);model.Refresh();
            Require(model.ControlInputFields().size()==2&&Node(model.View(),"control.confirm").enabled,"hidden SSID/security/password did not share one page");
        }
        CheckLayout(model.View());
        const int width=static_cast<int>(std::ceil(model.View().width*scale)),height=static_cast<int>(std::ceil(model.View().height*scale));
        const int left=(request.canvasWidth-width)/2,top=(request.canvasHeight-height)/2;
        auto pixels=Render(device,text,request,model.View(),appearance,background,stage,left,top);
        auto fields=model.ControlInputFields();for(auto& field:fields){field.bounds.left+=left/scale;field.bounds.right+=left/scale;field.bounds.top+=top/scale;field.bounds.bottom+=top/scale;field.clip.left+=left/scale;field.clip.right+=left/scale;field.clip.top+=top/scale;field.clip.bottom+=top/scale;}
        if(!fields.empty())
        {
            const auto withoutInputs=pixels;OverlaySystemCalendarInputs(fields,appearance,request.dpi,request.canvasWidth,request.canvasHeight,pixels);
            Require(pixels!=withoutInputs,"control panel preview omitted the real native EDIT children");
            if(mode==0)
            {
                const auto& password=fields.back();
                auto hovered=Render(device,text,request,model.View(),appearance,background,stage,left,top,nullptr,{},password.id);
                const auto withoutHoverInputs=hovered;OverlaySystemCalendarInputs(fields,appearance,request.dpi,request.canvasWidth,request.canvasHeight,hovered);
                const int x0=static_cast<int>(std::floor(password.bounds.left*scale)),y0=static_cast<int>(std::floor(password.bounds.top*scale));
                const int x1=static_cast<int>(std::ceil(password.bounds.right*scale))-1,y1=static_cast<int>(std::ceil(password.bounds.bottom*scale))-1;
                for(const int y:{y0,y1})for(const int x:{x0,x1})
                {
                    const auto index=static_cast<std::size_t>(y)*request.canvasWidth+x;
                    Require(pixels[index]==withoutInputs[index]&&hovered[index]==withoutHoverInputs[index],
                        "inline native password corners covered the real idle/hover card background");
                }
            }
        }
        const std::string name=mode==0?"inline-wifi-password":mode==1?"hidden-network-page":mode==2?"inline-forget-confirmation":"inline-power-confirmation";
        const auto path=request.outputDirectory/(request.component+"-"+name+".png");
        if(!preview_png::Save(path,request.canvasWidth,request.canvasHeight,pixels,result.error))throw std::runtime_error(result.error);
        result.outputs.push_back({request.component,name,path,false,false,false,false,false,false,static_cast<int>(std::lround(appearance.cornerRadius*scale)),width,height,left,top});
        if(mode<2)
        {
            if(mode==0)
            {
                ui::Input cancelInput;const auto cancelPoint=VisibleCenter(model.View(),"control.cancel");
                Require(cancelInput.Press(model.View(),cancelPoint)&&model.Invoke(cancelInput.Release(model.View(),cancelPoint).id)&&model.ControlInputFields().empty()&&requests.empty(),
                    "canceling an unsubmitted password card retained its input or executed a request");
                Require(model.Invoke("wifi.connect:network-preview"),"cancelled password card could not be reopened");
            }
            auto id=model.ControlInputFields().back().id;std::wstring invalid=L"short";model.SetControlInput(id,invalid);
            Require(std::all_of(invalid.begin(),invalid.end(),[](wchar_t ch){return ch==0;}),"password bridge retained its temporary input buffer");
            model.Invoke("control.confirm");Require(requests.empty()&&model.View().Find("control.error"),"invalid password bypassed explicit validation");
            std::wstring password=L"offline-only-password";model.SetControlInput(id,password);model.Refresh();
            for(const auto& field:model.ControlInputFields())if(field.password)Require(field.text.empty(),"password entered an ordinary EDIT descriptor");
            for(const auto& node:model.View().nodes)Require(node.text.find(L"offline-only-password")==std::wstring::npos&&node.tooltip.find(L"offline-only-password")==std::wstring::npos,
                "password leaked into the render/accessibility scene");
            if(mode==1){Require(model.Invoke("control.security:open")&&model.ControlInputFields().size()==1,"open security kept its password child");}
        }
        model.Reveal("control.confirm");ui::Input input;input.Sync(model.View());const auto point=VisibleCenter(model.View(),"control.confirm");
        Require(input.Press(model.View(),point),"inline confirm lost pointer input");const auto action=input.Release(model.View(),point);
        Require(action.kind==ui::InputResult::Kind::Invoke&&model.Invoke(action.id)&&requests.size()==1,"inline confirmation did not dispatch exactly once");
        Require(!model.Invoke("control.confirm")&&requests.size()==1,"in-flight confirmation allowed duplicate submission");
        if(mode==1)Require(requests.front().password.View().empty()&&requests.front().arguments.at("security")=="open","open hidden network retained a secret");
        if(mode==0)Require(requests.front().password.View()==L"offline-only-password","protected inline password never reached the internal request");
        if(mode==3)
        {
            system_control::Completion failed;failed.id=1001;failed.error="unavailable";state->completions.push_back(failed);model.Refresh();
            Require(!model.View().Find("control.error")&&Node(model.View(),"control.confirm").enabled&&model.Invoke("control.confirm")&&requests.size()==2,
                "generic confirmation failure added a vague banner or prevented retry");
        }
        const auto activeTask=std::uint64_t{1000}+requests.size();
        if(mode%2==0)model.Select("audio");else model.Close();
        Require(canceled==std::vector<std::uint64_t>{activeTask}&&model.ControlInputFields().empty(),"navigation/close retained a pending sensitive control operation");
        system_control::Completion late;late.id=activeTask;late.error="accessDenied";state->completions.push_back(late);model.Refresh();
        Require(mode%2==0?!model.View().Find("control.error"):!model.Invoke("control.confirm"),"obsolete inline completion leaked into the replacement page");
    }
}

void CheckCalendarPageVisuals(ID2D1Device* device,IDWriteFactory* text,
    const native_component_preview::Request& request,native_component_preview::Result& result,
    const PersonalizationSettings& appearance,const SystemPanel::Background& background,const widget_preview::Wallpaper& stage)
{
    CheckSystemCalendarInputs();
    calendar::CalendarEvent event;event.id="offline-calendar-page";event.revision=2;
    event.title=_L("settings.calendar.events");event.date="2026-09-26";event.startMinutes=630;event.endMinutes=690;
    event.reminderMinutes=15;event.notes=_L("settings.calendar.pageDescription");
    const float scale=static_cast<float>(request.dpi)/96.f;
    for(int page=0;page<9;++page)
    {
        const bool seriesDates=page==7,seriesMonthly=page==8;
        const bool creating=page==0||seriesDates||seriesMonthly,confirmation=page==2,overflow=page==3,
            narrow=page==4||seriesMonthly,datePicker=page==5,timePicker=page==6,picker=datePicker||timePicker;
        auto source=FixtureSource(std::make_shared<PreviewState>());
        source.calendar.events=[event](const std::string& date){return date==event.date?std::vector{event}:std::vector<calendar::CalendarEvent>{};};
        source.calendar.mutations.current=[event](const auto&){return std::optional(event);};
        source.calendar.mutations.save=[](const auto&)->calendar::MutationResult{throw std::runtime_error("calendar visual preview attempted persistence");};
        source.calendar.mutations.remove=[](const auto&)->calendar::MutationResult{throw std::runtime_error("calendar visual preview attempted deletion");};
        SystemPanelModel model(std::move(source),{},StatusBarAction::Calendar);
        model.Refresh(static_cast<float>(request.canvasHeight-2*request.padding)/scale,
            (std::min)(narrow?320.f:720.f,static_cast<float>(request.canvasWidth-2*request.padding)/scale));
        Require(creating?model.Invoke("calendar.add"):model.CalendarEventCommand("event:2026-09-26:offline-calendar-page",confirmation),
            "calendar visual fixture did not enter the real secondary page");
        if(seriesDates)
        {
            Require(model.SelectCalendarChoice("calendar.edit.mode",1)&&model.Invoke("calendar.edit.date")&&
                model.Invoke("picker.day:2026-09-28")&&model.Invoke("picker.confirm")&&
                Node(model.View(),"calendar.edit.date").text.find(L"2026-09-28")!=std::wstring::npos,
                "calendar multiple-date preview could not show the selected dates");
        }
        if(seriesMonthly)
        {
            Require(model.SelectCalendarChoice("calendar.edit.mode",3),
                "calendar monthly preview could not show its last-day rule");
            const auto& before=model.View();
            const auto& minus=Node(before,"calendar.edit.interval.minus");
            const auto& value=Node(before,"calendar.edit.interval.value");
            const auto& plus=Node(before,"calendar.edit.interval.plus");
            Require(minus.bounds.right<value.bounds.left&&value.bounds.right<plus.bounds.left&&
                plus.bounds.left-minus.bounds.right<=88.f&&value.text==L"1"&&
                model.CalendarChoices("calendar.edit.monthDay")[26].selected&&
                model.CalendarChoices("calendar.edit.endType")[0].selected,
                "monthly interval and choices did not expose a compact value and selected options");
            Require(model.SelectCalendarChoice("calendar.edit.monthDay",0)&&
                model.CalendarChoices("calendar.edit.monthDay")[0].selected&&
                Node(model.View(),"calendar.edit.monthDay").text==_LW("settings.calendar.lastDay"),
                "choosing the last day did not update the dropdown value");
            model.Reveal("calendar.edit.monthDay");
            const auto& last=Node(model.View(),"calendar.edit.monthDay");
            Require(last.bounds.top>=last.clip.top&&last.bounds.bottom<=last.clip.bottom,
                "narrow monthly last-day choice cannot be revealed");
        }
        if(overflow)
        {
            const auto previous=Node(model.View(),"calendar.edit.notes").bounds;
            std::wstring notes;
            for(int line=0;line<24;++line)notes+=std::to_wstring(line+1)+L". "+_LW("settings.calendar.pageDescription")+L"\r\n";
            Require(model.SetCalendarInput("calendar.edit.notes",std::move(notes)),"long calendar notes did not reach the page draft");
            model.Refresh(300.f,model.View().width);
            const auto expanded=Node(model.View(),"calendar.edit.notes").bounds;
            Require(expanded.bottom-expanded.top>previous.bottom-previous.top&&
                model.View().Find("scrollbar"),"long calendar notes did not grow into the panel scroll area");
        }
        if(!creating)model.Reveal("calendar.edit.notes");
        if(picker)
        {
            Require(model.Invoke(datePicker?"calendar.edit.date":"calendar.edit.start"),"calendar visual fixture did not enter its shared picker");
            if(timePicker)Require(model.View().Find("picker.hour:current")&&model.View().Find("picker.minute:current"),
                "time visual fixture did not expose both columns together");
        }
        const auto& scene=model.View();CheckLayout(scene);
        if(creating&&scene.width>=560.f)Require(scene.height>=520.f&&scene.height<=560.f,
            "wide calendar editor did not respect its bounded height range");
        if(creating)
        {
            const auto& date=Node(scene,"calendar.edit.date");
            const auto& modeLabel=Node(scene,"calendar.edit.mode.label");
            const auto& lastMode=Node(scene,"calendar.edit.mode");
            const auto& allDay=Node(scene,"calendar.edit.allDay");
            Require(modeLabel.bounds.top>=date.bounds.bottom&&lastMode.bounds.bottom<=allDay.bounds.top&&
                modeLabel.bounds.left==date.bounds.left&&lastMode.bounds.right<=date.bounds.right,
                "calendar date modes must stay next to the date field and before the remaining fields");
            if(narrow)Require(lastMode.bounds.bottom<Node(scene,"calendar.edit.notes.label").bounds.top,
                "narrow calendar date modes must appear before notes");
            if(seriesMonthly)
            {
                const auto& interval=Node(scene,"calendar.edit.interval.label");
                const auto& monthDay=Node(scene,"calendar.edit.monthDay");
                const auto& endType=Node(scene,"calendar.edit.endType");
                Require(interval.bounds.top>=lastMode.bounds.bottom&&monthDay.bounds.top>=interval.bounds.bottom&&
                    endType.bounds.top>=monthDay.bounds.bottom&&endType.bounds.bottom<=allDay.bounds.top&&
                    endType.bounds.bottom<Node(scene,"calendar.edit.notes.label").bounds.top,
                    "monthly rule controls must stay with the date selection before notes");
            }
        }
        for(const auto* id:{"calendar.edit.save","calendar.edit.cancel","calendar.edit.delete","calendar.edit.confirmDelete","calendar.edit.cancelDelete"})
            if(const auto* action=scene.Find(id))
            {
                ComPtr<IDWriteTextFormat> format;ComPtr<IDWriteTextLayout> layout;
                Require(text->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,action->fontSize,L"",&format));
                Require(text->CreateTextLayout(action->text.data(),static_cast<UINT32>(action->text.size()),format.Get(),4096,256,&layout));
                DWRITE_TEXT_METRICS metrics{};Require(layout->GetMetrics(&metrics));
                Require(metrics.widthIncludingTrailingWhitespace<=action->bounds.right-action->bounds.left-24,
                    "calendar primary and destructive action labels must not be truncated");
            }
        if(narrow)
            for(const auto* id:{"calendar.edit.mode","calendar.edit.monthDay","calendar.edit.endType"})
                if(const auto* action=scene.Find(id))
                {
                    ComPtr<IDWriteTextFormat> format;ComPtr<IDWriteTextLayout> layout;
                    Require(text->CreateTextFormat(L"Segoe UI",nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
                        DWRITE_FONT_STRETCH_NORMAL,action->fontSize,L"",&format));
                    Require(text->CreateTextLayout(action->text.data(),static_cast<UINT32>(action->text.size()),
                        format.Get(),4096,256,&layout));
                    DWRITE_TEXT_METRICS metrics{};Require(layout->GetMetrics(&metrics));
                    Require(metrics.widthIncludingTrailingWhitespace<=action->bounds.right-action->bounds.left-16,
                        "narrow calendar mode label is truncated");
                }
        Require(model.CalendarEditing()&&scene.cards.size()==1&&(scene.width>=560?scene.Find("calendar.month")!=nullptr:scene.Find("calendar.month")==nullptr)&&
            (picker?scene.Find("picker.confirm")!=nullptr:confirmation?scene.Find("calendar.edit.confirmDelete")!=nullptr:
                Node(scene,"calendar.edit.reminder").text==_LW(creating?"settings.calendar.reminder.-1":"settings.calendar.reminder.15")),
            "calendar secondary page lost its retained month, shared card or reminder selection");
        const int width=static_cast<int>(std::ceil(scene.width*scale)),height=static_cast<int>(std::ceil(scene.height*scale));
        const int left=(request.canvasWidth-width)/2,top=(request.canvasHeight-height)/2;
        auto pixels=Render(device,text,request,scene,appearance,background,stage,left,top);
        auto fields=model.CalendarInputFields();Require(fields.size()==(picker||confirmation?0u:2u),"calendar secondary page lost an embedded native input or overlaid a picker/confirmation");
        for(auto& field:fields)
        {
            const float dx=static_cast<float>(left)/scale,dy=static_cast<float>(top)/scale;
            field.bounds.left+=dx;field.bounds.right+=dx;field.bounds.top+=dy;field.bounds.bottom+=dy;
            field.clip.left+=dx;field.clip.right+=dx;field.clip.top+=dy;field.clip.bottom+=dy;
            if(confirmation)Require(!field.enabled,"calendar delete visual fixture retained writable inputs");
        }
        // Composite the production EDIT children, including IME-compatible
        // text and notes that grow into the panel scroll area.
        const auto scenePixels=pixels;
        OverlaySystemCalendarInputs(fields,appearance,static_cast<UINT>(request.dpi),request.canvasWidth,request.canvasHeight,pixels);
        Require(picker||confirmation||pixels!=scenePixels,"calendar page preview omitted its real embedded input controls");
        if(!creating&&!picker&&!confirmation)
        {
            auto emptyNotes=fields;const auto notes=std::find_if(emptyNotes.begin(),emptyNotes.end(),[](const auto& field){return field.id=="calendar.edit.notes";});
            Require(notes!=emptyNotes.end()&&!notes->text.empty(),"calendar notes fixture has no actual text");notes->text.clear();
            auto withoutNotes=scenePixels;
            OverlaySystemCalendarInputs(emptyNotes,appearance,static_cast<UINT>(request.dpi),request.canvasWidth,request.canvasHeight,withoutNotes);
            auto inside=notes->bounds;inside.left+=12;inside.right-=28;inside.top+=8;inside.bottom-=8;inside=Intersection(inside,notes->clip);
            const int x0=(std::max)(0,static_cast<int>(std::ceil(inside.left*scale))),x1=(std::min)(request.canvasWidth,static_cast<int>(std::floor(inside.right*scale)));
            const int y0=(std::max)(0,static_cast<int>(std::ceil(inside.top*scale))),y1=(std::min)(request.canvasHeight,static_cast<int>(std::floor(inside.bottom*scale)));
            bool visibleText=false;
            for(int y=y0;y<y1&&!visibleText;++y)for(int x=x0;x<x1;++x)
                if(pixels[static_cast<std::size_t>(y)*request.canvasWidth+x]!=withoutNotes[static_cast<std::size_t>(y)*request.canvasWidth+x]){visibleText=true;break;}
            Require(visibleText,"calendar notes text did not appear in the actual native EDIT overlay");
        }
        const std::string name=seriesDates?"multiple-dates":seriesMonthly?"monthly-narrow":creating?"new-event":
            confirmation?"delete-confirmation":overflow?"editor-notes-overflow":narrow?"editor-narrow":
            datePicker?"date-picker":timePicker?"time-picker":"editor";
        const auto path=request.outputDirectory/(request.component+"-"+name+".png");
        if(!preview_png::Save(path,request.canvasWidth,request.canvasHeight,pixels,result.error))throw std::runtime_error(result.error);
        result.outputs.push_back({request.component,name,path,false,false,false,false,false,false,
            static_cast<int>(std::lround(appearance.cornerRadius*scale)),width,height,left,top});
    }
}

void CheckBatteryStates(ID2D1Device* device, IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=240;request.canvasHeight=48;request.dpi=96;request.transparent=request.contentOnly=true;
    auto state=std::make_shared<PreviewState>();auto source=FixtureSource(state);const auto read=source.current;
    double percent=100;bool charging=true,known=true,onAC=true;
    source.current=[&](auto topic){auto result=read(topic);if(result&&topic=="system.power.plans"){
        result->value.object["batteryPresent"]=j::Boolean(known);result->value.object["batteryPercent"]=j::Number(percent);
        result->value.object["charging"]=j::Boolean(charging);result->value.object["onAC"]=j::Boolean(onAC);}return result;};
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
    const auto capture=[&]{model.Refresh();ui::Scene scene;scene.width=240;scene.height=48;auto battery=Node(model.View(),"battery");battery.bounds={0,0,240,48};scene.nodes.push_back(battery);return Render(device,text,request,scene,appearance,background,{},0,0);};
    const auto filling=capture();const auto chargingTip=Node(model.View(),"battery").tooltip;
    Require(Node(model.View(),"battery").charging&&!Node(model.View(),"battery").positiveGlyph,"100% while charging lost its charging state");
    charging=false;const auto full=capture();
    Require(!Node(model.View(),"battery").charging&&Node(model.View(),"battery").positiveGlyph&&Node(model.View(),"battery").pluggedIn&&
        Node(model.View(),"battery").tooltip!=chargingTip&&full==filling,"AC and charging must share the bolt while their status tooltips remain distinct");
    const auto green=[](const auto& pixels){return std::count_if(pixels.begin(),pixels.end(),[](auto p){return ((p>>8)&255)>((p>>16)&255)+30&&((p>>8)&255)>(p&255)+30;});};
    Require(green(full)>8&&green(filling)>8,"charging/full battery did not render its green fill");
    const auto isGreen=[](auto pixel){return ((pixel>>8)&255)>((pixel>>16)&255)+30&&((pixel>>8)&255)>(pixel&255)+30;};
    for(int y=0;y<request.canvasHeight;++y)for(int x=0;x<request.canvasWidth;++x)
        if(isGreen(full[static_cast<std::size_t>(y)*request.canvasWidth+x]))
            Require(x>=2&&x<=16&&y>=21&&y<=26,"battery green paint escaped its internal fill into the outline or electrode");
    Require((full[24*240]>>24)>0&&!isGreen(full[24*240])&&(full[24*240+19]>>24)>0&&!isGreen(full[24*240+19]),
        "battery outline/electrode lost foreground color or the glyph retained its old left inset");
    charging=true;percent=30;const auto partial=capture();
    Require(green(partial)>0&&green(partial)<green(filling),"charging battery fill ignored the real percentage");
    ui::Scene highContrast;highContrast.width=240;highContrast.height=48;auto battery=Node(model.View(),"battery");battery.bounds={0,0,240,48};highContrast.nodes.push_back(battery);
    auto palette=SystemPanelPalette(appearance);palette.highContrast=true;palette.text=D2D1::ColorF(0xffff00);
    const auto contrast=Render(device,text,request,highContrast,appearance,background,{},0,0,&palette);
    Require(green(contrast)==0&&(contrast[24*240]>>24)>0,"high-contrast battery kept fixed green fill or omitted its system foreground outline");
    charging=false;
    percent=80;const auto limited=capture();
    Require(!Node(model.View(),"battery").charging&&!Node(model.View(),"battery").positiveGlyph&&Node(model.View(),"battery").pluggedIn&&limited!=full,"AC charge limit was mislabeled as full/charging");
    onAC=false;const auto unplugged=capture();
    Require(!Node(model.View(),"battery").pluggedIn&&unplugged!=limited,"unplugging at the same percentage left the power mark visible");
    known=false;model.Refresh();Require(!model.View().Find("battery"),
        "absent battery left a placeholder in the control center");
}

void CheckMediaPending(ID2D1Device* device,IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    auto state=std::make_shared<PreviewState>();auto source=FixtureSource(state);const auto media=source.media;
    bool playing=true;std::uint64_t task=0;source.media=[&]{auto snapshot=media();if(snapshot)for(auto& session:snapshot->sessions)session.playbackStatus=playing?"playing":"paused";return snapshot;};
    source.start=[&](system_control::Request action){Require(action.name=="media.toggle","media pending fixture dispatched an unrelated command");return ++task;};
    SystemPanelModel model(std::move(source),{},StatusBarAction::ControlCenter);
    const auto initial=Node(model.View(),"media.toggle");const auto title=Node(model.View(),"media.title");
    ui::Scene transport;transport.width=40;transport.height=40;auto button=initial;button.bounds={4,4,36,36};transport.nodes.push_back(button);
    request.canvasWidth=40;request.canvasHeight=40;request.dpi=96;request.transparent=request.contentOnly=true;
    ui::Input input;Require(input.Press(transport,{20,20}),"media icon did not accept its initial press");
    const auto idle=Render(device,text,request,transport,appearance,background,{},0,0);
    Require(Render(device,text,request,transport,appearance,background,{},0,0,nullptr,{},{},input.Pressed())!=idle,
        "media control lacked immediate pointer press feedback");
    Require(model.Invoke("media.toggle")&&task==1&&!model.Invoke("media.toggle"),"media pending did not guard repeated commands");
    const auto& pending=Node(model.View(),"media.toggle");const auto& pendingTitle=Node(model.View(),"media.title");
    Require(pending.glyph==initial.glyph&&pending.text.empty()&&pending.detail.empty()&&pending.tooltip==initial.tooltip&&!pending.busy&&
        pendingTitle.text==title.text&&pendingTitle.detail==title.detail&&pendingTitle.tooltip==title.tooltip,
        "media pending replaced its transport glyph or song/action text with working feedback");
    system_control::Completion done;done.id=task;done.ok=true;playing=false;state->completions.push_back(done);model.Refresh();
    Require(Node(model.View(),"media.toggle").enabled&&Node(model.View(),"media.toggle").glyph!=initial.glyph,
        "completed media command failed to restore input or show actual playback readback");
}

void CheckNumericAlignment(ID2D1Device* device,IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=120;request.canvasHeight=96;request.dpi=96;request.transparent=request.contentOnly=true;
    ui::Scene scene;scene.width=120;scene.height=96;const wchar_t* values[]{L"9%",L"42%",L"100%"};
    for(int row=0;row<3;++row)
    {
        ui::Node number;number.id="value:"+std::to_string(row);number.bounds={16,static_cast<float>(row*32),104,static_cast<float>(row*32+28)};
        number.text=values[row];number.trailing=true;scene.nodes.push_back(number);
    }
    const auto pixels=Render(device,text,request,scene,appearance,background,{},0,0);
    int left[3]{120,120,120},right[3]{-1,-1,-1};
    for(int row=0;row<3;++row)for(int y=row*32;y<row*32+28;++y)for(int x=0;x<120;++x)
        if((pixels[static_cast<std::size_t>(y)*120+x]>>24)>80){left[row]=(std::min)(left[row],x);right[row]=(std::max)(right[row],x);}
    Require(left[0]>left[2]&&right[0]>98&&std::abs(right[0]-right[1])<=1&&std::abs(right[1]-right[2])<=1,
        "percentage widths moved the numeric column's right edge");
    auto leading=scene;for(auto& number:leading.nodes)number.trailing=false;
    Require(!scene.SameContent(leading)&&Render(device,text,request,leading,appearance,background,{},0,0)!=pixels,
        "numeric alignment oracle or scene invalidation cannot distinguish the old leading-aligned values");
}

void CheckFocusModality(ID2D1Device* device, IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=240;request.canvasHeight=160;request.dpi=96;request.transparent=request.contentOnly=true;
    ui::Scene scene;scene.width=240;scene.height=160;
    ui::Node button;button.id="device";button.role=ui::Role::ListItem;button.bounds={16,16,224,72};button.text=L"Headphones";
    scene.nodes.push_back(button);button.id="connect";button.role=ui::Role::Button;button.bounds={16,80,224,136};button.text=L"Connect";
    scene.nodes.push_back(button);
    ui::Input input;
    const auto baseline=Render(device,text,request,scene,appearance,background,{},0,0);
    Require(input.Press(scene,{40,40}),"mouse focus fixture could not press its device row");
    const auto click=input.Release(scene,{40,40});input.Sync(scene);
    Require(click.kind==ui::InputResult::Kind::Invoke&&click.id=="device"&&input.Focused()=="device"&&input.VisibleFocus().empty(),
        "mouse selection lost logical focus or exposed a keyboard focus cue after refresh");
    Require(Render(device,text,request,scene,appearance,background,{},0,0,nullptr,input.VisibleFocus())==baseline,
        "pure mouse selection painted a keyboard focus outline");
    // A deliberately wrong logical-focus render must differ: this ensures the
    // pixel oracle would catch the original mouse-ring regression.
    Require(Render(device,text,request,scene,appearance,background,{},0,0,nullptr,input.Focused())!=baseline,
        "focus pixel oracle cannot distinguish the original mouse-ring regression");
    input.Key(scene,VK_TAB,false);
    Require(input.Focused()=="connect"&&input.VisibleFocus()=="connect"&&
        Render(device,text,request,scene,appearance,background,{},0,0,nullptr,input.VisibleFocus())!=baseline,
        "keyboard navigation did not display its focus outline");
    const auto activate=input.Key(scene,VK_RETURN,false);
    Require(activate.kind==ui::InputResult::Kind::Invoke&&activate.id=="connect"&&input.VisibleFocus()=="connect",
        "keyboard activation lost its target or visible cue");
    Require(input.PointerInput()&&input.Focused()=="connect"&&input.VisibleFocus().empty(),
        "pointer wheel/scroll input did not hide its cue while retaining logical focus");
    Require(input.Focus("device")&&input.VisibleFocus()=="device","assistive focus did not reveal its target");
    Require(!input.Press(scene,{2,2})&&input.Focused()=="device"&&input.VisibleFocus().empty(),
        "blank-area mouse input retained a keyboard cue or destroyed logical focus");
    Require(input.Focus("device"),"disabled focus fixture could not select its initial row");
    scene.nodes[0].enabled=false;input.Sync(scene);
    Require(input.Focused().empty()&&input.VisibleFocus().empty()&&input.Key(scene,VK_RETURN,false).kind==ui::InputResult::Kind::None,
        "a disabled target retained visible focus or keyboard activation");
}

void CheckDeviceCardHover(ID2D1Device* device,IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=440;request.canvasHeight=140;request.dpi=96;request.transparent=request.contentOnly=true;
    for(const auto* page:{"wifi","bluetooth"})
    {
        const bool wifi=std::string_view(page)=="wifi";auto state=std::make_shared<PreviewState>();state->emptyMedia=true;
        SystemPanelModel model(FixtureSource(state),{},StatusBarAction::ControlCenter);model.Select(page);
        const std::string suffix=wifi?"network-preview":"bluetooth-device-preview";
        const std::string row=std::string(page)+(wifi?".network:":".device:")+suffix;
        const std::string card=std::string(page)+".card:"+suffix,button=std::string(page)+".connect:"+suffix;
        Require(model.Invoke(row),"card hover fixture did not expand");
        ui::Scene scene;scene.width=440;scene.height=140;const float shift=10-Node(model.View(),card).bounds.top;
        for(const auto& node:model.View().nodes)if(node.hoverGroup==card)
        {
            auto copy=node;copy.bounds.top+=shift;copy.bounds.bottom+=shift;copy.clip={};scene.nodes.push_back(std::move(copy));
        }
        const auto* gap=scene.Hit({20,90},false);
        Require(gap&&gap->id==card,"expanded card padding lost its hover identity");
        ui::Input input;input.Sync(scene);Require(input.Focus(row)&&input.Focus(button)&&!input.Focus(card),
            "shared card hover changed row/button accessibility identities or made decoration focusable");
        if(!wifi)
        {
            const auto& action=Node(scene,button);const auto& bounds=Node(scene,card).bounds;
            Require(action.bounds.right==bounds.right-8&&action.bounds.right-action.bounds.left<=196&&action.bounds.left>bounds.left+48,
                "Bluetooth expanded action still spans the device card instead of aligning compactly right");
            const auto point=VisibleCenter(scene,button);Require(input.Press(scene,point)&&input.Release(scene,point).id==button,
                "compact Bluetooth child action lost its independent pointer target");
        }
        const auto idle=Render(device,text,request,scene,appearance,background,{},0,0);
        const auto overRow=Render(device,text,request,scene,appearance,background,{},0,0,nullptr,{},row);
        const auto overButton=Render(device,text,request,scene,appearance,background,{},0,0,nullptr,{},button);
        const auto overGap=Render(device,text,request,scene,appearance,background,{},0,0,nullptr,{},card);
        const std::size_t upper=30*440+20,lower=90*440+20;
        Require(overRow[lower]!=idle[lower]&&overRow[upper]==overRow[lower]&&
            overRow[lower]==overButton[lower]&&overRow[lower]==overGap[lower],
            "expanded device hover highlighted only its title row or lost highlight over its actions/padding");
        // Removing the production group must recreate the old title-only
        // result, so a weak pixel oracle cannot pass this regression.
        for(auto& node:scene.nodes)node.hoverGroup.clear();
        const auto old=Render(device,text,request,scene,appearance,background,{},0,0,nullptr,{},row);
        Require(old[lower]==idle[lower]&&old[lower]!=overRow[lower],"card hover oracle cannot distinguish the original partial highlight");
    }
}

void CheckSplitOpacity(ID2D1Device* device, IDWriteFactory* text,
    native_component_preview::Request request, const PersonalizationSettings& appearance,
    const SystemPanel::Background& background)
{
    // Independent pixel check for the known translucent seam overdraw. Omit
    // wallpaper/material so arbitrary user preview backgrounds cannot affect it.
    request.transparent = request.contentOnly = true;
    ui::Scene scene; scene.width = 200; scene.height = 80;
    ui::Node button; button.id = "split.main"; button.role = ui::Role::Button;
    button.bounds = {20,20,120,68}; button.joinRight = true;
    scene.nodes.push_back(button);
    button.id = "split.more"; button.bounds = {120,20,168,68};
    button.joinRight = false; button.joinLeft = true; scene.nodes.push_back(button);
    const auto pixels = Render(device,text,request,scene,appearance,background,{},0,0);
    const float scale = request.dpi/96.f;
    const auto sample = [&](float x) {
        return pixels[static_cast<std::size_t>(std::lround(32*scale))*request.canvasWidth +
            static_cast<std::size_t>(std::lround(x*scale))];
    };
    Require(sample(105) == sample(117) && sample(105) == sample(122) && sample(105) == sample(135),
        "translucent split control overdraw changed opacity at its inner join");
}

void CheckSelectedDetailContrast(ID2D1Device* device,IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=240;request.canvasHeight=232;request.dpi=96;request.transparent=request.contentOnly=true;
    ui::Scene scene;scene.width=240;scene.height=232;
    ui::Node button;button.id="accent";button.role=ui::Role::Button;button.bounds={8,8,232,72};button.text=L"Selected";button.detail=L"Readable detail";button.accent=true;
    scene.nodes.push_back(button);
    button.id="toggle";button.role=ui::Role::Toggle;button.bounds={8,80,232,144};button.accent=false;button.selected=true;scene.nodes.push_back(button);
    button.id="plain";button.role=ui::Role::Button;button.bounds={8,152,232,216};button.selected=false;scene.nodes.push_back(button);
    ui::Node thumb;thumb.id="decorative-thumb";thumb.role=ui::Role::Scrollbar;thumb.bounds={235,16,237,64};thumb.enabled=false;scene.nodes.push_back(thumb);
    const ui::Palette palette{D2D1::ColorF(0xffffff),D2D1::ColorF(0xff00ff),D2D1::ColorF(0x0000ff),
        D2D1::ColorF(0xffff00),D2D1::ColorF(0x202020),D2D1::ColorF(0x000000),D2D1::ColorF(0x00ff00)};
    const auto pixels=Render(device,text,request,scene,appearance,background,{},0,0,&palette);
    Require(pixels[40*240+235]==0xffff00ffu,"scrollbar thumb lost its solid secondary foreground through disabled opacity");
    ui::Input input;input.Sync(scene);
    Require(!input.Focus(thumb.id)&&input.AccessibilityRegions().size()==3,"decorative scrollbar entered keyboard or accessibility control order");
    for(int row=0;row<3;++row)
    {
        unsigned accentInk=0,secondaryInk=0;
        for(int y=45+row*72;y<67+row*72;++y)for(int x=20;x<218;++x)
        {
            const auto pixel=pixels[static_cast<std::size_t>(y)*240+static_cast<std::size_t>(x)];
            const unsigned red=(pixel>>16)&255,green=(pixel>>8)&255,blue=pixel&255;
            if(red>80&&green>80&&std::abs(static_cast<int>(red)-static_cast<int>(green))<8)++accentInk;
            if(red>80&&blue>80&&green<16)++secondaryInk;
        }
        Require(row<2?accentInk>8&&secondaryInk==0:secondaryInk>8&&accentInk==0,
            "selected detail text did not use high-contrast accent foreground, or plain detail lost its secondary color");
    }
}

void CheckChartAndSwitchPixels(ID2D1Device* device,IDWriteFactory* text,
    native_component_preview::Request request,const PersonalizationSettings& appearance,const SystemPanel::Background& background)
{
    request.canvasWidth=240;request.canvasHeight=160;request.dpi=96;request.transparent=request.contentOnly=true;
    ui::Scene scene;scene.width=240;scene.height=160;
    for(int row=0;row<3;++row)
    {
        ui::Node chart;chart.id="trace:"+std::to_string(row);chart.role=ui::Role::Chart;chart.chartGrid=false;
        chart.bounds={8,8+row*24.f,232,32+row*24.f};chart.paths={{{12,16+row*24.f},{228,16+row*24.f}}};
        chart.dashedPaths={row==1};chart.fillPaths=row==2;scene.nodes.push_back(std::move(chart));
    }
    for(int on=0;on<2;++on)
    {
        ui::Node toggle;toggle.id="switch:"+std::to_string(on);toggle.role=ui::Role::Toggle;toggle.switchStyle=true;toggle.selected=on!=0;
        toggle.bounds={16+on*112.f,108,64+on*112.f,144};scene.nodes.push_back(std::move(toggle));
    }
    const auto pixels=Render(device,text,request,scene,appearance,background,{},0,0);
    const auto alpha=[&](int x,int y){return pixels[static_cast<std::size_t>(y)*240+static_cast<std::size_t>(x)]>>24;};
    unsigned solid=0,dash=0,gaps=0;
    for(int x=20;x<220;++x){if(alpha(x,16)>80)++solid;if(alpha(x,40)>80)++dash;else ++gaps;}
    Require(solid>190&&dash>30&&gaps>30,"resource upload and download rendered as the same stroke instead of real solid/dashed traces");
    Require(alpha(100,72)>0&&alpha(100,72)<80&&alpha(100,64)>80,"resource chart lost its light area fill or obscured its trace");
    Require(alpha(24,110)==0&&alpha(136,110)==0&&alpha(30,126)>80&&alpha(162,126)>80,
        "detail switches rendered as full tiles or omitted their state-positioned thumbs");
    scene.nodes[3].busy=true;scene.nodes[3].enabled=false;
    const auto busy=Render(device,text,request,scene,appearance,background,{},0,0);
    Require(!scene.nodes[3].selected&&busy[126*240+40]!=pixels[126*240+40],
        "pending compact switch lacked a visible progress cue or changed its true checked state");
    ui::Scene scan;scan.width=240;scan.height=160;ui::Node scanIcon;scanIcon.id="wifi.scan";scanIcon.role=ui::Role::Icon;
    scanIcon.bounds={84,108,120,144};scanIcon.glyph=L"\uE72C";scan.nodes.push_back(scanIcon);
    const auto readyScan=Render(device,text,request,scan,appearance,background,{},0,0);
    scan.nodes.front().busy=true;scan.nodes.front().enabled=false;
    const auto busyScan=Render(device,text,request,scan,appearance,background,{},0,0);
    Require(busyScan!=readyScan&&(busyScan[126*240+102]>>24)>80,
        "icon-only scan pending lost its visible progress cue");
    ui::Scene rounded;rounded.width=240;rounded.height=160;rounded.cards={{0,0,240,160}};
    ui::Node full;full.id="full-chart";full.role=ui::Role::Chart;full.bounds={0,0,240,90};full.fillPaths=true;full.paths={{{0,0},{240,0}}};rounded.nodes.push_back(std::move(full));
    auto roundedAppearance=appearance;roundedAppearance.cornerRadius=72;
    const auto cornerPixels=Render(device,text,request,rounded,roundedAppearance,background,{},0,0);
    Require(cornerPixels[20*240+10]==0&&(cornerPixels[20*240+120]>>24)>0,
        "full-width chart paint escaped the popup theme radius in the offline renderer");
}
}

native_component_preview::Result ExportSystemPanelPreview(const native_component_preview::Request& request,
    ID2D1Device* device, IDWriteFactory* text, const PersonalizationSettings& appearance,
    const SystemPanel::Background& background)
{
    native_component_preview::Result result; result.request = request; result.stage = "panel.request";
    try
    {
        const bool controls = request.component == "control-panel", trayPanel = request.component == "tray-panel";
        const bool resources = request.component == "resource-panel", calendarPanel = request.component == "calendar-panel";
        Require(controls || trayPanel || resources || calendarPanel, "unsupported native system panel preview");
        Require(device && text && background, "system panel preview requires initialized native graphics");
        CheckTooltipViewport();
        Require(request.appearance == "light" || request.appearance == "dark",
            "native panel offline previews support light and dark; live compositor blur is not captured");
        const float scale = static_cast<float>(request.dpi)/96.f;
        const float available = static_cast<float>(request.canvasHeight-2*request.padding)/scale;
        const float availableWidth = static_cast<float>(request.canvasWidth-2*request.padding)/scale;
        Require(scale > 0 && available >= 200, "system panel preview canvas is too short");
        widget_preview::Wallpaper stage;
        if (!request.backgroundImage.empty())
        {
            const auto image = widget_preview::LoadWallpaperImage(request.backgroundImage);
            Require(!image.pixels.empty(), "cannot decode system panel preview background");
            stage = widget_preview::GenerateWallpaper(image,request.canvasWidth,request.canvasHeight);
        }
        else if (!request.transparent && !request.contentOnly)
            stage = widget_preview::GenerateWallpaper(request.canvasWidth,request.canvasHeight,request.appearance == "light");
        if (request.transparent || request.contentOnly)
        { stage.width = request.canvasWidth; stage.height = request.canvasHeight; stage.pixels.resize(static_cast<std::size_t>(stage.width)*stage.height); }
        Require(!stage.pixels.empty(), "cannot create system panel preview background");
        const std::vector<std::string> presets = controls ?
            std::vector<std::string>{"overview","bluetooth-off","audio","brightness","wifi","bluetooth","media","power","unavailable",
                "audio-many","wifi-many","bluetooth-many","media-empty"} :
            trayPanel ? std::vector<std::string>{"grid","updated","manage","empty","connecting","unavailable"} :
            resources ? std::vector<std::string>{"cpu","memory","gpu","traffic","idle","warming","unavailable","gap","gpu-partial"} :
            std::vector<std::string>{"empty","agenda"};
        const auto trayFixture = trayPanel ? TrayFixture() : tray::Snapshot{};
        for (const auto& preset : presets)
        {
            result.stage = "panel.model."+preset;
            auto state = std::make_shared<PreviewState>(); StatusBarSettings settings;
            state->unavailable = controls && preset == "unavailable";
            state->bluetoothOn = preset != "bluetooth-off"; state->emptyMedia = preset == "media-empty";
            state->agenda = calendarPanel && preset == "agenda";
            state->manySection = preset.ends_with("-many") ? preset.substr(0,preset.size()-5) : "";
            if (resources) FillResourceFixture(*state,preset);
            if (trayPanel)
            {
                state->tray = trayFixture; settings.pinnedTrayItems = {"preview-0"}; settings.trayOrder = {"offline-app"};
                if (preset == "empty") state->tray.icons.resize(1);
                if (preset == "connecting") { state->tray.connected = false; state->tray.icons.clear(); }
                if (preset == "unavailable") state->tray.degraded = true;
                if (preset == "updated")
                {
                    state->tray.icons[1].tip = L"Snow Sync — updated";
                    state->tray.icons[1].pixels = state->tray.icons[4].pixels;
                    state->tray.icons[2].pixels.resize(3);
                }
            }
            const auto action = controls ? StatusBarAction::ControlCenter : trayPanel ? StatusBarAction::Tray :
                calendarPanel ? StatusBarAction::Calendar : preset == "memory" ? StatusBarAction::Memory :
                preset == "gpu" || preset == "gpu-partial" ? StatusBarAction::Gpu :
                preset == "traffic" ? StatusBarAction::Traffic : StatusBarAction::Cpu;
            SystemPanelModel model(FixtureSource(state),settings,action);std::string renderedHover;
            model.Refresh(available,availableWidth);
            if (controls)
            {
                model.Select(preset == "overview" || preset == "unavailable" || preset == "bluetooth-off" || preset == "media-empty" ? "" :
                    !state->manySection.empty() ? state->manySection : preset);
                model.Refresh(available,availableWidth); // Drain deterministic fixture scan completion before rendering.
                if(preset=="bluetooth"||preset=="bluetooth-many")
                {
                    const std::string row="bluetooth.device:bluetooth-device-preview";
                    ui::Input input;const auto point=VisibleCenter(model.View(),row);
                    Require(input.Press(model.View(),point),"Bluetooth visual fixture could not press its first device");
                    const auto expanded=input.Release(model.View(),point);
                    Require(expanded.kind==ui::InputResult::Kind::Invoke&&expanded.id==row&&model.Invoke(expanded.id),
                        "Bluetooth visual fixture did not expand through its production input action");
                    const auto& card=Node(model.View(),"bluetooth.card:bluetooth-device-preview");
                    const auto& button=Node(model.View(),"bluetooth.connect:bluetooth-device-preview");
                    Require(button.bounds.right==card.bounds.right-8&&button.bounds.right-button.bounds.left<=196&&
                        button.bounds.left>card.bounds.left+48&&button.hoverGroup==card.id,
                        "Bluetooth preview lost its compact right-aligned action or shared whole-card hover");
                    renderedHover=row;
                }
            }
            if (resources && preset == "gpu-partial")
            {
                Require(model.Invoke("gpu.select") && model.Invoke("gpu:gpu-preview-1"), "GPU fixture selection failed");
            }
            if (trayPanel && preset == "manage")
            {
                // Preserve the old preset name while exercising the replacement:
                // direct drag from the bar into the same compact overflow grid.
                const auto first = Node(model.View(),"tray:preview-1").bounds;
                Require(model.Drop("preview-0",{first.left,first.top}) && state->trayChanges == 1 &&
                    state->savedTraySettings.pinnedTrayItems.empty(), "tray drag did not unpin the fixture icon in one transaction");
            }
            if (calendarPanel)
            {
                state->requestedDates.clear();
                Require(model.Invoke("date:2026-09-27") && !model.View().Find("calendar.day") &&
                    Node(model.View(),"calendar.selected").text==L"2026-09-27"&&Node(model.View(),"date:2026-09-27").outlined&&
                    Node(model.View(),"date:2026-09-26").selected&&state->requestedDates.contains("2026-09-27")&&
                    state->requestedDates.contains("2026-09-28")&&state->requestedDates.contains("2026-09-29"),
                    "calendar selection changed today or failed to update its selected and nearby agenda");
                Require(model.Invoke("calendar.today") && Node(model.View(),"calendar.selected").text == L"2026-09-26"&&
                    Node(model.View(),"date:2026-09-26").selected&&Node(model.View(),"date:2026-09-26").outlined,
                    "calendar today did not restore the fixed fixture date");
            }
            const auto& scene = model.View(); CheckLayout(scene);
            if (calendarPanel) CheckCalendarNames(scene);
            if (calendarPanel)
                for (const auto& node : scene.nodes)
                    if (node.id.starts_with("date:"))
                        Require((node.bounds.top>=0&&node.bounds.bottom<=scene.height)||scene.Find("scrollbar"),
                            "a constrained calendar month has no scroll affordance");
            if (controls) CheckControls(model,state,preset);
            if (trayPanel)
            {
                Require(!scene.Find("tray:preview-7") && (preset == "manage" || !scene.Find("tray:preview-0")),
                    "tray overflow included a hidden or still-pinned icon");
                if (preset == "updated") Require(!Node(scene,"tray:preview-2").image,
                    "invalid tray pixels did not use the missing-image fallback");
            }
            if (resources)
            {
                const bool missing = preset == "warming" || preset == "unavailable" || preset == "gpu-partial";
                const auto& chart = Node(scene,"resource.chart");
                Require(chart.paths.size() == (missing ? 0u : preset == "gap" ? 3u : preset == "traffic" ? 2u : 1u),
                    "resource graph bridged unavailable samples or omitted a valid trace");
                Require(chart.bounds.left==0&&chart.bounds.top==0&&chart.bounds.right==scene.width&&chart.fillPaths&&
                    Node(scene,"resource.title").bounds.top>=chart.bounds.bottom&&chart.dashedPaths.size()==chart.paths.size(),
                    "resource chart lost its full-width top position, fill or per-segment channel style");
                if(preset=="traffic")Require(!chart.dashedPaths.front()&&chart.dashedPaths.back()&&
                    Node(scene,"resource.legend:1").dashedPaths==std::vector<bool>{true},"upload trace or its legend is not dashed");
                const auto value = Node(scene,"resource.card:0").text;
                Require((!missing || value == L"—") && (preset != "idle" || value == L"0%"),
                    "resource panel conflated a valid zero with unavailable data");
                Require(state->subscriptions.size() == 1, "resource preview subscribed to unrelated sampling topics");
            }
            const int width = static_cast<int>(std::ceil(scene.width*scale));
            const int height = static_cast<int>(std::ceil(scene.height*scale));
            Require(width+2*request.padding <= request.canvasWidth && height+2*request.padding <= request.canvasHeight,
                "preview canvas is too small for the native panel");
            const int left = (request.canvasWidth-width)/2, top = (request.canvasHeight-height)/2;
            result.stage = "panel.render."+preset;
            const auto pixels = Render(device,text,request,scene,appearance,background,stage,left,top,nullptr,{},renderedHover);
            const auto path = request.outputDirectory/(request.component+"-"+preset+".png");
            result.stage = "panel.png."+preset;
            if (!preview_png::Save(path,request.canvasWidth,request.canvasHeight,pixels,result.error)) return result;
            result.outputs.push_back({request.component,preset,path,false,false,false,false,false,false,
                static_cast<int>(std::lround(appearance.cornerRadius*scale)),width,height,left,top});
            result.stage = "panel.check."+preset;
            if (resources)
            {
                const auto unchanged = model.View(); model.Refresh(available);
                Require(model.View().SameContent(unchanged), "unchanged resource data invalidated the visible scene");
            }
            if (controls && (preset == "overview" || preset == "audio")) CheckMute(model,state,preset == "overview");
            if (controls && preset == "overview")
            {
                CheckClosedCallbacks();
                CheckControlPromptVisuals(request,result,appearance);
                CheckInlineControlForms(device,text,request,result,appearance,background,stage);
                CheckLogicalFocus();
                CheckModelScrolling();
                CheckPendingActions();
                CheckFeedbackLayouts();
                CheckFocusModality(device,text,request,appearance,background);
                CheckBatteryStates(device,text,request,appearance,background);
                CheckMediaPending(device,text,request,appearance,background);
                CheckNumericAlignment(device,text,request,appearance,background);
                CheckDeviceCardHover(device,text,request,appearance,background);
                CheckSplitOpacity(device,text,request,appearance,background);
                CheckSelectedDetailContrast(device,text,request,appearance,background);
                CheckControlInput(model,state,available);
                const float withMedia = model.View().height;
                state->emptyMedia = true; model.Refresh(available);
                Require(model.View().cards.size() == 1 && model.View().height <= withMedia && !model.View().Find("media.title"),
                    "ended media kept its card or reserved empty space");
            }
            if (trayPanel && preset == "grid") { CheckTrayInput(model.View());CheckTrayDropTargets(trayFixture); }
            if (calendarPanel && preset == "agenda") CheckCalendarResponsive();
            if (calendarPanel && preset == "agenda") {CheckTimePickerInput();CheckCalendarManagement();CheckCalendarSeriesManagement();CheckCalendarPageVisuals(device,text,request,result,appearance,background,stage);}
            if (resources && preset == "gpu")
                Require(model.Invoke("gpu.select") && model.Invoke("gpu:gpu-preview-1") &&
                    Node(model.View(),"resource.card:0").text == L"61%", "GPU selection did not switch reading and history");
            if(resources&&preset=="cpu")CheckChartAndSwitchPixels(device,text,request,appearance,background);
            model.Close(); const auto reads = state->reads; model.Refresh(available); model.Close();
            Require(state->subscriptions.empty() && state->closes == 1 && state->reads == reads,
                "closed native panel retained subscriptions or read its old source");
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
}
