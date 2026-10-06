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
#include "../quick_navigation_animation_rules.h"
#include "../animation_settings.h"
#include <cmath>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
struct SystemPanel::Impl
{
    SettingsChanged changed;
    std::function<bool(std::string_view, POINT)> dropOutside;
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
    DWORD contextProcess = 0;
    double regionRadius = -1;
    std::vector<RECT> cardBounds;
    UiAnimationScheduler* scheduler = nullptr;
    UiScheduleToken animationToken = 0;
    quick_navigation_animation_rules::State slide;
    // The panel is constructed before the lazy WinUI runtime is initialized.
    // Creating a XAML object here throws on the first status-bar click.
    m::TranslateTransform translation{nullptr};
    Impl(SettingsChanged callback, SystemCalendarActions dates, std::function<bool(std::string_view, POINT)> drop,
        UiAnimationScheduler* timing)
        : changed(std::move(callback)), dropOutside(std::move(drop)), calendarActions(std::move(dates)), scheduler(timing) {}
    ~Impl()
    {
        Hide();
        backdrop.Reset();
        runtime.Detach(); resources.reset(); trayView.reset(); controls.reset(); calendar.reset(); frame = nullptr; content = nullptr;
        translation = nullptr;
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
        if (activation == tray::Activation::RightDown || activation == tray::Activation::RightUp || activation == tray::Activation::ContextKeyboard)
            contextProcess = icon.identity.process;
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
        translation = m::TranslateTransform(); frame.RenderTransform(translation);
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
            actions.dropOutside = dropOutside;
            trayView = std::make_unique<SystemTrayView>(std::move(actions), settings, [this] { if (showing) Arrange(); });
            root.Children().Append(trayView->Root()); RefreshTray(true);
            frame.Padding({10, 10, 10, 10});
        }
        else if (IsSystemResourceAction(action))
        {
            resources = std::make_unique<SystemResourceView>(data, action, [this] { if (showing) Arrange(); });
            root.Children().Append(resources->Root());
        }
        else
        {
            controls = std::make_unique<SystemControlView>(data, settings, action, [this] { if (showing) Arrange(); });
            // Control pages own their body viewport and fixed header/footer.
            // Keep the scrollbar at the surface edge, outside content padding.
            controls->ApplyAppearance(appearance);
            frame.Background(nullptr); frame.BorderThickness({0, 0, 0, 0});
            frame.Padding({0, 0, 0, 0}); frame.Child(controls->Root()); content = frame;
            if (!runtime.Attach(window, frame)) throw winrt::hresult_error(E_FAIL, runtime.LastError());
            return;
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
        if (controls) controls->SetViewportHeight(availableHeight - 12);
        content.Measure({widthDip, static_cast<float>(availableHeight)});
        const int width = static_cast<int>(std::ceil(widthDip * scale));
        const int height = static_cast<int>(std::ceil((std::min)(availableHeight,
            (std::max)(trayView ? 36. : 128., static_cast<double>(content.DesiredSize().Height))) * scale));
        int left = action == StatusBarAction::Calendar ? (anchor.left + anchor.right - width) / 2 : anchor.right - width;
        int top = settings.position == DockPosition::Bottom ? anchor.top - height - static_cast<int>(6 * scale) : anchor.bottom + static_cast<int>(6 * scale);
        left = std::clamp(left, static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - width);
        top = std::clamp(top, static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - height);
        RECT previous{}; GetWindowRect(window, &previous);
        const double radius = appearance.cornerRadius * scale;
        // Arrange the measured tree now: the island's queued resize otherwise
        // leaves a media card's old bounds in the native region for one frame.
        content.Arrange({0, 0, static_cast<float>(width / scale), static_cast<float>(height / scale)});
        auto nextCards = controls ? controls->CardBounds(scale) : std::vector<RECT>{{0, 0, width, height}};
        const bool sameCards = nextCards.size() == cardBounds.size() && std::equal(nextCards.begin(), nextCards.end(), cardBounds.begin(),
            [](const auto& a, const auto& b) { return EqualRect(&a, &b) != FALSE; });
        if (showing && sameCards && regionRadius == radius && previous.left == left && previous.top == top && previous.right == left + width && previous.bottom == top + height) return;
        cardBounds = std::move(nextCards);
        SetWindowPos(window, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
        runtime.ResizeToClient();
        if (UpdateSystemPanelRegion(window, width, height, radius, 0, cardBounds)) regionRadius = radius;
        if (appearance.glassEnabled)
        {
            if (!backdrop.IsAvailable()) backdrop.InitializePopup(window, showing, false);
            backdrop.Reattach(window); backdrop.BeginFrame(true);
            for (std::size_t i = 0; i < cardBounds.size(); ++i)
                backdrop.AddPanel(cardBounds[i], appearance.cornerRadius * static_cast<float>(scale),
                    appearance.glassBlurRadius * static_cast<float>(scale), reinterpret_cast<std::uintptr_t>(this) + i);
            backdrop.EndFrame();
            if (showing) backdrop.ShowPopupWindowPair(window);
        }
        else backdrop.Reset();
        if (slide.IsAnimating()) ApplyAnimation();
    }
    void ApplyAnimation()
    {
        if (!frame || !window || !translation) return;
        RECT client{}; GetClientRect(window, &client);
        const float eased = quick_navigation_animation_rules::EaseInOutSmooth(slide.GetVisual().progress);
        const float y = (settings.position == DockPosition::Bottom ? 1.f : -1.f) * (1.f - eased) * client.bottom;
        const double scale = GetDpiForWindow(window) / 96.;
        translation.Y(y / scale);
        UpdateSystemPanelRegion(window, client.right, client.bottom, appearance.cornerRadius * scale,
            static_cast<int>(std::lround(y)), cardBounds);
        if (appearance.glassEnabled && backdrop.IsAvailable())
        {
            const LONG offset = static_cast<LONG>(std::lround(y));
            for (std::size_t i = 0; i < cardBounds.size(); ++i)
            {
                RECT visible = cardBounds[i]; OffsetRect(&visible, 0, offset);
                (void)backdrop.SetPanelTransform(reinterpret_cast<std::uintptr_t>(this) + i,
                    D2D1::Matrix4x4F::Translation(0, y, 0), visible);
            }
            backdrop.CommitVisualChanges();
        }
    }
    void Animate(bool opening)
    {
        if (scheduler) scheduler->Cancel(animationToken);
        animationToken = 0;
        if (!scheduler || animation::RuntimePopupEffect() == animation::NoEffect)
        { if (opening) { slide.ShowImmediately(); ApplyAnimation(); } else Hide(); return; }
        slide.Configure(quick_navigation_animation_rules::Effect::Fade, animation::RuntimeDurationScale());
        const auto now = static_cast<std::uint64_t>(UiAnimationScheduler::MonotonicMilliseconds());
        if (opening) { slide.ResetHidden(); slide.Open(now); }
        else { showing = false; frame.IsHitTestVisible(false); slide.Close(now); }
        ApplyAnimation();
        if (!slide.IsAnimating()) { if (!opening) Hide(); return; }
        animationToken = scheduler->StartAnimation(UiAnimationSurface::Popup, [this](double tick) {
            slide.Advance(static_cast<std::uint64_t>(tick)); ApplyAnimation();
            if (slide.IsAnimating()) return true;
            animationToken = 0;
            if (slide.IsHidden()) Hide();
            return false;
        });
    }
    void Hide(bool animate = false)
    {
        if (hiding) return;
        if (animate && showing) { Animate(false); return; }
        if (scheduler) scheduler->Cancel(animationToken);
        animationToken = 0; slide.ResetHidden();
        if (translation) translation.Y(0);
        hiding = true;
        showing = false;
        contextProcess = 0;
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
            if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && self->showing)
            {
                DWORD process = 0; GetWindowThreadProcessId(reinterpret_cast<HWND>(lp), &process);
                // The icon owner creates its own menu. Keep the overflow alive
                // while that process owns activation; unrelated activation closes it.
                if (!self->contextProcess || (process && process != self->contextProcess &&
                    reinterpret_cast<HWND>(lp) != self->owner)) self->Hide(true);
            }
            else if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) { self->Hide(true); return 0; }
            else if (message == WM_TIMER && wp == 1 && self->showing)
            {
                if (self->controls) { self->controls->Refresh(); self->Arrange(); }
                else if (self->calendar) { self->calendar->Refresh(); self->Arrange(); }
                else if (self->resources) self->resources->Refresh();
                else
                {
                    if (self->contextProcess)
                    {
                        const HWND foreground = GetForegroundWindow();
                        DWORD process = 0; GetWindowThreadProcessId(foreground, &process);
                        if (foreground != window && process && process != self->contextProcess && process != GetCurrentProcessId())
                        { self->Hide(); return 0; }
                    }
                    self->RefreshTray();
                }
            }
            else if (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE) self->Hide();
        }
        catch (...) { self->Hide(); }
        return DefWindowProcW(window, message, wp, lp);
    }
};
SystemPanel::SystemPanel(SettingsChanged changed, SystemCalendarActions calendar, std::function<bool(std::string_view, POINT)> dropOutside,
    UiAnimationScheduler* scheduler)
    : impl_(std::make_unique<Impl>(std::move(changed), std::move(calendar), std::move(dropOutside), scheduler)) {}
