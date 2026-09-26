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
        if (!state->unavailable)
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
struct TrayPreviewState
{
    StatusBarSettings saved;
    unsigned changes = 0, native = 0;
    std::vector<std::pair<std::string, tray::Activation>> activations;
};
SystemTrayActions PreviewTrayActions(std::shared_ptr<TrayPreviewState> state)
{
    SystemTrayActions actions;
    actions.activate = [state](const tray::Icon& icon, const auto&, tray::Activation activation) {
        state->activations.emplace_back(icon.key, activation); return true;
    };
    actions.changed = [state](const StatusBarSettings& settings) { state->saved = settings; ++state->changes; };
    actions.native = [state] { ++state->native; };
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
    InvokePreviewElement(FindPreviewElement(root, L"tray.organize"));
    PumpUntil([&] { return view.PreferredWidth() == 440; }); root.UpdateLayout();
    auto pin = FindPreviewElement(root, L"tray.pin.preview-0");
    if (!pin || FindPreviewElement(root, L"tray.earlier.preview-0").as<x::Controls::Button>().IsEnabled())
        throw std::runtime_error("tray management omitted the pinned icon or enabled an out-of-range move");
    auto pinPeer = x::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(pin);
    pinPeer.GetPattern(x::Automation::Peers::PatternInterface::Toggle).as<x::Automation::Provider::IToggleProvider>().Toggle();
    PumpUntil([&] { return state->changes == 1; });
    if (!state->saved.pinnedTrayItems.empty()) throw std::runtime_error("tray pin control did not save its changed state");
    InvokePreviewElement(FindPreviewElement(root, L"tray.earlier.preview-1"));
    PumpUntil([&] { return state->changes == 2; });
    if (state->saved.trayOrder.size() != 8 || state->saved.trayOrder[0] != "preview-1" ||
        state->saved.trayOrder[1] != "preview-0" || state->saved.trayOrder.back() != "offline-app")
        throw std::runtime_error("tray reorder failed or discarded a disconnected identity");
    root.UpdateLayout();
    const auto native = FindPreviewElement(root, L"tray.native");
    InvokePreviewElement(native); PumpUntil([&] { return state->native == 1; });
    view.Close();
    InvokePreviewElement(native); InvokePreviewElement(FindPreviewElement(root, L"tray.icon.preview-1"));
    // Drain queued automation invocations via a sentinel, not a fixed delay.
    bool drained = false;
    root.DispatcherQueue().TryEnqueue([&] { drained = true; }); PumpUntil([&] { return drained; });
    if (state->native != 1 || state->activations.size() != 1)
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
        unsigned layoutChanges = 0;
        if (controlPanel) controls = std::make_unique<SystemControlView>(PreviewControls(controlState),
            StatusBarSettings{}, StatusBarAction::ControlCenter, [&] { ++layoutChanges; });
        const std::vector<std::string> presets = controlPanel ?
            std::vector<std::string>{"overview", "bluetooth-off", "audio", "brightness", "wifi", "bluetooth", "media", "power", "unavailable"} :
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
            tray::Snapshot traySnapshot;
            float widthDip = controlPanel || resourcePanel ? 440.f : trayPanel ? 280.f : 520.f;
            if (controls)
            {
                controlState->unavailable = preset == "unavailable";
                controlState->bluetoothOn = preset != "bluetooth-off";
                const auto before = layoutChanges;
                controls->Select(preset == "overview" || preset == "unavailable" || preset == "bluetooth-off" ? "" : preset);
                if (layoutChanges == before) throw std::runtime_error("control page switch did not request immediate measurement");
                x::Controls::ScrollViewer scroll; scroll.MaxHeight(SystemControlViewportHeight); scroll.Content(controls->Root());
                scroll.HorizontalScrollBarVisibility(x::Controls::ScrollBarVisibility::Disabled);
                frame.Child(scroll);
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
                trayView->Refresh(traySnapshot); frame.Child(trayView->Root());
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
            actions.today = [] { return std::string("2026-09-26"); };
            actions.manage = [] {}; // Preview never opens settings or changes events.
            actions.events = [populated = preset == "agenda"](const std::string& date) {
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
            if (controls)
            {
                const auto radio = FindPreviewElement(frame, L"control.radio.bluetooth").as<x::Controls::Primitives::ToggleButton>();
                if (radio.IsEnabled() == controlState->unavailable ||
                    (radio.IsEnabled() && radio.IsChecked().Value() != controlState->bluetoothOn))
                    throw std::runtime_error("Bluetooth off must remain enabled, and unavailable hardware must remain disabled");
            }
            if (resourcePanel && preset == "gpu-partial")
            {
                FindPreviewElement(frame, L"resource.adapter").as<x::Controls::ComboBox>().SelectedIndex(1);
                frame.UpdateLayout();
            }
            if (trayPanel && preset == "manage")
            {
                const auto before = layoutChanges;
                InvokePreviewElement(FindPreviewElement(frame, L"tray.organize"));
                PumpUntil([&] { return trayView->PreferredWidth() == 440; });
                if (before == layoutChanges) throw std::runtime_error("tray management did not request immediate measurement");
                widthDip = static_cast<float>(trayView->PreferredWidth());
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
                // These fixtures contain only one device/network per section.
                // All commands must fit; outer PNG bounds alone miss a clipped
                // settings button at the end of a short device list.
                const auto scroll = FindPreviewType<x::Controls::ScrollViewer>(frame.Child());
                if (scroll.ScrollableHeight() > 1)
                    throw std::runtime_error("panel preview clipped commands in its short fixture: " + preset);
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
                    if (bounds.Width < 39 || bounds.Height < 39 || bounds.X < 0 || bounds.Y < 0 ||
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
            if (trayView && preset == "grid") CheckTrayUpdates(*trayView, traySnapshot, trayState, layoutChanges);
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
            if (controls) frame.Child().as<x::Controls::ScrollViewer>().Content(nullptr);
            frame.Child(nullptr);
        }
        if (controls)
        {
            controls->Close();
            if (!controlState->subscriptions.empty() || controlState->scans != 1)
                throw std::runtime_error("offline control lifecycle left subscriptions or scanned outside the Wi-Fi page");
        }
        result.ok = true; result.stage = "complete";
    }
    catch (const winrt::hresult_error& error) { result.error = winrt::to_string(error.message()); }
    catch (const std::exception& error) { result.error = error.what(); }
    return result;
}
}
