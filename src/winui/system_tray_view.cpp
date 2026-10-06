#include "pch.h"
#include "system_tray_view.h"
#include "../l10n.h"
#include "../tray_order.h"
#include "../tray_presentation.h"
#include <cmath>
#include <robuffer.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>

namespace snowdesktop::winui
{
namespace x = winrt::Microsoft::UI::Xaml;
namespace c = x::Controls;
namespace a = x::Automation;
namespace
{
std::wstring DisplayName(const tray::Icon& icon)
{
    if (!icon.tip.empty()) return icon.tip;
    const auto separator = icon.application.find_last_of(L"\\/");
    auto name = separator == std::wstring::npos ? icon.application : icon.application.substr(separator + 1);
    return name.empty() ? _LW("statusBar.trayUnknown") : name;
}
void Subtle(const c::Button& button)
{
    button.Style(x::Application::Current().Resources().Lookup(winrt::box_value(L"SubtleButtonStyle")).as<x::Style>());
}
}
struct SystemTrayView::Impl : std::enable_shared_from_this<Impl>
{
    struct Row
    {
        tray::Icon icon;
        c::Grid container;
        c::Button activate;
        c::Image image;
        c::SymbolIcon fallback{c::Symbol::AllApps};
        c::ToolTip tip;
        winrt::Windows::Foundation::Point pressed{};
        bool pointer = false, dragging = false, releasedPointer = false;
    };
    SystemTrayActions actions;
    StatusBarSettings settings;
    std::function<void()> layoutChanged;
    tray::Snapshot snapshot;
    c::StackPanel root;
    c::Grid grid;
    c::TextBlock notice;
    std::vector<std::shared_ptr<Row>> rows;
    bool closed = false;