SystemPanel::~SystemPanel() = default;
void SystemPanel::Hide() { impl_->Hide(true); }
void SystemPanel::HideForMonitor(HMONITOR monitor) { if (impl_->monitor == monitor) impl_->Hide(); }
bool SystemPanel::PreTranslateMessage(MSG* message)
{
    return impl_->showing && (impl_->runtime.PreTranslateMessage(message) || impl_->runtime.ProcessTabNavigation(message));
}
bool SystemPanel::DropTrayIcon(std::string_view key, POINT screen)
{
    auto& self = *impl_;
    if (!self.showing || !self.trayView || !self.content.XamlRoot()) return false;
    ScreenToClient(self.window, &screen);
    const auto scale = self.content.XamlRoot().RasterizationScale();
    const auto point = self.content.TransformToVisual(self.trayView->Root()).TransformPoint(
        {static_cast<float>(screen.x / scale), static_cast<float>(screen.y / scale)});
    return self.trayView->Drop(key, point);
}
void SystemPanel::Show(StatusBarAction action, HWND owner, RECT anchor,
    const PersonalizationSettings& appearance, const StatusBarSettings& settings,
    std::shared_ptr<tray::Service> tray, std::shared_ptr<widget_runtime::WidgetSystemDataProvider> data)
{
    auto& self = *impl_;
    if (self.showing && self.action == action && self.owner == owner) { self.Hide(true); return; }
    self.Hide();
    if (!self.Ensure()) return;
    self.action = action; self.owner = owner; self.anchor = anchor; self.appearance = appearance;
    self.settings = settings; self.tray = std::move(tray); self.data = std::move(data);
    self.monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)}; if (!GetMonitorInfoW(self.monitor, &info)) return;
    const double scale = GetDpiForWindow(owner) / 96.;
    const int width = (std::min)(static_cast<int>((action == StatusBarAction::Calendar ? 520 : action == StatusBarAction::Tray ? 208 : 440) * scale), static_cast<int>(info.rcWork.right - info.rcWork.left));
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
    self.Animate(true);
    if (appearance.glassEnabled) self.backdrop.ShowPopupWindowPair(self.window);
    self.backdrop.SetPopupWindowPairZOrder(self.window, HWND_TOPMOST, true);
    ShowWindow(self.window, SW_SHOW); SetForegroundWindow(self.window); SetTimer(self.window, 1, 500, nullptr);
}
}
