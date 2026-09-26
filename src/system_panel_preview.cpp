#include "system_panel_preview.h"
#include "system_panel_model.h"
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
    unsigned scans = 0, closes = 0, reads = 0, trayChanges = 0;
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
        return ++state->scans;
    };
    source.completions = [state] { return std::exchange(state->completions, {}); };
    source.subscribe = [state](std::string topic, std::chrono::milliseconds) { state->subscriptions.insert(std::move(topic)); };
    source.unsubscribe = [state](std::string_view topic) { state->subscriptions.erase(std::string(topic)); };
    source.close = [state] { ++state->closes; state->subscriptions.clear(); };
    source.settings = [](const wchar_t*) { throw std::runtime_error("offline panel must not launch Windows Settings"); };
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
    source.calendar.events = [state](const std::string& date) {
        ++state->reads; state->requestedDate = date;
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
    // A native confirmation runs a nested message loop: closing or replacing
    // the panel during it must invalidate the old action before it can submit.
    auto state = std::make_shared<PreviewState>();
    auto source = FixtureSource(state);
    SystemPanelModel* active = nullptr;
    unsigned starts = 0, prompts = 0;
    source.start = [&](system_control::Request) { ++starts; return std::uint64_t{1}; };
    source.prompt = [&](system_control::Request&) { ++prompts; active->Close(); return true; };
    SystemPanelModel model(std::move(source), {}, StatusBarAction::ControlCenter);
    active = &model; model.Select("power");
    Require(!model.Invoke("power.shutdown") && prompts == 1 && starts == 0 && state->closes == 1,
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
        pending.tooltip.find(_LW("controlCenter.working"))!=std::wstring::npos&&pending.accessibilityLabel==_LW("statusBar.volume")&&
        !stableName.empty()&&name()==stableName&&stableName.find('%')==std::string::npos,
        "fractional volume wheel input lost its pending percentage or replaced the stable UIA name with transient feedback");
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
    Require(name(september)=="2026-09-01"&&name(october)=="2026-10-01",
        "same-number calendar days lack distinct full year/month/date accessibility names");
}

std::vector<std::uint32_t> Render(ID2D1Device* device, IDWriteFactory* text,
    const native_component_preview::Request& request, const ui::Scene& scene,
    const PersonalizationSettings& appearance, const SystemPanel::Background& background,
    const widget_preview::Wallpaper& stage, int left, int top,const ui::Palette* palette=nullptr)
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
    const auto contentResult = ui::Draw(context.Get(),text,scene,palette?*palette:SystemPanelPalette(appearance));
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
        Require(request.appearance == "light" || request.appearance == "dark",
            "native panel offline previews support light and dark; live compositor blur is not captured");
        const float scale = static_cast<float>(request.dpi)/96.f;
        const float available = static_cast<float>(request.canvasHeight-2*request.padding)/scale;
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
            SystemPanelModel model(FixtureSource(state),settings,action);
            model.Refresh(available);
            if (controls)
                model.Select(preset == "overview" || preset == "unavailable" || preset == "bluetooth-off" || preset == "media-empty" ? "" :
                    !state->manySection.empty() ? state->manySection : preset);
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
                Require(model.Invoke("date:2026-09-27") && Node(model.View(),"calendar.day").text == L"27" &&
                    state->requestedDate == "2026-09-27", "calendar selection did not update date and agenda together");
                Require(model.Invoke("calendar.today") && Node(model.View(),"calendar.day").text == L"26",
                    "calendar today did not restore the fixed fixture date");
            }
            const auto& scene = model.View(); CheckLayout(scene);
            if (calendarPanel) CheckCalendarNames(scene);
            if (calendarPanel)
                for (const auto& node : scene.nodes)
                    if (node.id.starts_with("date:"))
                        Require(node.bounds.top >= 0 && node.bounds.bottom <= scene.height,
                            "preview canvas clips the complete calendar month");
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
            const auto pixels = Render(device,text,request,scene,appearance,background,stage,left,top);
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
                CheckLogicalFocus();
                CheckModelScrolling();
                CheckSplitOpacity(device,text,request,appearance,background);
                CheckSelectedDetailContrast(device,text,request,appearance,background);
                CheckControlInput(model,state,available);
                const float withMedia = model.View().height;
                state->emptyMedia = true; model.Refresh(available);
                Require(model.View().cards.size() == 1 && model.View().height <= withMedia && !model.View().Find("media.title"),
                    "ended media kept its card or reserved empty space");
            }
            if (trayPanel && preset == "grid") CheckTrayInput(model.View());
            if (resources && preset == "gpu")
                Require(model.Invoke("gpu.select") && model.Invoke("gpu:gpu-preview-1") &&
                    Node(model.View(),"resource.card:0").text == L"61%", "GPU selection did not switch reading and history");
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
