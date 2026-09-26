#include "pch.h"
#include "system_panel_preview.h"
#include "system_panel_surface.h"
#include "system_calendar_view.h"
#include "system_control_view.h"
#include "system_tray_view.h"
#include "system_resource_view.h"
#include "system_resource_preview.h"
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include "../widget_system_control_data.h"
#include "../calendar_display.h"
#include <set>
#include "winui_runtime.h"
#include "../l10n.h"
#include "../data_paths.h"
#include "../preview_png_writer.h"
#include "../widget_preview_stage.h"
#include <robuffer.h>
#include <cmath>
#include <stdexcept>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Microsoft.UI.Xaml.Media.Animation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace
{
template<class Ready> void PumpUntil(Ready ready)
{
    const auto deadline = GetTickCount64() + 15000;
    while (!ready())
    {
        if (GetTickCount64() >= deadline) throw std::runtime_error("XAML preview operation timed out");
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) throw std::runtime_error("XAML preview closed");
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!ready()) MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
template<class Operation> auto Await(const Operation& operation)
{
    try { PumpUntil([&] { return operation.Status() != winrt::Windows::Foundation::AsyncStatus::Started; }); }
    catch (...) { operation.Cancel(); throw; }
    return operation.GetResults();
}
struct PreviewHost
{
    WinUiRuntime runtime;
    HWND window = nullptr;
    ~PreviewHost()
    {
        runtime.Detach();
        if (window) DestroyWindow(window);
        runtime.Shutdown();
    }
};
struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct ControlPreviewState
{
    bool unavailable = false;
    bool bluetoothOn = true;
    bool emptyMedia = false;
    std::string manySection;
    std::set<std::string> subscriptions;
    unsigned scans = 0;
};
SystemControlViewSource PreviewControls(std::shared_ptr<ControlPreviewState> state)
{
    SystemControlViewSource source;
    source.current = [state](std::string_view topic) -> std::optional<system_control::Snapshot> {
        auto value = widget_runtime::PreviewSystemControlData(topic, state->unavailable);
        if (topic == "bluetooth.devices" && !state->unavailable && !state->bluetoothOn)
        {
            value.object["radios"].array.front().object["enabled"] = system_control::json::Boolean(false);
            value.object["devices"].array.clear();
        }
        if (topic == "audio.output.volume") ParseJson(R"({"endpointId":"audio-output-preview","volume":0.42,"muted":false})", value);
        if (topic == "system.power.plans" && !state->unavailable)
        {
            value.object["onAC"] = system_control::json::Boolean(true);
            value.object["charging"] = system_control::json::Boolean(true);
        }
        if (!state->unavailable && !state->manySection.empty())
        {
            auto* items = topic == "audio.devices" && state->manySection == "audio" ? &value.object["devices"].array :
                topic == "bluetooth.devices" && state->manySection == "bluetooth" ? &value.object["devices"].array :
                topic == "network.wifi" && state->manySection == "wifi" ? &value.object["interfaces"].array.front().object["networks"].array : nullptr;
            if (items && !items->empty())
            {
                const char* label = state->manySection == "wifi" ? "ssid" : "name";
                items->front().object[label] = system_control::json::Text(state->manySection == "wifi" ?
                    "SnowDesktop Guest Network 5 GHz" : "SnowDesktop Studio — Wireless Headphones and Conference Audio Device");
                const auto first = items->front();
                for (int i = 1; i <= 12; ++i)
                {
                    auto item = first; item.object["id"] = system_control::json::Text(state->manySection + "-extra-" + std::to_string(i));
                    item.object[label] = system_control::json::Text("Snow " + state->manySection + " " + std::to_string(i));
                    item.object["connected"] = system_control::json::Boolean(false);
                    item.object["isDefault"] = system_control::json::Boolean(false);
                    if (state->manySection == "bluetooth") item.object["canConnect"] = system_control::json::Boolean(i % 2 == 0);
                    items->push_back(std::move(item));
                }
            }
        }
        return system_control::Snapshot{!state->unavailable, std::move(value), state->unavailable ? "unavailable" : "", 0, 1};
    };
    source.start = [state](system_control::Request request) -> std::uint64_t {
        if (request.name != "network.wifi.scan" || request.arguments.at("interfaceId") != "wifi-preview")
            throw std::runtime_error("offline control view unexpectedly dispatched a device mutation");
        return ++state->scans; // Only the explicit Wi-Fi detail page may request a scan.
    };
    source.completions = [] { return std::vector<system_control::Completion>{}; };
    source.subscribe = [state](std::string topic, std::chrono::milliseconds) { state->subscriptions.insert(std::move(topic)); };
    source.unsubscribe = [state](std::string_view topic) { state->subscriptions.erase(std::string(topic)); };
    source.close = [state] { state->subscriptions.clear(); };
    source.settings = [](const wchar_t*) { throw std::runtime_error("offline view must not launch Windows Settings"); };
    source.media = [state]() -> std::optional<widget_runtime::WidgetMediaSessionsDataSnapshot> {
        widget_runtime::WidgetMediaSessionsDataSnapshot result; result.available = !state->unavailable;
        if (!state->unavailable && !state->emptyMedia)
        {
            widget_runtime::WidgetMediaSessionDataSnapshot session;
            session.id = "media-preview"; session.sourceName = "Snow Music";
            session.title = _L("app.widget_preview.api.calendar_publish"); session.artist = "Snow Music";
            session.playbackStatus = "playing"; session.current = true;
            session.controls.canPrevious = session.controls.canNext = session.controls.canPlayPause = true;
            result.currentSessionId = session.id; result.sessions.push_back(std::move(session));
        }
        return result;
    };
    source.artwork = [] { return std::optional<widget_runtime::WidgetMediaArtworkDataSnapshot>{}; };
    return source;
}
tray::Snapshot PreviewTray()
{
    // Windows stock icons are fixture pixels, not a claim that these apps were
    // discovered. This path never constructs a tray::Service or injects a hook.
    tray::Snapshot snapshot; snapshot.connected = true; snapshot.revision = 1;
    const SHSTOCKICONID stock[]{SIID_SHIELD, SIID_WORLD, SIID_DRIVEFIXED, SIID_FOLDER,
        SIID_INFO, SIID_WARNING, SIID_APPLICATION, SIID_RECYCLER};
    const wchar_t* names[]{L"Snow Shield", L"Snow Sync", L"Snow Drive", L"Snow Notes",
        L"Snow Info", L"Snow Alerts", L"Snow Tools", L"Hidden application"};
    for (unsigned i = 0; i < std::size(stock); ++i)
    {
        SHSTOCKICONINFO info{sizeof(info)};
        winrt::check_hresult(SHGetStockIconInfo(stock[i], SHGSI_ICON | SHGSI_SMALLICON, &info));
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
        if (!bitmap.dc || !bitmap.bitmap || !bytes) winrt::throw_last_error();
        bitmap.old = SelectObject(bitmap.dc, bitmap.bitmap); std::memset(bytes, 0, 32 * 32 * 4);
        if (!DrawIconEx(bitmap.dc, 0, 0, info.hIcon, 32, 32, 0, nullptr, DI_NORMAL)) winrt::throw_last_error();
        GdiFlush();
        tray::Icon icon; icon.key = icon.persistentKey = "preview-" + std::to_string(i);
        icon.tip = names[i]; icon.width = icon.height = 32;
        icon.pixels.assign(static_cast<const std::uint32_t*>(bytes), static_cast<const std::uint32_t*>(bytes) + 1024);
        if (i == 7) icon.state = NIS_HIDDEN;
        snapshot.icons.push_back(std::move(icon));
    }
    return snapshot;
}
x::FrameworkElement FindPreviewElement(const x::DependencyObject& root, const wchar_t* id)
{
    if (const auto element = root.try_as<x::FrameworkElement>(); element &&
        x::Automation::AutomationProperties::GetAutomationId(element) == id) return element;
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(root); ++i)
        if (const auto child = FindPreviewElement(x::Media::VisualTreeHelper::GetChild(root, i), id)) return child;
    return nullptr;
}
template<class Type> Type FindPreviewType(const x::DependencyObject& root)
{
    if (const auto match = root.try_as<Type>()) return match;
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(root); ++i)
        if (const auto child = FindPreviewType<Type>(x::Media::VisualTreeHelper::GetChild(root, i))) return child;
    return nullptr;
}
void InvokePreviewElement(const x::FrameworkElement& element)
{
    if (!element) throw std::runtime_error("tray preview command is missing");
    auto peer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
    peer.GetPattern(x::Automation::Peers::PatternInterface::Invoke).as<x::Automation::Provider::IInvokeProvider>().Invoke();
}
struct CalendarPreviewState
{
    std::string today = "2026-09-26", requestedDate;
    bool secondaryEnabled = true;
};
std::string PreviewSecondaryDate(const std::string& date)
{
    calendar::DisplayPreferences preferences; preferences.enabled = true;
    const auto days = calendar::Annotate(date, date, preferences, Locale::Instance().GetEffectiveLanguage());
    if (days.empty() || !days.front().calendarAvailable || days.front().fullDate.empty())
        throw std::runtime_error("calendar preview secondary calendar fixture is unavailable");
    return days.front().fullDate;
}
void CheckCalendarSelection(const x::FrameworkElement& frame, SystemCalendarView& view,
    const std::shared_ptr<CalendarPreviewState>& state, bool populated)
{
    const auto month = FindPreviewType<x::Controls::CalendarView>(frame);
    const auto day = FindPreviewElement(frame, L"calendar.day").as<x::Controls::TextBlock>();
    const auto secondary = FindPreviewElement(frame, L"calendar.secondary").as<x::Controls::TextBlock>();
    const auto today = FindPreviewElement(frame, L"calendar.today");
    if (day.Text() != L"26" || month.CalendarItemBorderThickness().Left < 2 ||
        x::Controls::Grid::GetRow(today) != 0)
        throw std::runtime_error("calendar must show the selected day, a continuous outline and a header Today command");
    if (!populated)
    {
        if (secondary.Visibility() != x::Visibility::Collapsed)
            throw std::runtime_error("disabled secondary calendar must not reserve an empty label");
        return;
    }
    auto select = [&](int value) {
        winrt::Windows::Globalization::Calendar date; date.ChangeCalendarSystem(L"GregorianCalendar");
        date.Day(1); date.Year(2026); date.Month(9); date.Day(value); date.Hour(12);
        month.SelectedDates().Clear(); month.SelectedDates().Append(date.GetDateTime());
        PumpUntil([&] { return day.Text() == std::to_wstring(value); });
    };
    select(27);
    if (state->requestedDate != "2026-09-27" || secondary.Text() != winrt::to_hstring(PreviewSecondaryDate("2026-09-27")))
        throw std::runtime_error("calendar selection did not update agenda and configured secondary date");
    state->today = "2026-09-28"; view.Refresh();
    if (day.Text() != L"27") throw std::runtime_error("midnight refresh overwrote the user's selected date");
    InvokePreviewElement(today); PumpUntil([&] { return day.Text() == L"28"; });
    if (state->requestedDate != "2026-09-28") throw std::runtime_error("Today did not select the current local day");
    state->secondaryEnabled = false; view.Refresh();
    if (secondary.Visibility() != x::Visibility::Collapsed)
        throw std::runtime_error("calendar failed to apply a secondary-calendar preference change");
    state->today = "2026-09-26"; state->secondaryEnabled = true; select(27);
}
struct TrayPreviewState
{
    StatusBarSettings saved;
    unsigned changes = 0;
    std::vector<std::pair<std::string, tray::Activation>> activations;
};
SystemTrayActions PreviewTrayActions(std::shared_ptr<TrayPreviewState> state)
{
    SystemTrayActions actions;
    actions.activate = [state](const tray::Icon& icon, const auto&, tray::Activation activation) {
        state->activations.emplace_back(icon.key, activation); return true;
    };
    actions.changed = [state](const StatusBarSettings& settings) { state->saved = settings; ++state->changes; };
    return actions;
}
void CheckTrayUpdates(SystemTrayView& view, const tray::Snapshot& snapshot,
    const std::shared_ptr<TrayPreviewState>& state, unsigned& layoutChanges)
{
    auto root = view.Root(); root.UpdateLayout();
    if (FindPreviewElement(root, L"tray.icon.preview-0") || FindPreviewElement(root, L"tray.icon.preview-7"))
        throw std::runtime_error("overflow duplicates a pinned or hidden icon");
    const auto first = FindPreviewElement(root, L"tray.icon.preview-1");
    const auto image = FindPreviewType<x::Controls::Image>(first);
    const auto source = image.Source();
    const auto tip = x::Controls::ToolTipService::GetToolTip(first).as<x::Controls::ToolTip>();
    const auto tipContent = tip.Content();
    const auto before = layoutChanges;
    auto same = snapshot; ++same.revision; view.Refresh(same);
    if (first != FindPreviewElement(root, L"tray.icon.preview-1") || !source || source != image.Source() ||
        tipContent != tip.Content() || layoutChanges != before)
        throw std::runtime_error("unchanged tray event replaced the image, tooltip or layout");
    same.icons[1].pixels = same.icons[2].pixels; same.icons[1].tip = L"Snow Sync — updated";
    ++same.revision; view.Refresh(same);
    if (source == image.Source() || winrt::unbox_value<winrt::hstring>(tip.Content()) != same.icons[1].tip)
        throw std::runtime_error("tray ignored dynamic pixels or tooltip updates");
    same.icons[1].pixels.resize(3); ++same.revision; view.Refresh(same);
    if (image.Source()) throw std::runtime_error("invalid tray pixels retained the old image");
    view.Refresh(snapshot); root.UpdateLayout();
    InvokePreviewElement(first);
    PumpUntil([&] { return state->activations.size() == 1; });
    if (state->activations.front() != std::make_pair(std::string("preview-1"), tray::Activation::Keyboard))
        throw std::runtime_error("accessible tray invoke did not route to the application");
    if (FindPreviewElement(root, L"tray.organize") || FindPreviewElement(root, L"tray.native"))
        throw std::runtime_error("compact tray contains obsolete management controls");
    if (!view.Drop("preview-0", {0, 0}) || state->changes != 1 || !state->saved.pinnedTrayItems.empty())
        throw std::runtime_error("dropping a pinned icon into overflow failed to unpin it");
    root.UpdateLayout();
    if (!view.Drop("preview-1", {0, 0}) || state->changes != 2)
        throw std::runtime_error("tray reorder did not save exactly once");
    const auto& order = state->saved.trayOrder;
    if (std::find(order.begin(), order.end(), "preview-1") >= std::find(order.begin(), order.end(), "preview-0") ||
        std::find(order.begin(), order.end(), "offline-app") == order.end())
        throw std::runtime_error("tray reorder failed or discarded a disconnected identity");
    root.UpdateLayout();
    view.Close();
    if (view.Drop("preview-1", {0, 0})) throw std::runtime_error("closed tray accepted a drop");
    InvokePreviewElement(FindPreviewElement(root, L"tray.icon.preview-1"));
    // Drain queued automation invocations via a sentinel, not a fixed delay.
    bool drained = false;
    root.DispatcherQueue().TryEnqueue([&] { drained = true; }); PumpUntil([&] { return drained; });
    if (state->activations.size() != 1 || state->changes != 2)
        throw std::runtime_error("closed tray view still dispatched a retained control action");
}
void CheckMonthFits(const x::DependencyObject& element, const x::Controls::CalendarView& month = nullptr,
    bool* foundLastDay = nullptr)
{
    if (const auto calendar = element.try_as<x::Controls::CalendarView>())
    {
        bool found = false;
        for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(calendar); ++i)
            CheckMonthFits(x::Media::VisualTreeHelper::GetChild(calendar, i), calendar, &found);
        if (!found) throw std::runtime_error("calendar preview clipped the last day of the fixture month");
        return;
    }
    if (month)
        if (const auto day = element.try_as<x::Controls::CalendarViewDayItem>())
        {
            winrt::Windows::Globalization::Calendar date;
            date.ChangeCalendarSystem(L"GregorianCalendar"); date.SetDateTime(day.Date());
            if (date.Year() == 2026 && date.Month() == 9 && date.Day() == 30)
            {
                const auto bounds = day.TransformToVisual(month).TransformBounds(
                    {0, 0, static_cast<float>(day.ActualWidth()), static_cast<float>(day.ActualHeight())});
                *foundLastDay = bounds.Width > 0 && bounds.Height > 0 && bounds.Y >= 0 &&
                    bounds.Y + bounds.Height <= month.ActualHeight() + 1;
            }
        }
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(element); ++i)
        CheckMonthFits(x::Media::VisualTreeHelper::GetChild(element, i), month, foundLastDay);
}
// An offscreen RenderTargetBitmap must capture resting control states, not
// the first frame of compositor brush fades. Keep the actual stock templates
// and final state values; only complete their animation in this preview tree.
void RemovePreviewBrushTransitions(const x::DependencyObject& element)
{
    if (const auto presenter = element.try_as<x::Controls::ContentPresenter>()) presenter.BackgroundTransition(nullptr);
    if (const auto border = element.try_as<x::Controls::Border>()) border.BackgroundTransition(nullptr);
    if (const auto panel = element.try_as<x::Controls::Panel>()) panel.BackgroundTransition(nullptr);
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(element); ++i)
        RemovePreviewBrushTransitions(x::Media::VisualTreeHelper::GetChild(element, i));
}
void CompletePreviewAnimations(const x::DependencyObject& element)
{
    // Re-enter the same logical state after removing implicit brush fades.
    // Clearing BackgroundTransition alone does not end an already running
    // compositor brush transition. Do not toggle IsChecked or call Click:
    // those can dispatch real user actions from the production view.
    if (const auto toggle = element.try_as<x::Controls::Primitives::ToggleButton>())
    {
        const auto checked = toggle.IsChecked();
        const wchar_t* state = checked ? (checked.Value() ? L"Checked" : L"Normal") : L"Indeterminate";
        if (!toggle.IsEnabled()) state = checked && checked.Value() ? L"CheckedDisabled" : L"Disabled";
        x::VisualStateManager::GoToState(toggle, L"Pressed", false);
        x::VisualStateManager::GoToState(toggle, state, false);
    }
    if (const auto framework = element.try_as<x::FrameworkElement>())
        for (const auto& group : x::VisualStateManager::GetVisualStateGroups(framework))
            if (const auto state = group.CurrentState())
                if (const auto storyboard = state.Storyboard(); storyboard &&
                    storyboard.GetCurrentState() == x::Media::Animation::ClockState::Active)
                    storyboard.SkipToFill();
    for (int i = 0; i < x::Media::VisualTreeHelper::GetChildrenCount(element); ++i)
        CompletePreviewAnimations(x::Media::VisualTreeHelper::GetChild(element, i));
}
}
native_component_preview::Result ExportSystemPanelPreview(
    const native_component_preview::Request& request, PersonalizationSettings appearance)
{
    native_component_preview::Result result; result.request = request;
    try
    {
        result.stage = "panel.appearance";
        // RenderTargetBitmap captures XAML, not the compositor's desktop blur.
        // Keep that unsupported boundary explicit instead of exporting fake glass.
        if (appearance.glassEnabled || appearance.acrylicEnabled)
            throw std::runtime_error("system panel preview currently supports light/dark XAML surfaces; desktop blur requires separate compositor verification");
        if (request.contentOnly) { appearance.widgetAlpha = 0; appearance.widgetBorderAlpha = 0; }
        result.stage = "panel.locale";
        const auto languageDirectory = std::filesystem::path(GetExecutableDirectoryPath()) / L"lang";
        Locale::Instance().Init(languageDirectory.c_str());
        if (request.locale != "system" && !Locale::Instance().HasLanguage(request.locale))
            throw std::runtime_error("requested preview locale is not installed");
        Locale::Instance().SetLanguage(request.locale.c_str());
        ComScope com; winrt::check_hresult(com.result);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        PreviewHost host;
        result.stage = "panel.runtime";
        if (!host.runtime.Initialize()) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
        // An isolated render window, never a DesktopApp, AppBar or Shell hook.
        // RTB supports offscreen content provided it is attached and not collapsed.
        host.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
            L"STATIC", L"SnowDesktop offline panel render", WS_POPUP,
            -32000, -32000, 900, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!host.window) winrt::throw_last_error();
        ShowWindow(host.window, SW_SHOWNOACTIVATE);
        const bool controlPanel = request.component == "control-panel";
        const bool trayPanel = request.component == "tray-panel";
        const bool resourcePanel = request.component == "resource-panel";
        const auto controlState = std::make_shared<ControlPreviewState>();
        std::unique_ptr<SystemControlView> controls;
        std::vector<winrt::weak_ref<x::Controls::ListView>> releasedDeviceLists;
        unsigned layoutChanges = 0;
        if (controlPanel) controls = std::make_unique<SystemControlView>(PreviewControls(controlState),
            StatusBarSettings{}, StatusBarAction::ControlCenter, [&] { ++layoutChanges; });
        const std::vector<std::string> presets = controlPanel ?
            std::vector<std::string>{"overview", "bluetooth-off", "audio", "brightness", "wifi", "bluetooth", "media", "power", "unavailable",
                "audio-many", "wifi-many", "bluetooth-many", "media-empty"} :
            trayPanel ? std::vector<std::string>{"grid", "updated", "manage", "empty", "connecting", "unavailable"} :
            resourcePanel ? std::vector<std::string>{"cpu", "memory", "gpu", "traffic", "idle", "warming", "unavailable", "gap", "gpu-partial"} :
            std::vector<std::string>{"empty", "agenda"};
        for (const auto& preset : presets)
        {
            result.stage = "panel.tree";
            auto frame = CreateSystemPanelFrame(appearance);
            std::unique_ptr<SystemCalendarView> calendar;
            std::unique_ptr<SystemTrayView> trayView;
            std::unique_ptr<SystemResourceView> resources;
            std::shared_ptr<ResourcePreviewState> resourceState;
            auto trayState = std::make_shared<TrayPreviewState>();
            auto calendarState = std::make_shared<CalendarPreviewState>();
            tray::Snapshot traySnapshot;
            float widthDip = controlPanel || resourcePanel ? 440.f : trayPanel ? 208.f : 520.f;
            if (controls)
            {
                controlState->unavailable = preset == "unavailable";
                controlState->bluetoothOn = preset != "bluetooth-off";
                controlState->emptyMedia = preset == "media-empty";
                const bool many = preset.ends_with("-many");
                controlState->manySection = many ? preset.substr(0, preset.size() - 5) : "";
                const auto before = layoutChanges;
                controls->Select(preset == "overview" || preset == "unavailable" || preset == "bluetooth-off" || preset == "media-empty" ? "" :
                    many ? controlState->manySection : preset);
                if (layoutChanges == before) throw std::runtime_error("control page switch did not request immediate measurement");
                controls->ApplyAppearance(appearance);
                frame.Background(nullptr); frame.BorderThickness({0, 0, 0, 0});
                frame.Padding({0, 0, 0, 0}); frame.Child(controls->Root());
                controls->SetViewportHeight((request.canvasHeight - request.padding * 2) * 96. / request.dpi);
            }
            else if (trayPanel)
            {
                StatusBarSettings settings; settings.pinnedTrayItems = {"preview-0"}; settings.trayOrder = {"offline-app"};
                trayView = std::make_unique<SystemTrayView>(PreviewTrayActions(trayState), settings, [&] { ++layoutChanges; });
                traySnapshot = PreviewTray();
                if (preset == "empty") traySnapshot.icons.resize(1); // Only the pinned icon remains.
                if (preset == "connecting") { traySnapshot.connected = false; traySnapshot.icons.clear(); }
                if (preset == "unavailable") traySnapshot.degraded = true;
                if (preset == "updated")
                {
                    traySnapshot.icons[1].tip = L"Snow Sync — updated";
                    traySnapshot.icons[1].pixels = traySnapshot.icons[4].pixels;
                    traySnapshot.icons[2].pixels.resize(3); // Explicit missing-image fallback.
                }
                trayView->Refresh(traySnapshot); frame.Child(trayView->Root()); frame.Padding({10, 10, 10, 10});
            }
            else if (resourcePanel)
            {
                resourceState = ResourcePreviewFixture(preset);
                const auto action = preset == "memory" ? StatusBarAction::Memory : preset == "gpu" || preset == "gpu-partial" ?
                    StatusBarAction::Gpu : preset == "traffic" ? StatusBarAction::Traffic : StatusBarAction::Cpu;
                resources = std::make_unique<SystemResourceView>(ResourcePreviewSource(resourceState), action, [&] { ++layoutChanges; });
                frame.Child(resources->Root());
            }
            else
            {
            SystemCalendarActions actions;
            actions.today = [calendarState] { return calendarState->today; };
            actions.manage = [] {}; // Preview never opens settings or changes events.
            actions.secondaryDate = [calendarState, populated = preset == "agenda"](const std::string& date) {
                return populated && calendarState->secondaryEnabled ? PreviewSecondaryDate(date) : std::string{};
            };
            actions.events = [calendarState, populated = preset == "agenda"](const std::string& date) {
                calendarState->requestedDate = date;
                std::vector<calendar::CalendarEvent> events;
                if (!populated) return events;
                calendar::CalendarEvent first; first.id = "preview-brunch"; first.revision = 1;
                first.date = date; first.title = _L("app.widget_preview.api.calendar_review"); first.startMinutes = 630; first.endMinutes = 690;
                calendar::CalendarEvent second; second.id = "preview-weekend"; second.revision = 1;
                second.date = date; second.title = _L("app.widget_preview.api.calendar_publish"); second.allDay = true;
                events.push_back(std::move(first)); events.push_back(std::move(second)); return events;
            };
            calendar = std::make_unique<SystemCalendarView>(std::move(actions)); frame.Child(calendar->Root());
            }
            bool loaded = false;
            auto loadedEvent = frame.Loaded(winrt::auto_revoke, [&](const auto&, const auto&) { loaded = true; });
            if (!host.runtime.Attach(host.window, frame)) throw winrt::hresult_error(E_FAIL, host.runtime.LastError());
            PumpUntil([&] { return loaded; }); loadedEvent.revoke();
            if (calendar) CheckCalendarSelection(frame, *calendar, calendarState, preset == "agenda");
            if (controls)
            {
                controls->SetViewportHeight((request.canvasHeight - request.padding * 2) * 96. / request.dpi);
                const auto radio = FindPreviewElement(frame, L"control.radio.bluetooth").as<x::Controls::Primitives::ToggleButton>();
                if (radio.IsEnabled() == controlState->unavailable ||
                    (radio.IsEnabled() && radio.IsChecked().Value() != controlState->bluetoothOn))
                    throw std::runtime_error("Bluetooth off must remain enabled, and unavailable hardware must remain disabled");
                const auto detail = FindPreviewElement(frame, L"control.detail.bluetooth").as<x::Controls::Button>();
                if (static_cast<bool>(detail.Style()) != (controlState->bluetoothOn && !controlState->unavailable) ||
                    detail.Content().as<x::Controls::FontIcon>().Glyph() != L"\uE76C" ||
                    FindPreviewElement(frame, L"control.back").as<x::Controls::Button>().Content().as<x::Controls::FontIcon>().Glyph() != L"\uE76B")
                    throw std::runtime_error("control detail must share the active tile accent and use chevrons");
            }
            if (resourcePanel && preset == "gpu-partial")
            {
                FindPreviewElement(frame, L"resource.adapter").as<x::Controls::ComboBox>().SelectedIndex(1);
                frame.UpdateLayout();
            }
            if (trayPanel && preset == "manage")
            {
                frame.Measure({widthDip, 1000}); frame.UpdateLayout();
                if (!trayView->Drop("preview-0", {0, 0}))
                    throw std::runtime_error("tray drop fixture did not unpin the icon");
            }
            frame.Measure({widthDip, 1000});
            const float heightDip = std::ceil(frame.DesiredSize().Height);
            const double windowScale = GetDpiForWindow(host.window) / 96.;
            SetWindowPos(host.window, nullptr, 0, 0, static_cast<int>(std::ceil(widthDip * windowScale)),
                static_cast<int>(std::ceil(heightDip * windowScale)), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            host.runtime.ResizeToClient();
            // SiteBridge sizing is dispatched asynchronously. UpdateLayout by
            // itself can still arrange against the initial 900-pixel island.
            const double layoutHeight = std::ceil(heightDip * windowScale) / windowScale;
            PumpUntil([&] {
                frame.UpdateLayout();
                return std::abs(frame.ActualWidth() - widthDip) < .51 / windowScale &&
                    std::abs(frame.ActualHeight() - layoutHeight) < .51 / windowScale;
            });
            int width = static_cast<int>(std::ceil(frame.ActualWidth() * request.dpi / 96.));
            int height = static_cast<int>(std::ceil(frame.ActualHeight() * request.dpi / 96.));
            if (width + request.padding * 2 > request.canvasWidth || height + request.padding * 2 > request.canvasHeight)
                throw std::runtime_error("preview canvas is too small for the system panel");
            result.stage = "panel.bitmap";
            if (controlPanel || trayPanel)
            {
                RemovePreviewBrushTransitions(frame); CompletePreviewAnimations(frame); frame.UpdateLayout();
                const auto scroll = FindPreviewType<x::Controls::ScrollViewer>(frame.Child());
                if (scroll.ScrollableHeight() > 1 && (!controlPanel ||
                    (preset != "audio" && preset != "power" && !preset.ends_with("-many"))))
                    throw std::runtime_error("panel preview clipped commands in its short fixture: " + preset);
                if (controlPanel && preset.ends_with("-many") && scroll.ScrollableHeight() <= 20)
                    throw std::runtime_error("long device list did not create a bounded scroll viewport");
            }
            x::Media::Imaging::RenderTargetBitmap bitmap;
            // WinUI's island rasterizer applies the XamlRoot scale to these
            // dimensions. Convert from requested output pixels exactly once.
            const double rasterScale = frame.XamlRoot().RasterizationScale();
            const auto renderWidth = static_cast<int>(std::lround(width / rasterScale));
            const auto renderHeight = static_cast<int>(std::lround(height / rasterScale));
            Await(bitmap.RenderAsync(frame, renderWidth, renderHeight));
            const auto buffer = Await(bitmap.GetPixelsAsync());
            // Check the real visual tree as well as outer pixels. A valid PNG
            // and rounded frame must not mask missing dates inside the month.
            if (!controlPanel && !trayPanel && !resourcePanel) CheckMonthFits(frame);
            if (resourcePanel)
            {
                const auto trace = FindPreviewElement(frame, L"resource.primary").as<x::Shapes::Path>();
                const auto geometry = trace.Data().as<x::Media::PathGeometry>();
                const bool missing = preset == "warming" || preset == "unavailable" || preset == "gpu-partial";
                const auto expectedFigures = missing ? 0u : preset == "gap" ? 3u : 1u;
                if (geometry.Figures().Size() != expectedFigures)
                    throw std::runtime_error("resource graph bridged missing samples or omitted a valid trace: " + preset);
                const auto value = FindPreviewElement(frame, L"resource.value.0").as<x::Controls::TextBlock>();
                if ((missing && value.Text() != L"—") || (preset == "idle" && value.Text() != L"0%"))
                    throw std::runtime_error("resource panel conflated a valid zero with unavailable data");
                const auto chart = FindPreviewElement(frame, L"resource.chart");
                if (chart.ActualWidth() < 400 || chart.ActualHeight() != 128)
                    throw std::runtime_error("resource chart has invalid measured bounds");
                for (int i = 0; i < (preset == "memory" || preset == "gpu" || preset == "gpu-partial" || preset == "traffic" ? 4 : 2); ++i)
                {
                    const auto id = L"resource.value." + std::to_wstring(i);
                    const auto metric = FindPreviewElement(frame, id.c_str());
                    const auto bounds = metric.TransformToVisual(frame).TransformBounds({0, 0,
                        static_cast<float>(metric.ActualWidth()), static_cast<float>(metric.ActualHeight())});
                    const auto cardId = L"resource.card." + std::to_wstring(i);
                    const auto card = FindPreviewElement(frame, cardId.c_str());
                    const auto cardBounds = card.TransformToVisual(frame).TransformBounds({0, 0,
                        static_cast<float>(card.ActualWidth()), static_cast<float>(card.ActualHeight())});
                    // TextBlock's ActualWidth follows short text; it is not
                    // the width of the containing, evenly sized metric card.
                    if (cardBounds.Width < 180 || bounds.Width <= 0 || bounds.X < cardBounds.X ||
                        bounds.X + bounds.Width > cardBounds.X + cardBounds.Width + .5 ||
                        bounds.Y + bounds.Height > cardBounds.Y + cardBounds.Height + .5 ||
                        cardBounds.Y + cardBounds.Height > frame.ActualHeight() + .5)
                        throw std::runtime_error("resource metric bounds invalid: " + preset + "/" + std::to_string(i) +
                            " card=" + std::to_string(cardBounds.Width) + " text=" + std::to_string(bounds.Width) +
                            " bottom=" + std::to_string(cardBounds.Y + cardBounds.Height) +
                            " frame=" + std::to_string(frame.ActualHeight()));
                }
                resources->Refresh(); frame.UpdateLayout();
                if (trace.Data() != geometry) throw std::runtime_error("unchanged resource snapshot rebuilt its graph");
                if (resourceState->subscriptions.size() != 1)
                    throw std::runtime_error("resource panel subscribed to unrelated sampling topics");
            }
            if (trayPanel)
            {
                const auto notice = FindPreviewElement(frame, L"tray.notice").as<x::Controls::TextBlock>();
                const auto expected = preset == "empty" ? _LW("statusBar.trayEmpty") : preset == "connecting" ?
                    _LW("statusBar.trayConnecting") : preset == "unavailable" ? _LW("statusBar.trayUnavailable") : L"";
                if (notice.Text() != expected) throw std::runtime_error("tray preview conflated empty, connecting and restricted states");
                for (int i = 0; i < 8; ++i)
                {
                    const auto id = L"tray.icon.preview-" + std::to_wstring(i);
                    const auto element = FindPreviewElement(frame, id.c_str());
                    const bool present = preset != "empty" && preset != "connecting" && i != 7 && (i != 0 || preset == "manage");
                    if (static_cast<bool>(element) != present) throw std::runtime_error("tray preview has missing or duplicate icons");
                    if (!element) continue;
                    const auto bounds = element.TransformToVisual(frame).TransformBounds({0, 0,
                        static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight())});
                    if (bounds.Width < 35 || bounds.Height < 35 || bounds.X < 0 || bounds.Y < 0 ||
                        bounds.X + bounds.Width > frame.ActualWidth() || bounds.Y + bounds.Height > frame.ActualHeight())
                        throw std::runtime_error("tray preview clipped an icon target");
                }
            }
            // Allow one-pixel rounding at a fractional rasterization scale.
            // Reject a stale layout or missing pixels; retain the real size in
            // both the image and its metadata rather than stretching it.
            const int renderedWidth = bitmap.PixelWidth(), renderedHeight = bitmap.PixelHeight();
            if (renderedWidth <= 0 || renderedHeight <= 0 ||
                std::abs(renderedWidth - width) > 1 || std::abs(renderedHeight - height) > 1 ||
                buffer.Length() != static_cast<unsigned>(renderedWidth * renderedHeight * 4))
                throw std::runtime_error("panel bitmap mismatch: requested=" + std::to_string(width) + "x" +
                    std::to_string(height) + ", returned=" + std::to_string(renderedWidth) + "x" +
                    std::to_string(renderedHeight) + ", bytes=" + std::to_string(buffer.Length()));
            width = renderedWidth; height = renderedHeight;
            BYTE* bytes = nullptr;
            winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
            widget_preview::Wallpaper canvas;
            if (request.transparent || request.contentOnly)
                canvas = {request.canvasWidth, request.canvasHeight, std::vector<std::uint32_t>(static_cast<std::size_t>(request.canvasWidth) * request.canvasHeight)};
            else if (!request.backgroundImage.empty())
                canvas = widget_preview::GenerateWallpaper(widget_preview::LoadWallpaperImage(request.backgroundImage), request.canvasWidth, request.canvasHeight);
            else canvas = widget_preview::GenerateWallpaper(request.canvasWidth, request.canvasHeight, appearance.contentTheme == 1);
            const int left = (canvas.width - width) / 2, top = (canvas.height - height) / 2;
            const auto* pixels = reinterpret_cast<const std::uint32_t*>(bytes);
            if (controlPanel)
            {
                const auto bounds = controls->CardBounds(width / frame.ActualWidth());
                const bool media = preset != "media" && preset != "unavailable" && preset != "media-empty";
                if (bounds.size() != (media ? 2u : 1u)) throw std::runtime_error("media card duplicated or left an empty placeholder");
                if (media)
                {
                    const int y = (bounds[0].bottom + bounds[1].top) / 2;
                    if (bounds[1].top - bounds[0].bottom < 6 || (pixels[y * width + width / 2] >> 24) != 0)
                        throw std::runtime_error("media card must have a transparent gap below the controls");
                    // The same native region function used by the live popup
                    // must exclude that gap, including during its slide.
                    for (const int offset : {0, -12, 12})
                    {
                        if (!UpdateSystemPanelRegion(host.window, width, height, appearance.cornerRadius * width / frame.ActualWidth(), offset, bounds))
                            throw std::runtime_error("cannot set the production multi-card region");
                        const auto region = CreateRectRgn(0, 0, 0, 0);
                        if (!region) winrt::throw_last_error();
                        const auto kind = GetWindowRgn(host.window, region);
                        const bool gap = PtInRegion(region, width / 2, y + offset) != FALSE;
                        const bool body = PtInRegion(region, width / 2, bounds[1].top + 20 + offset) != FALSE;
                        DeleteObject(region);
                        if (kind == ERROR || gap || !body) throw std::runtime_error("native media card region includes its gap or drops its body");
                    }
                }
                if (preset == "wifi" || preset == "wifi-many")
                {
                    const auto footer = FindPreviewElement(frame, L"control.footer.network.wifi");
                    const auto scan = FindPreviewElement(frame, L"control.wifi.scan");
                    const auto button = scan.TransformToVisual(footer).TransformBounds({0, 0,
                        static_cast<float>(scan.ActualWidth()), static_cast<float>(scan.ActualHeight())});
                    if (footer.ActualHeight() > 40 || button.X + button.Width < footer.ActualWidth() - 1)
                        throw std::runtime_error("Wi-Fi footer is stacked or scan is not right aligned");
                    const auto icon = scan.as<x::Controls::Button>().Content().as<x::Controls::FontIcon>();
                    const auto glyph = icon.TransformToVisual(scan).TransformBounds({0, 0,
                        static_cast<float>(icon.ActualWidth()), static_cast<float>(icon.ActualHeight())});
                    const auto padding = scan.as<x::Controls::Button>().Padding();
                    if (glyph.X < padding.Left || glyph.X + glyph.Width > scan.ActualWidth() - padding.Right + .5 ||
                        glyph.Width + .5 < icon.FontSize())
                        throw std::runtime_error("scan icon is clipped by its button padding");
                }
            }
            if (controlPanel && (preset == "audio" || preset == "wifi" || preset == "audio-many" || preset == "wifi-many" || preset == "bluetooth-many"))
            {
                const auto label = FindPreviewElement(frame, preset.starts_with("audio") ? L"control.audio.label.audio-output-preview" :
                    preset.starts_with("wifi") ? L"control.wifi.label.network-preview" : L"control.bluetooth.label.bluetooth-device-preview");
                if (!label) throw std::runtime_error("device row has no visible label");
                const auto rect = label.TransformToVisual(frame).TransformBounds({0, 0,
                    static_cast<float>(label.ActualWidth()), static_cast<float>(label.ActualHeight())});
                const double scale = width / frame.ActualWidth();
                if (rect.Width <= 40 || rect.X < 0 || rect.X + rect.Width > frame.ActualWidth() - 8)
                    throw std::runtime_error("long device label escaped the panel content bounds");
                unsigned ink = 0;
                for (int y = std::max(0, static_cast<int>(std::floor(rect.Y * scale))); y < std::min(height, static_cast<int>(std::ceil((rect.Y + rect.Height) * scale))); ++y)
                    for (int col = std::max(0, static_cast<int>(std::floor(rect.X * scale))); col < std::min(width, static_cast<int>(std::ceil((rect.X + rect.Width) * scale))); ++col)
                    {
                        const auto pixel = pixels[y * width + col];
                        const auto brightness = ((pixel & 255) + ((pixel >> 8) & 255) + ((pixel >> 16) & 255)) / 3;
                        if ((pixel >> 24) >= 128 && (appearance.contentTheme == 1 ? brightness < 140 : brightness > 180)) ++ink;
                    }
                if (ink < 24 * scale * scale)
                    throw std::runtime_error("device label is invisible or still in its entrance animation: " + preset);
            }
            for (int y = 0; y < height; ++y) for (int col = 0; col < width; ++col)
            {
                const auto source = pixels[y * width + col];
                auto& destination = canvas.pixels[static_cast<std::size_t>(top + y) * canvas.width + left + col];
                const unsigned inverse = 255 - (source >> 24);
                std::uint32_t composed = 0;
                for (unsigned shift : {0u, 8u, 16u, 24u})
                    composed |= (std::min)(255u, ((source >> shift) & 255) + ((((destination >> shift) & 255) * inverse + 127) / 255)) << shift;
                destination = composed;
            }
            const auto path = request.outputDirectory / (request.component + "-" + preset + ".png");
            result.stage = "panel.png";
            if (!preview_png::Save(path, canvas.width, canvas.height, canvas.pixels, result.error)) return result;
            result.outputs.push_back({request.component, preset, path, false, false, false, false, false, false,
                static_cast<int>(std::lround(appearance.cornerRadius * request.dpi / 96.)), width, height, left, top});
            if (controls && preset == "overview")
            {
                const auto card = FindPreviewElement(frame, L"control.card.main");
                const auto before = layoutChanges;
                controlState->emptyMedia = true; controls->Refresh(); frame.UpdateLayout();
                if (FindPreviewElement(frame, L"control.card.media").Visibility() != x::Visibility::Collapsed ||
                    layoutChanges == before || FindPreviewElement(frame, L"control.card.main") != card)
                    throw std::runtime_error("ended media must collapse its card and resize without replacing the controls");
                controlState->emptyMedia = false; controls->Refresh();
            }
            if (trayView && preset == "grid") CheckTrayUpdates(*trayView, traySnapshot, trayState, layoutChanges);
            if (controls && (preset == "audio" || preset == "wifi"))
            {
                const auto list = FindPreviewElement(frame, preset == "audio" ? L"control.audio.devices.output" :
                    L"control.wifi.networks").try_as<x::Controls::ListView>();
                if (!list) throw std::runtime_error("device page omitted its selection list");
                releasedDeviceLists.push_back(winrt::make_weak(list));
            }
            if (resources)
            {
                if (preset == "gpu")
                {
                    FindPreviewElement(frame, L"resource.adapter").as<x::Controls::ComboBox>().SelectedIndex(1);
                    if (FindPreviewElement(frame, L"resource.value.0").as<x::Controls::TextBlock>().Text() != L"61%")
                        throw std::runtime_error("GPU selection did not switch its reading and history immediately");
                }
                resources->Close(); const auto reads = resourceState->reads; resources->Refresh();
                if (!resourceState->subscriptions.empty() || resourceState->closed != 1 || resourceState->reads != reads)
                    throw std::runtime_error("closed resource panel retained sampling demand or read its old source");
            }
            host.runtime.Detach();
            frame.Child(nullptr);
        }
        if (controls)
        {
            controls->Close();
            if (!controlState->subscriptions.empty() || controlState->scans != 2)
                throw std::runtime_error("offline control lifecycle left subscriptions or scanned outside the Wi-Fi page");
            controls.reset();
            // A list event must not capture the list itself. Drain deferred
            // XAML releases and verify production pages become unreachable.
            PumpUntil([&] {
                return std::none_of(releasedDeviceLists.begin(), releasedDeviceLists.end(),
                    [](const auto& list) { return static_cast<bool>(list.get()); });
            });
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const winrt::hresult_error& error) { result.error = winrt::to_string(error.message()); }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
}