    Impl(SystemTrayActions source, StatusBarSettings value, std::function<void()> layout)
        : actions(std::move(source)), settings(std::move(value)), layoutChanged(std::move(layout)) {}
    bool Pinned(const tray::Icon& icon) const
    {
        return !icon.persistentKey.empty() && std::find(settings.pinnedTrayItems.begin(),
            settings.pinnedTrayItems.end(), icon.persistentKey) != settings.pinnedTrayItems.end();
    }
    void Build()
    {
        root.Spacing(8);
        c::ScrollViewer scroll; scroll.MaxHeight(400); scroll.Content(grid);
        scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(c::ScrollBarVisibility::Auto); root.Children().Append(scroll);
        notice.FontSize(13); notice.TextWrapping(x::TextWrapping::Wrap);
        a::AutomationProperties::SetAutomationId(notice, L"tray.notice"); root.Children().Append(notice);
        Present();
    }
    void Notice(const wchar_t* value)
    {
        if (notice.Text() == value) return;
        notice.Text(value); notice.Visibility(*value ? x::Visibility::Visible : x::Visibility::Collapsed);
        if (layoutChanged) layoutChanged();
    }
    void Activate(const std::shared_ptr<Row>& row, tray::Activation activation)
    {
        if (closed || !actions.activate) return;
        const auto callback = actions.activate;
        if (!callback(row->icon, row->activate, activation) && !closed &&
            activation != tray::Activation::Hover && activation != tray::Activation::Leave)
            Notice(_LW("statusBar.trayUnavailable"));
    }
    void Update(const std::shared_ptr<Row>& row, const tray::Icon& icon, bool initial)
    {
        if (initial || row->icon.width != icon.width || row->icon.height != icon.height || row->icon.pixels != icon.pixels)
        {
            // A malformed or removed bitmap must clear the previous image, and
            // never copy untrusted pixel bytes past the WriteableBitmap buffer.
            const auto count = static_cast<std::size_t>(icon.width) * icon.height;
            const bool valid = icon.width && icon.height && icon.width <= tray::kIconSize &&
                icon.height <= tray::kIconSize && count == icon.pixels.size();
            row->image.Source(nullptr);
            row->fallback.Visibility(valid ? x::Visibility::Collapsed : x::Visibility::Visible);
            if (valid)
            {
                x::Media::Imaging::WriteableBitmap bitmap(static_cast<int>(icon.width), static_cast<int>(icon.height));
                auto buffer = bitmap.PixelBuffer();
                if (buffer.Length() != count * sizeof(std::uint32_t)) winrt::throw_hresult(E_UNEXPECTED);
                BYTE* bytes = nullptr;
                winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&bytes));
                std::memcpy(bytes, icon.pixels.data(), count * sizeof(std::uint32_t));
                bitmap.Invalidate(); row->image.Source(bitmap);
            }
        }
        const auto name = DisplayName(icon);
        if (initial || DisplayName(row->icon) != name)
        {
            row->tip.Content(winrt::box_value(name));
            a::AutomationProperties::SetName(row->activate, name);
        }
        row->icon = icon;
    }
    void Save()
    {
        if (closed) return;
        Present();
        // The owner may hide the view while handling a settings change.
        if (const auto callback = actions.changed) callback(settings);
    }
    bool Drop(std::string_view key, winrt::Windows::Foundation::Point point)
    {
        if (closed || point.X < 0 || point.Y < 0 || point.X > root.ActualWidth() || point.Y > root.ActualHeight()) return false;
        std::string before;
        for (const auto& row : rows)
        {
            const auto rect = row->activate.TransformToVisual(root).TransformBounds({0, 0, 36, 36});
            if (point.Y < rect.Y || (point.Y < rect.Y + rect.Height && point.X < rect.X + rect.Width / 2))
            { before = row->icon.key; break; }
        }
        if (!tray::PlaceIcon(settings, snapshot, key, false, before)) return false;
        Save(); return true;
    }
    std::shared_ptr<Row> Create(const tray::Icon& icon)
    {
        auto row = std::make_shared<Row>();
        row->image.Width(20); row->image.Height(20); row->fallback.Width(20); row->fallback.Height(20);
        c::Grid picture; picture.Width(20); picture.Height(20);
        picture.Children().Append(row->fallback); picture.Children().Append(row->image);
        Subtle(row->activate); row->activate.Width(36); row->activate.Height(36); row->activate.Padding({8, 8, 8, 8});
        a::AutomationProperties::SetAutomationId(row->activate, winrt::to_hstring("tray.icon." + icon.key));
        c::ToolTipService::SetToolTip(row->activate, row->tip);
        row->activate.Content(picture);
        row->container.Children().Append(row->activate);
        const auto weak = weak_from_this(); const std::weak_ptr<Row> item = row;
        row->activate.AddHandler(x::UIElement::PointerPressedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [weak, item](const auto&, const x::Input::PointerRoutedEventArgs& args) {
                const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
                if (args.GetCurrentPoint(row->activate).Properties().IsLeftButtonPressed())
                {
                    row->pointer = true; row->dragging = false; row->releasedPointer = false;
                    row->pressed = args.GetCurrentPoint(self->root).Position();
                    row->activate.CapturePointer(args.Pointer()); args.Handled(true);
                }
            })), true);
        row->activate.Click([weak, item](const auto&, const auto&) {
            const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
            if (!row->pointer && !std::exchange(row->releasedPointer, false)) self->Activate(row, tray::Activation::Keyboard);
        });
        row->activate.AddHandler(x::UIElement::PointerMovedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [weak, item](const auto&, const x::Input::PointerRoutedEventArgs& args) {
                const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row || !row->pointer) return;
                const auto point = args.GetCurrentPoint(self->root).Position();
                if (std::abs(point.X - row->pressed.X) >= 8 || std::abs(point.Y - row->pressed.Y) >= 8)
                { row->dragging = true; row->tip.IsOpen(false); row->activate.Opacity(.5); }
                args.Handled(true);
            })), true);
        row->activate.AddHandler(x::UIElement::PointerReleasedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [weak, item](const auto&, const x::Input::PointerRoutedEventArgs& args) {
                const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row || !row->pointer) return;
                row->pointer = false; row->releasedPointer = true; row->activate.Opacity(1);
                const bool dragged = std::exchange(row->dragging, false);
                row->activate.ReleasePointerCapture(args.Pointer()); args.Handled(true);
                row->activate.DispatcherQueue().TryEnqueue([item] {
                    if (const auto row = item.lock()) row->releasedPointer = false;
                });
                if (dragged)
                {
                    if (!self->Drop(row->icon.key, args.GetCurrentPoint(self->root).Position()) && self->actions.dropOutside)
                    { POINT screen{}; GetCursorPos(&screen); self->actions.dropOutside(row->icon.key, screen); }
                }
                else
                {
                    const auto point = args.GetCurrentPoint(row->activate).Position();
                    if (point.X >= 0 && point.Y >= 0 && point.X < row->activate.ActualWidth() && point.Y < row->activate.ActualHeight())
                    { self->Activate(row, tray::Activation::LeftDown); self->Activate(row, tray::Activation::LeftUp); }
                }
            })), true);
        row->activate.PointerCaptureLost([item](const auto&, const auto&) {
            if (const auto row = item.lock()) { row->pointer = row->dragging = false; row->activate.Opacity(1); }
        });
        row->activate.PointerCanceled([item](const auto&, const auto&) {
            if (const auto row = item.lock()) { row->pointer = row->dragging = false; row->activate.Opacity(1); }
        });
        row->activate.DoubleTapped([weak, item](const auto&, const auto&) {
            if (const auto self = weak.lock()) if (const auto row = item.lock()) self->Activate(row, tray::Activation::DoubleClick);
        });
        row->activate.RightTapped([weak, item](const auto&, const x::Input::RightTappedRoutedEventArgs& args) {
            const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
            self->Activate(row, tray::Activation::RightDown); self->Activate(row, tray::Activation::RightUp); args.Handled(true);
        });
        row->activate.KeyDown([weak, item](const auto&, const x::Input::KeyRoutedEventArgs& args) {
            const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
            if (args.Key() == winrt::Windows::System::VirtualKey::Application ||
                (args.Key() == winrt::Windows::System::VirtualKey::F10 && (GetKeyState(VK_SHIFT) & 0x8000)))
            { self->Activate(row, tray::Activation::ContextKeyboard); args.Handled(true); }
        });
        row->activate.PointerEntered([weak, item](const auto&, const auto&) {
            if (const auto self = weak.lock()) if (const auto row = item.lock()) self->Activate(row, tray::Activation::Hover);
        });
        row->activate.PointerExited([weak, item](const auto&, const auto&) {
            if (const auto row = item.lock())
            {
                if (!(GetKeyState(VK_LBUTTON) & 0x8000)) row->pointer = false;
                if (const auto self = weak.lock()) self->Activate(row, tray::Activation::Leave);
            }
        });
        Update(row, icon, true); return row;
    }
    void Present()
    {
        if (closed) return;
        auto icons = snapshot.icons;
        std::erase_if(icons, [&](const auto& icon) {
            return (icon.state & NIS_HIDDEN) || tray::DuplicatesControlCenter(icon) || Pinned(icon);
        });
        const auto rank = [&](const auto& icon) { return std::find(settings.trayOrder.begin(), settings.trayOrder.end(), icon.persistentKey) - settings.trayOrder.begin(); };
        std::stable_sort(icons.begin(), icons.end(), [&](const auto& left, const auto& right) { return rank(left) < rank(right); });
        std::vector<std::shared_ptr<Row>> next;
        for (const auto& icon : icons)
        {
            auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row->icon.key == icon.key; });
            if (found == rows.end()) next.push_back(Create(icon));
            else { Update(*found, icon, false); next.push_back(*found); }
        }
        const bool structureChanged = rows != next;
        if (structureChanged)
            for (const auto& row : rows)
                if (std::find(next.begin(), next.end(), row) == next.end())
                { row->pointer = row->dragging = false; row->activate.ReleasePointerCaptures(); row->tip.IsOpen(false); }
        rows = std::move(next);
        if (structureChanged)
        {
            grid.Children().Clear(); grid.ColumnDefinitions().Clear(); grid.RowDefinitions().Clear();
            grid.ColumnSpacing(2); grid.RowSpacing(2);
            grid.HorizontalAlignment(x::HorizontalAlignment::Center);
            for (int i = 0; i < 5; ++i)
            {
                c::ColumnDefinition column; column.Width(x::GridLengthHelper::Auto());
                grid.ColumnDefinitions().Append(column);
            }
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const auto index = static_cast<int>(i), columns = 5;
                if (index % columns == 0) grid.RowDefinitions().Append(c::RowDefinition());
                c::Grid::SetRow(rows[i]->container, index / columns); c::Grid::SetColumn(rows[i]->container, index % columns);
                grid.Children().Append(rows[i]->container);
            }
        }
        Notice(snapshot.degraded ? _LW("statusBar.trayUnavailable") : !snapshot.connected ? _LW("statusBar.trayConnecting") : rows.empty() ? _LW("statusBar.trayEmpty") : L"");
        if (structureChanged && layoutChanged) layoutChanged();
    }
};
SystemTrayView::SystemTrayView(SystemTrayActions actions, StatusBarSettings settings, std::function<void()> layoutChanged)
    : impl_(std::make_shared<Impl>(std::move(actions), std::move(settings), std::move(layoutChanged))) { impl_->Build(); }
SystemTrayView::~SystemTrayView() { Close(); }
x::FrameworkElement SystemTrayView::Root() const { return impl_->root; }
double SystemTrayView::PreferredWidth() const { return 208; }
bool SystemTrayView::Drop(std::string_view key, winrt::Windows::Foundation::Point point) { return impl_->Drop(key, point); }
void SystemTrayView::Refresh(tray::Snapshot snapshot)
{
    if (impl_->closed) return;
    impl_->snapshot = std::move(snapshot); impl_->Present();
}
void SystemTrayView::Close()
{
    impl_->closed = true; impl_->actions = {}; impl_->layoutChanged = {};
    for (const auto& row : impl_->rows)
    {
        row->pointer = row->dragging = false;
        row->activate.ReleasePointerCaptures(); row->activate.Opacity(1); row->tip.IsOpen(false);
    }
}
}
