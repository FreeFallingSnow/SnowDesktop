#include "pch.h"
#include "system_panel.h"
#include "winui_runtime.h"
#include "../tray_service.h"
#include "../widget_system_data_provider.h"
#include "../app/desktop_backdrop_compositor.h"
#include "../l10n.h"
#include <robuffer.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.System.h>
#include <cmath>
#include <map>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace m = x::Media;
namespace a = x::Automation;
namespace
{
winrt::Windows::UI::Color Color(float r, float g, float b, float alpha = 1)
{
    const auto byte = [](float value) { return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.f, 1.f) * 255)); };
    return {byte(alpha), byte(r), byte(g), byte(b)};
}
std::wstring DisplayName(const tray::Icon& icon)
{
    if (!icon.tip.empty()) return icon.tip;
    const auto separator = icon.application.find_last_of(L"\\/");
    return separator == std::wstring::npos ? icon.application : icon.application.substr(separator + 1);
}
}
struct SystemPanel::Impl
{
    SettingsChanged changed;
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
    c::StackPanel body{nullptr};
    c::TextBlock notice{nullptr};
    struct Row
    {
        tray::Icon icon;
        c::Button activate{nullptr};
        c::Image image{nullptr};
        c::TextBlock title{nullptr};
        bool pointer = false;
    };
    std::vector<std::shared_ptr<Row>> rows;
    std::uint64_t revision = 0;
    bool showing = false, rebuilding = false;
    explicit Impl(SettingsChanged callback) : changed(std::move(callback)) {}
    ~Impl()
    {
        Hide();
        runtime.Detach(); rows.clear(); body = nullptr; notice = nullptr; frame = nullptr; content = nullptr;
        if (window) DestroyWindow(window);
        window = nullptr;
        runtime.Shutdown();
    }
    c::TextBlock Text(const wchar_t* value)
    {
        c::TextBlock text; text.Text(value); text.TextWrapping(x::TextWrapping::Wrap); return text;
    }
    bool Ensure()
    {
        if (window) return true;
        if (!runtime.Initialize()) return false;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.lpfnWndProc = Procedure; cls.hInstance = GetModuleHandleW(nullptr);
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"SnowDesktop.SystemPanel";
        RegisterClassExW(&cls);
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName,
            _LW("statusBar.controlCenter"), WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, this);
        return window != nullptr;
    }
    void Geometry(const std::shared_ptr<Row>& row)
    {
        if (!tray || !row->activate.XamlRoot()) return;
        const auto rect = row->activate.TransformToVisual(content).TransformBounds({0, 0,
            static_cast<float>(row->activate.ActualWidth()), static_cast<float>(row->activate.ActualHeight())});
        const double scale = row->activate.XamlRoot().RasterizationScale();
        POINT origin{}; ClientToScreen(window, &origin);
        RECT screen{origin.x + static_cast<LONG>(rect.X * scale), origin.y + static_cast<LONG>(rect.Y * scale),
            origin.x + static_cast<LONG>((rect.X + rect.Width) * scale), origin.y + static_cast<LONG>((rect.Y + rect.Height) * scale)};
        tray->SetGeometry(row->icon.key, screen);
    }
    void Activate(const std::shared_ptr<Row>& row, tray::Activation activation)
    {
        if (!tray || !showing) return;
        Geometry(row);
        POINT point{}; GetCursorPos(&point);
        if (activation == tray::Activation::Keyboard || activation == tray::Activation::ContextKeyboard)
        {
            const auto rect = row->activate.TransformToVisual(content).TransformBounds({0, 0, 1, 1});
            point = {static_cast<LONG>(rect.X * row->activate.XamlRoot().RasterizationScale()),
                static_cast<LONG>(rect.Y * row->activate.XamlRoot().RasterizationScale())};
            ClientToScreen(window, &point);
        }
        if (!tray->Activate(row->icon.key, activation, point))
            notice.Text(_LW("statusBar.trayUnavailable"));
    }
    void Image(const std::shared_ptr<Row>& row)
    {
        if (row->icon.pixels.empty() || !row->icon.width || !row->icon.height) return;
        m::Imaging::WriteableBitmap bitmap(static_cast<int>(row->icon.width), static_cast<int>(row->icon.height));
        auto access = bitmap.PixelBuffer().as<::Windows::Storage::Streams::IBufferByteAccess>();
        BYTE* bytes = nullptr;
        winrt::check_hresult(access->Buffer(&bytes));
        std::memcpy(bytes, row->icon.pixels.data(), row->icon.pixels.size() * sizeof(std::uint32_t));
        bitmap.Invalidate(); row->image.Source(bitmap);
    }
    void SaveTray()
    {
        if (changed) changed(settings);
        RefreshTray(true);
    }
    void Move(const std::string& persistentKey, int delta)
    {
        if (persistentKey.empty()) return;
        std::vector<std::string> order;
        for (const auto& row : rows) if (!row->icon.persistentKey.empty()) order.push_back(row->icon.persistentKey);
        const auto found = std::find(order.begin(), order.end(), persistentKey);
        if (found == order.end()) return;
        const auto index = found - order.begin();
        const auto next = index + delta;
        if (next < 0 || next >= static_cast<std::ptrdiff_t>(order.size())) return;
        std::iter_swap(order.begin() + index, order.begin() + next);
        // Keep disconnected applications' identities so they retain their order
        // when they register again in this or a later Explorer generation.
        for (const auto& key : settings.trayOrder)
            if (std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
        settings.trayOrder = std::move(order); SaveTray();
    }
    void AddTrayRow(tray::Icon icon)
    {
        auto row = std::make_shared<Row>(); row->icon = std::move(icon);
        c::Grid grid; grid.ColumnSpacing(8);
        for (int i = 0; i < 4; ++i)
        {
            c::ColumnDefinition column;
            column.Width(i == 0 ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star) : x::GridLengthHelper::Auto());
            grid.ColumnDefinitions().Append(column);
        }
        row->activate = c::Button(); row->activate.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        c::StackPanel label; label.Orientation(c::Orientation::Horizontal); label.Spacing(10);
        row->image = c::Image(); row->image.Width(22); row->image.Height(22); Image(row);
        row->title = Text(DisplayName(row->icon).c_str()); row->title.MaxWidth(200);
        label.Children().Append(row->image); label.Children().Append(row->title);
        row->activate.Content(label);
        a::AutomationProperties::SetName(row->activate, DisplayName(row->icon));
        c::ToolTipService::SetToolTip(row->activate, winrt::box_value(row->icon.tip));
        const std::weak_ptr<Row> weak = row;
        row->activate.PointerPressed([this, weak](const auto&, const x::Input::PointerRoutedEventArgs& args) {
            const auto row = weak.lock(); if (!row) return;
            if (args.GetCurrentPoint(row->activate).Properties().IsLeftButtonPressed())
            { row->pointer = true; Activate(row, tray::Activation::LeftDown); }
        });
        row->activate.Click([this, weak](const auto&, const auto&) {
            const auto row = weak.lock(); if (!row) return;
            Activate(row, row->pointer ? tray::Activation::LeftUp : tray::Activation::Keyboard); row->pointer = false;
        });
        row->activate.DoubleTapped([this, weak](const auto&, const auto&) { if (const auto row = weak.lock()) Activate(row, tray::Activation::DoubleClick); });
        row->activate.RightTapped([this, weak](const auto&, const x::Input::RightTappedRoutedEventArgs& args) {
            const auto row = weak.lock(); if (!row) return;
            Activate(row, tray::Activation::RightDown); Activate(row, tray::Activation::RightUp); args.Handled(true);
        });
        row->activate.KeyDown([this, weak](const auto&, const x::Input::KeyRoutedEventArgs& args) {
            const auto row = weak.lock(); if (!row) return;
            if (args.Key() == winrt::Windows::System::VirtualKey::Application)
            { Activate(row, tray::Activation::ContextKeyboard); args.Handled(true); }
        });
        row->activate.PointerEntered([this, weak](const auto&, const auto&) { if (const auto row = weak.lock()) Activate(row, tray::Activation::Hover); });
        row->activate.PointerExited([this, weak](const auto&, const auto&) { if (const auto row = weak.lock()) Activate(row, tray::Activation::Leave); });
        grid.Children().Append(row->activate);
        c::CheckBox pin; pin.Content(winrt::box_value(_LW("statusBar.pin")));
        pin.IsChecked(std::find(settings.pinnedTrayItems.begin(), settings.pinnedTrayItems.end(), row->icon.persistentKey) != settings.pinnedTrayItems.end());
        pin.IsEnabled(!row->icon.persistentKey.empty()); c::Grid::SetColumn(pin, 1);
        a::AutomationProperties::SetName(pin, std::wstring(_LW("statusBar.pin")) + L" " + DisplayName(row->icon));
        pin.Click([this, weak](const auto& sender, const auto&) {
            const auto row = weak.lock(); if (!row) return;
            const auto pin = sender.template as<c::CheckBox>();
            if (rebuilding) return;
            std::erase(settings.pinnedTrayItems, row->icon.persistentKey);
            if (pin.IsChecked().Value()) settings.pinnedTrayItems.push_back(row->icon.persistentKey);
            SaveTray();
        });
        grid.Children().Append(pin);
        for (const int delta : {-1, 1})
        {
            c::Button move; move.Content(winrt::box_value(delta < 0 ? L"↑" : L"↓"));
            move.IsEnabled(!row->icon.persistentKey.empty()); c::Grid::SetColumn(move, delta < 0 ? 2 : 3);
            a::AutomationProperties::SetName(move, _LW(delta < 0 ? "statusBar.moveEarlier" : "statusBar.moveLater"));
            move.Click([this, weak, delta](const auto&, const auto&) { if (const auto row = weak.lock()) Move(row->icon.persistentKey, delta); });
            grid.Children().Append(move);
        }
        rows.push_back(row); body.Children().Append(grid);
    }
    void RefreshTray(bool force = false)
    {
        if (!tray || action != StatusBarAction::Tray || !body) return;
        auto state = tray->Current();
        if (!force && revision == state.revision) return;
        revision = state.revision;
        std::erase_if(state.icons, [](const auto& icon) { return (icon.state & NIS_HIDDEN) != 0; });
        const auto rank = [&](const auto& icon) { return std::find(settings.trayOrder.begin(), settings.trayOrder.end(), icon.persistentKey) - settings.trayOrder.begin(); };
        std::stable_sort(state.icons.begin(), state.icons.end(), [&](const auto& left, const auto& right) { return rank(left) < rank(right); });
        const bool same = rows.size() == state.icons.size() && std::equal(rows.begin(), rows.end(), state.icons.begin(),
            [](const auto& row, const auto& icon) { return row->icon.key == icon.key; });
        if (same && !force)
        {
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                rows[i]->icon = std::move(state.icons[i]); Image(rows[i]);
                rows[i]->title.Text(DisplayName(rows[i]->icon));
                a::AutomationProperties::SetName(rows[i]->activate, DisplayName(rows[i]->icon));
                c::ToolTipService::SetToolTip(rows[i]->activate, winrt::box_value(rows[i]->icon.tip));
            }
        }
        else
        {
            rebuilding = true; body.Children().Clear(); rows.clear();
            for (auto& icon : state.icons) AddTrayRow(std::move(icon));
            rebuilding = false;
        }
        notice.Text(state.connected && !state.degraded && !rows.empty() ? L"" : _LW("statusBar.trayUnavailable"));
    }
    void Build()
    {
        runtime.Detach(); rows.clear();
        frame = c::Border(); frame.Padding({12, 12, 12, 12});
        const double radius = appearance.cornerRadius;
        frame.CornerRadius({radius, radius, radius, radius});
        frame.RequestedTheme(appearance.contentTheme == 1 ? x::ElementTheme::Light : x::ElementTheme::Dark);
        HIGHCONTRASTW hc{sizeof(hc)}; SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
        const bool highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        if (highContrast)
        {
            const auto color = GetSysColor(COLOR_WINDOW);
            frame.Background(m::SolidColorBrush(Color(GetRValue(color) / 255.f, GetGValue(color) / 255.f, GetBValue(color) / 255.f)));
        }
        else if (appearance.panelGradient.enabled)
        {
            m::LinearGradientBrush brush;
            const auto line = ResolvePanelGradientLine(appearance.panelGradient, 480, 560);
            brush.StartPoint({static_cast<float>(line.x1 / 480), static_cast<float>(line.y1 / 560)});
            brush.EndPoint({static_cast<float>(line.x2 / 480), static_cast<float>(line.y2 / 560)});
            for (const auto& value : appearance.panelGradient.stops)
            {
                m::GradientStop stop; stop.Offset(value.position);
                stop.Color(Color(((value.color >> 16) & 255) / 255.f, ((value.color >> 8) & 255) / 255.f,
                    (value.color & 255) / 255.f, static_cast<float>(value.opacity) * appearance.widgetAlpha));
                brush.GradientStops().Append(stop);
            }
            frame.Background(brush);
        }
        else frame.Background(m::SolidColorBrush(Color(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha)));
        if (!highContrast)
        {
            frame.BorderBrush(m::SolidColorBrush(Color(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha)));
            const double width = appearance.widgetBorderWidth;
            frame.BorderThickness({width, width, width, width});
        }
        c::StackPanel root; root.Spacing(12);
        auto title = Text(_LW(action == StatusBarAction::Tray ? "statusBar.tray" : action == StatusBarAction::Calendar ? "statusBar.clock" : "statusBar.controlCenter"));
        title.FontSize(20); title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        root.Children().Append(title);
        body = c::StackPanel(); body.Spacing(6);
        notice = Text(L"");
        if (action == StatusBarAction::Calendar)
        {
            c::CalendarView calendar; calendar.SelectionMode(c::CalendarViewSelectionMode::Single);
            calendar.SetDisplayDate(winrt::clock::now()); root.Children().Append(calendar);
        }
        else if (action == StatusBarAction::Tray)
        {
            c::ScrollViewer scroll; scroll.MaxHeight(410); scroll.Content(body);
            scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
            root.Children().Append(scroll); root.Children().Append(notice);
            c::Button native; native.Content(winrt::box_value(_LW("statusBar.nativeTray")));
            native.Click([this](const auto&, const auto&) { auto service = tray; Hide(); if (service) service->OpenNativeTray(); });
            root.Children().Append(native); RefreshTray(true);
        }
        else
        {
            // Control sections are supplied by the shared system service in the
            // next batch. Keep this host independent of Explorer collection.
            root.Children().Append(body);
        }
        frame.Child(root); content = frame;
        if (!runtime.Attach(window, frame)) throw winrt::hresult_error(E_FAIL, runtime.LastError());
    }
    void Hide()
    {
        if (frame && frame.XamlRoot())
            try
            {
                for (const auto& popup : m::VisualTreeHelper::GetOpenPopupsForXamlRoot(frame.XamlRoot())) popup.IsOpen(false);
            }
            catch (...) {}
        showing = false;
        if (window)
        {
            KillTimer(window, 1); backdrop.HidePopupWindowPair(window); backdrop.SetPopupTopmost(false);
            SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
        }
        if (data) data->RemoveConsumer("systemPanel");
        data.reset(); tray.reset();
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
        try
        {
            self->runtime.HandleWindowMessage(message, wp, lp);
            if (message == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && self->showing) self->Hide();
            else if (message == WM_CLOSE || (message == WM_KEYDOWN && wp == VK_ESCAPE)) { self->Hide(); return 0; }
            else if (message == WM_TIMER && wp == 1 && self->showing) self->RefreshTray();
            else if (message == WM_DPICHANGED || message == WM_DISPLAYCHANGE) self->Hide();
        }
        catch (...) { self->Hide(); }
        return DefWindowProcW(window, message, wp, lp);
    }
};
SystemPanel::SystemPanel(SettingsChanged changed) : impl_(std::make_unique<Impl>(std::move(changed))) {}
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
    const int width = (std::min)(static_cast<int>(480 * scale), static_cast<int>(info.rcWork.right - info.rcWork.left));
    const int height = (std::min)(static_cast<int>((action == StatusBarAction::Calendar ? 430 : 560) * scale), static_cast<int>(info.rcWork.bottom - info.rcWork.top));
    int left = anchor.right - width, top = anchor.bottom;
    if (settings.position == DockPosition::Bottom) top = anchor.top - height;
    if (settings.position == DockPosition::Left) left = anchor.right;
    if (settings.position == DockPosition::Right) left = anchor.left - width;
    left = std::clamp(left, static_cast<int>(info.rcWork.left), static_cast<int>(info.rcWork.right) - width);
    top = std::clamp(top, static_cast<int>(info.rcWork.top), static_cast<int>(info.rcWork.bottom) - height);
    SetWindowLongPtrW(self.window, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    SetWindowPos(self.window, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
    try { self.Build(); } catch (...) { self.Hide(); return; }
    if (appearance.glassEnabled)
    {
        if (!self.backdrop.IsAvailable()) self.backdrop.InitializePopup(self.window, true, false);
        self.backdrop.Reattach(self.window); self.backdrop.BeginFrame(true);
        self.backdrop.AddPanel({0, 0, width, height}, appearance.cornerRadius * static_cast<float>(scale),
            appearance.glassBlurRadius * static_cast<float>(scale), reinterpret_cast<std::uintptr_t>(&self));
        self.backdrop.EndFrame(); self.backdrop.ShowPopupWindowPair(self.window);
    }
    else self.backdrop.Reset();
    self.showing = true;
    self.backdrop.SetPopupWindowPairZOrder(self.window, HWND_TOPMOST, true);
    ShowWindow(self.window, SW_SHOW); SetForegroundWindow(self.window); SetTimer(self.window, 1, 500, nullptr);
}
}
