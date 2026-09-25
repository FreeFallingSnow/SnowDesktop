#include "pch.h"
#include "system_panel.h"
#include "system_control_view.h"
#include "system_tray_view.h"
#include "system_resource_view.h"
#include "system_panel_surface.h"
#include "winui_runtime.h"
#include "../tray_service.h"
#include "../widget_system_data_provider.h"
#include "../app/desktop_backdrop_compositor.h"
#include "../l10n.h"
#include <cmath>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
struct SystemPanel::Impl
{
    SettingsChanged changed;
    SystemCalendarActions calendarActions;
    WinUiRuntime runtime;
    DesktopBackdropCompositor backdrop;
    HWND window = nullptr, owner = nullptr;
    HMONITOR monitor = nullptr;
    RECT anchor{};
    StatusBarAction action = StatusBarAction::ControlCenter;
    StatusBarSettings settings;
    PersonalizationSettings appearance;
    std::shared_ptr<tray::Service> tray;
    std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data;
    x::FrameworkElement content{nullptr};
    c::Border frame{nullptr};
    std::unique_ptr<SystemControlView> controls;
    std::unique_ptr<SystemCalendarView> calendar;
    std::unique_ptr<SystemTrayView> trayView;
    std::unique_ptr<SystemResourceView> resources;
    std::uint64_t revision = 0;
    bool showing = false, hiding = false;
    double regionRadius = -1;
    Impl(SettingsChanged callback, SystemCalendarActions dates) : changed(std::move(callback)), calendarActions(std::move(dates)) {}
    ~Impl()
    {
        Hide();
        backdrop.Reset();
        runtime.Detach(); resources.reset(); trayView.reset(); controls.reset(); calendar.reset(); frame = nullptr; content = nullptr;
        if (window) DestroyWindow(window);
        window = nullptr;
        runtime.Shutdown();
    }
    bool Ensure()
    {
        if (window && IsWindow(window)) return true;
        window = nullptr;
        if (!runtime.Initialize()) return false;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"SnowDesktop.SystemPanel";
        RegisterClassExW(&cls);
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName,
            _LW("statusBar.controlCenter"), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, this);
        return window != nullptr;
    }
    bool ActivateTray(const tray::Icon& icon, const x::FrameworkElement& element, tray::Activation activation)
    {
        if (!tray || !showing || !element.XamlRoot()) return false;
        const auto rect = element.TransformToVisual(content).TransformBounds({0, 0,
            static_cast<float>(element.ActualWidth()), static_cast<float>(element.ActualHeight())});
        const double scale = element.XamlRoot().RasterizationScale();
        POINT origin{}; ClientToScreen(window, &origin);
        RECT screen{origin.x + static_cast<LONG>(rect.X * scale), origin.y + static_cast<LONG>(rect.Y * scale),
            origin.x + static_cast<LONG>((rect.X + rect.Width) * scale), origin.y + static_cast<LONG>((rect.Y + rect.Height) * scale)};
        tray->SetGeometry(icon.key, screen);
        POINT point{}; GetCursorPos(&point);
        if (activation == tray::Activation::Keyboard || activation == tray::Activation::ContextKeyboard)
            point = {screen.left, screen.top};
        const auto service = tray; // The app's callback may deactivate this popup.
        return service->Activate(icon.key, activation, point, {owner, window});
    }
    void RefreshTray(bool force = false)
    {
        if (!tray || !trayView) return;
        auto state = tray->Current();
        if (!force && revision == state.revision) return;
        revision = state.revision; trayView->Refresh(std::move(state));
    }
    void Build()
    {
        regionRadius = -1;
        controls.reset();
        calendar.reset();
        trayView.reset();
        resources.reset();
        frame = CreateSystemPanelFrame(appearance);
        c::StackPanel root; root.Spacing(12);
        if (action == StatusBarAction::Calendar)
        {
            calendar = std::make_unique<SystemCalendarView>(calendarActions, [this] { if (showing) Arrange(); });
            root.Children().Append(calendar->Root());
        }
        else if (action == StatusBarAction::Tray)
        {
            SystemTrayActions actions;
            actions.activate = [this](const auto& icon, const auto& element, auto activation) { return ActivateTray(icon, element, activation); };
            actions.changed = [this](const auto& value) { settings = value; if (changed) changed(value); };
            actions.native = [this] { auto service = tray; Hide(); if (service) service->OpenNativeTray(); };
            trayView = std::make_unique<SystemTrayView>(std::move(actions), settings, [this] { if (showing) Arrange(); });
            root.Children().Append(trayView->Root()); RefreshTray(true);
        }
        else if (IsSystemResourceAction(action))
        {
            resources = std::make_unique<SystemResourceView>(data, action, [this] { if (showing) Arrange(); });
            root.Children().Append(resources->Root());
        }
        else
        {
            controls = std::make_unique<SystemControlView>(data, settings, action, [this] { if (showing) Arrange(); });
            c::ScrollViewer scroll; scroll.MaxHeight(SystemControlViewportHeight); scroll.Content(controls->Root());
            scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
            root.Children().Append(scroll);
        }
        c::ScrollViewer viewport;
        viewport.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
        viewport.VerticalScrollBarVisibility(c::ScrollBarVisibility::Auto);
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        if (GetMonitorInfoW(monitor, &monitorInfo))
            viewport.MaxHeight((std::max)(80., (monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) * 96. / GetDpiForWindow(owner) - 24.));
        viewport.Content(root); frame.Child(viewport); content = frame;
        if (!runtime.Attach(window, frame)) throw winrt::hresult_error(E_FAIL, runtime.LastError());
    }
    void Arrange()
    {
        if (!content || !window) return;
        MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(monitor, &info)) return;
        const double scale = GetDpiForWindow(owner) / 96.;
        const double requested = action == StatusBarAction::Calendar ? 520 : trayView ? trayView->PreferredWidth() : 440;
        const double availableWidth = (info.rcWork.right - info.rcWork.left) / scale;
        const double availableHeight = (info.rcWork.bottom - info.rcWork.top) / scale;
        const float widthDip = static_cast<float>((std::min)(requested, availableWidth));
        content.Measure({widthDip, static_cast<float>(availableHeight)});
        const int width = static_cast<int>(std::ceil(widthDip * scale));
        const int height = static_cast<int>(std::ceil((std::min)(availableHeight,
            (std::max)(128., static_cast<double>(content.DesiredSize().Height))) * scale));
        int left = action == StatusBarAction::Calendar ? (anchor.left + anchor.right - width) / 2 : anchor.right - width;
        int top = settings.position == DockPosition::Bottom ? anchor.top - height - static_cast<int>(6 * scale) : anchor.bottom + static_cast<int>(6 * scale);
        left = std::clamp(left, static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - width);
        top = std::clamp(top, static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - height);
        RECT previous{}; GetWindowRect(window, &previous);
        const double radius = appearance.cornerRadius * scale;
        if (showing && regionRadius == radius && previous.left == left && previous.top == top && previous.right == left + width && previous.bottom == top + height) return;
        SetWindowPos(window, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
        runtime.ResizeToClient();
        if (UpdateSystemPanelRegion(window, width, height, radius)) regionRadius = radius;
        if (appearance.glassEnabled)
        {
            if (!backdrop.IsAvailable()) backdrop.InitializePopup(window, showing, false);
            backdrop.Reattach(window); backdrop.BeginFrame(true);
            backdrop.AddPanel({0, 0, width, height}, appearance.cornerRadius * static_cast<float>(scale),
                appearance.glassBlurRadius * static_cast<float>(scale), reinterpret_cast<std::uintptr_t>(this));
            backdrop.EndFrame();
            if (showing) backdrop.ShowPopupWindowPair(window);
        }
        else backdrop.Reset();
    }
    void Hide()
    {
        if (hiding) return;
        hiding = true;
        showing = false;
        try { if (controls) controls->Close(); } catch (...) {}
        try { if (calendar) calendar->Close(); } catch (...) {}
        try { if (trayView) trayView->Close(); } catch (...) {}
        try { if (resources) resources->Close(); } catch (...) {}
        // The visual tree still owns its event handlers until Build replaces
        // the page. Keep their controller alive while the popup is hidden.
        try
        {
            if (frame && frame.XamlRoot())
                for (const auto& popup : m::VisualTreeHelper::GetOpenPopupsForXamlRoot(frame.XamlRoot())) popup.IsOpen(false);
        }
        catch (...) {}
        if (window)
        {
            KillTimer(window, 1); backdrop.HidePopupWindowPair(window); backdrop.SetPopupTopmost(false);
            SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }
        if (data) data->RemoveConsumer("systemPanel");
        data.reset(); tray.reset();
        owner = nullptr;
        hiding = false;
    }
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        if (message == WM_DESTROY)
        {
            self->Hide();
            self->backdrop.Reset();
            self->runtime.Detach();
        }
        if (message == WM_NCDESTROY)
        {
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->window = nullptr;
            return DefWindowProcW(window, message, wp, lp);
        }
        try
        {
            self->runtime.HandleWindowMessage(message, wp, lp);
            if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && self->showing) self->Hide();
            else if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) { self->Hide(); return 0; }
            else if (message == WM_TIMER && wp == 1 && self->showing)
            {
                if (self->controls) { self->controls->Refresh(); self->Arrange(); }
                else if (self->calendar) { self->calendar->Refresh(); self->Arrange(); }
                else if (self->resources) self->resources->Refresh();
                else self->RefreshTray();
            }
            else if (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE) self->Hide();
        }
        catch (...) { self->Hide(); }
        return DefWindowProcW(window, message, wp, lp);
    }
};
SystemPanel::SystemPanel(SettingsChanged changed, SystemCalendarActions calendar) : impl_(std::make_unique<Impl>(std::move(changed), std::move(calendar))) {}
SystemPanel::~SystemPanel() = default;
void SystemPanel::Hide() { impl_->Hide(); }
void SystemPanel::HideForMonitor(HMONITOR monitor) { if (impl_->monitor == monitor) impl_->Hide(); }
bool SystemPanel::PreTranslateMessage(MSG* message)
{
    return impl_->showing && (impl_->runtime.PreTranslateMessage(message) || impl_->runtime.ProcessTabNavigation(message));
}
void SystemPanel::Show(StatusBarAction action, HWND owner, RECT anchor,
    const PersonalizationSettings& appearance, const StatusBarSettings& settings,
    std::shared_ptr<tray::Service> tray, std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data)
{
    auto& self = *impl_;
    if (self.showing && self.action == action && self.owner == owner) { self.Hide(); return; }
    self.Hide();
    if (!self.Ensure()) return;
    self.action = action; self.owner = owner; self.anchor = anchor; self.appearance = appearance;
    self.settings = settings; self.tray = std::move(tray); self.data = std::move(data);
    self.monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(self.monitor, &info)) return;
    const double scale = GetDpiForWindow(owner) / 96.;
    const int width = (std::min)(static_cast<int>((action == StatusBarAction::Calendar ? 520 : action == StatusBarAction::Tray ? 280 : 440) * scale), static_cast<int>(info.rcWork.right - info.rcWork.left));
    const int height = (std::min)(static_cast<int>((action == StatusBarAction::Calendar ? 500 : 560) * scale), static_cast<int>(info.rcWork.bottom - info.rcWork.top));
    int left = action == StatusBarAction::Calendar ? (anchor.left + anchor.right - width) / 2 : anchor.right - width;
    int top = anchor.bottom + static_cast<int>(6 * scale);
    if (settings.position == DockPosition::Bottom) top = anchor.top - height - static_cast<int>(6 * scale);
    left = std::clamp(left, static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - width);
    top = std::clamp(top, static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - height);
    // This application-level popup outlives individual monitor AppBars. An
    // HWND owner would destroy its Island implicitly when that bar is removed.
    SetWindowPos(self.window, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
    try { self.Build(); } catch (...) { self.Hide(); return; }
    self.Arrange();
    self.showing = true;
    if (appearance.glassEnabled) self.backdrop.ShowPopupWindowPair(self.window);
    self.backdrop.SetPopupWindowPairZOrder(self.window, HWND_TOPMOST, true);
    ShowWindow(self.window, SW_SHOW); SetForegroundWindow(self.window); SetTimer(self.window, 1, 500, nullptr);
}
}
