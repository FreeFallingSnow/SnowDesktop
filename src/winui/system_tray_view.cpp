#include "pch.h"
#include "system_tray_view.h"
#include "../l10n.h"
#include <robuffer.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Windows.System.h>

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
        c::TextBlock title;
        c::CheckBox pin;
        c::Button earlier, later;
        bool pointer = false, updating = false;
    };
    SystemTrayActions actions;
    StatusBarSettings settings;
    std::function<void()> layoutChanged;
    tray::Snapshot snapshot;
    c::StackPanel root;
    c::Grid grid;
    c::TextBlock notice;
    c::Button organize;
    std::vector<std::shared_ptr<Row>> rows;
    bool managing = false, closed = false;

    Impl(SystemTrayActions source, StatusBarSettings value, std::function<void()> layout)
        : actions(std::move(source)), settings(std::move(value)), layoutChanged(std::move(layout)) {}
    bool Pinned(const tray::Icon& icon) const
    {
        return !icon.persistentKey.empty() && std::find(settings.pinnedTrayItems.begin(),
            settings.pinnedTrayItems.end(), icon.persistentKey) != settings.pinnedTrayItems.end();
    }
    void Build()
    {
        root.Spacing(12);
        c::Grid heading;
        c::ColumnDefinition titleColumn; titleColumn.Width(x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
        c::ColumnDefinition editColumn; editColumn.Width(x::GridLengthHelper::Auto());
        heading.ColumnDefinitions().Append(titleColumn); heading.ColumnDefinitions().Append(editColumn);
        c::TextBlock title; title.Text(_LW("statusBar.tray")); title.FontSize(16);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold()); title.VerticalAlignment(x::VerticalAlignment::Center);
        heading.Children().Append(title);
        Subtle(organize); organize.Width(32); organize.Height(32); organize.Padding({6, 6, 6, 6});
        a::AutomationProperties::SetAutomationId(organize, L"tray.organize");
        c::Grid::SetColumn(organize, 1); heading.Children().Append(organize);
        const auto weak = weak_from_this();
        organize.Click([weak](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed)
            {
                self->managing = !self->managing;
                self->rows.clear(); self->grid.Children().Clear();
                self->UpdateHeader(); self->Present();
                if (self->layoutChanged) self->layoutChanged();
            }
        });
        root.Children().Append(heading);
        c::ScrollViewer scroll; scroll.MaxHeight(400); scroll.Content(grid);
        scroll.HorizontalScrollBarVisibility(c::ScrollBarVisibility::Disabled);
        scroll.VerticalScrollBarVisibility(c::ScrollBarVisibility::Auto); root.Children().Append(scroll);
        notice.FontSize(13); notice.TextWrapping(x::TextWrapping::Wrap);
        a::AutomationProperties::SetAutomationId(notice, L"tray.notice"); root.Children().Append(notice);
        c::HyperlinkButton native; native.Content(winrt::box_value(_LW("statusBar.nativeTray")));
        native.FontSize(12); native.HorizontalAlignment(x::HorizontalAlignment::Stretch);
        native.Padding({0, 4, 0, 4}); a::AutomationProperties::SetAutomationId(native, L"tray.native");
        native.Click([weak](const auto&, const auto&) {
            if (const auto self = weak.lock(); self && !self->closed)
                if (const auto callback = self->actions.native) callback();
        });
        root.Children().Append(native); UpdateHeader(); Present();
    }
    void UpdateHeader()
    {
        const auto label = _LW(managing ? "statusBar.trayBack" : "statusBar.organizeTray");
        organize.Content(c::SymbolIcon(managing ? c::Symbol::Back : c::Symbol::Edit));
        a::AutomationProperties::SetName(organize, label);
        c::ToolTipService::SetToolTip(organize, winrt::box_value(label));
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
            row->title.Text(name); row->tip.Content(winrt::box_value(name));
            a::AutomationProperties::SetName(row->activate, name);
            a::AutomationProperties::SetName(row->pin, std::wstring(_LW("statusBar.pin")) + L" " + name);
        }
        row->icon = icon;
        row->updating = true;
        row->pin.IsChecked(Pinned(icon)); row->pin.IsEnabled(!icon.persistentKey.empty());
        row->updating = false;
    }
    void Save()
    {
        if (closed) return;
        Present();
        // The owner may hide the view while handling a settings change.
        if (const auto callback = actions.changed) callback(settings);
    }
    void Pin(const std::shared_ptr<Row>& row, bool pinned)
    {
        if (closed || row->icon.persistentKey.empty()) return;
        std::erase(settings.pinnedTrayItems, row->icon.persistentKey);
        if (pinned) settings.pinnedTrayItems.push_back(row->icon.persistentKey);
        Save();
    }
    void Move(const std::shared_ptr<Row>& row, int delta)
    {
        if (closed || !managing || row->icon.persistentKey.empty()) return;
        std::vector<std::string> order;
        for (const auto& item : rows)
            if (!item->icon.persistentKey.empty() && std::find(order.begin(), order.end(), item->icon.persistentKey) == order.end())
                order.push_back(item->icon.persistentKey);
        const auto found = std::find(order.begin(), order.end(), row->icon.persistentKey);
        if (found == order.end()) return;
        const auto index = found - order.begin(), next = index + delta;
        if (next < 0 || next >= static_cast<std::ptrdiff_t>(order.size())) return;
        std::iter_swap(order.begin() + index, order.begin() + next);
        for (const auto& key : settings.trayOrder)
            if (std::find(order.begin(), order.end(), key) == order.end()) order.push_back(key);
        settings.trayOrder = std::move(order); Save();
    }
    std::shared_ptr<Row> Create(const tray::Icon& icon)
    {
        auto row = std::make_shared<Row>();
        row->image.Width(20); row->image.Height(20); row->fallback.Width(20); row->fallback.Height(20);
        c::Grid picture; picture.Width(20); picture.Height(20);
        picture.Children().Append(row->fallback); picture.Children().Append(row->image);
        Subtle(row->activate); row->activate.Height(40); row->activate.Padding({8, 8, 8, 8});
        a::AutomationProperties::SetAutomationId(row->activate, winrt::to_hstring("tray.icon." + icon.key));
        c::ToolTipService::SetToolTip(row->activate, row->tip);
        if (managing)
        {
            row->container.ColumnSpacing(4);
            for (int i = 0; i < 4; ++i)
            {
                c::ColumnDefinition column;
                column.Width(i ? x::GridLengthHelper::Auto() : x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star));
                row->container.ColumnDefinitions().Append(column);
            }
            c::Grid label; label.ColumnSpacing(10);
            c::ColumnDefinition imageColumn; imageColumn.Width(x::GridLengthHelper::Auto());
            label.ColumnDefinitions().Append(imageColumn); label.ColumnDefinitions().Append(c::ColumnDefinition());
            label.Children().Append(picture); c::Grid::SetColumn(row->title, 1);
            row->title.TextTrimming(x::TextTrimming::CharacterEllipsis); row->title.MaxLines(1);
            row->title.VerticalAlignment(x::VerticalAlignment::Center); label.Children().Append(row->title);
            row->activate.HorizontalAlignment(x::HorizontalAlignment::Stretch);
            row->activate.HorizontalContentAlignment(x::HorizontalAlignment::Stretch); row->activate.Content(label);
            row->pin.Content(winrt::box_value(_LW("statusBar.pin"))); c::Grid::SetColumn(row->pin, 1);
            a::AutomationProperties::SetAutomationId(row->pin, winrt::to_hstring("tray.pin." + icon.key));
            row->container.Children().Append(row->pin);
            for (const auto& button : {row->earlier, row->later}) { Subtle(button); button.Width(32); button.Height(32); button.Padding({6, 6, 6, 6}); }
            c::FontIcon up; up.Glyph(L"\uE70E"); up.FontSize(14);
            c::FontIcon down; down.Glyph(L"\uE70D"); down.FontSize(14);
            row->earlier.Content(up); row->later.Content(down);
            a::AutomationProperties::SetName(row->earlier, _LW("statusBar.moveEarlier"));
            a::AutomationProperties::SetName(row->later, _LW("statusBar.moveLater"));
            c::ToolTipService::SetToolTip(row->earlier, winrt::box_value(_LW("statusBar.moveEarlier")));
            c::ToolTipService::SetToolTip(row->later, winrt::box_value(_LW("statusBar.moveLater")));
            a::AutomationProperties::SetAutomationId(row->earlier, winrt::to_hstring("tray.earlier." + icon.key));
            a::AutomationProperties::SetAutomationId(row->later, winrt::to_hstring("tray.later." + icon.key));
            c::Grid::SetColumn(row->earlier, 2); c::Grid::SetColumn(row->later, 3);
            row->container.Children().Append(row->earlier); row->container.Children().Append(row->later);
        }
        else { row->activate.Width(40); row->activate.Content(picture); }
        row->container.Children().Append(row->activate);
        const auto weak = weak_from_this(); const std::weak_ptr<Row> item = row;
        row->activate.AddHandler(x::UIElement::PointerPressedEvent(), winrt::box_value(x::Input::PointerEventHandler(
            [weak, item](const auto&, const x::Input::PointerRoutedEventArgs& args) {
                const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
                if (args.GetCurrentPoint(row->activate).Properties().IsLeftButtonPressed())
                {
                    row->pointer = true;
                    self->Activate(row, tray::Activation::LeftDown);
                }
            })), true);
        row->activate.Click([weak, item](const auto&, const auto&) {
            const auto self = weak.lock(); const auto row = item.lock(); if (!self || self->closed || !row) return;
            const bool pointer = std::exchange(row->pointer, false);
            self->Activate(row, pointer ? tray::Activation::LeftUp : tray::Activation::Keyboard);
        });
        row->activate.PointerCanceled([item](const auto&, const auto&) { if (const auto row = item.lock()) row->pointer = false; });
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
        const auto pinChanged = [weak, item](const auto&, const auto&) {
            if (const auto self = weak.lock()) if (const auto row = item.lock(); row && !row->updating)
                self->Pin(row, row->pin.IsChecked().Value());
        };
        row->pin.Checked(pinChanged); row->pin.Unchecked(pinChanged);
        row->earlier.Click([weak, item](const auto&, const auto&) { if (const auto self = weak.lock()) if (const auto row = item.lock()) self->Move(row, -1); });
        row->later.Click([weak, item](const auto&, const auto&) { if (const auto self = weak.lock()) if (const auto row = item.lock()) self->Move(row, 1); });
        Update(row, icon, true); return row;
    }
    void Present()
    {
        if (closed) return;
        auto icons = snapshot.icons;
        std::erase_if(icons, [&](const auto& icon) { return (icon.state & NIS_HIDDEN) || (!managing && Pinned(icon)); });
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
        rows = std::move(next);
        if (structureChanged)
        {
            grid.Children().Clear(); grid.ColumnDefinitions().Clear(); grid.RowDefinitions().Clear();
            grid.ColumnSpacing(managing ? 0 : 8); grid.RowSpacing(managing ? 4 : 8);
            grid.HorizontalAlignment(managing ? x::HorizontalAlignment::Stretch : x::HorizontalAlignment::Center);
            for (int i = 0; i < (managing ? 1 : 5); ++i)
            {
                c::ColumnDefinition column; column.Width(managing ? x::GridLengthHelper::FromValueAndType(1, x::GridUnitType::Star) : x::GridLengthHelper::Auto());
                grid.ColumnDefinitions().Append(column);
            }
            for (std::size_t i = 0; i < rows.size(); ++i)
            {
                const auto index = static_cast<int>(i), columns = managing ? 1 : 5;
                if (index % columns == 0) grid.RowDefinitions().Append(c::RowDefinition());
                c::Grid::SetRow(rows[i]->container, index / columns); c::Grid::SetColumn(rows[i]->container, index % columns);
                grid.Children().Append(rows[i]->container);
            }
        }
        for (std::size_t i = 0; i < rows.size(); ++i)
        {
            const auto& row = rows[i];
            const bool identified = !row->icon.persistentKey.empty();
            const auto movable = [](const auto& other) { return !other->icon.persistentKey.empty(); };
            row->earlier.IsEnabled(identified && std::any_of(rows.begin(), rows.begin() + i, movable));
            row->later.IsEnabled(identified && std::any_of(rows.begin() + i + 1, rows.end(), movable));
        }
        Notice(snapshot.degraded ? _LW("statusBar.trayUnavailable") : !snapshot.connected ? _LW("statusBar.trayConnecting") : rows.empty() ? _LW("statusBar.trayEmpty") : L"");
        if (structureChanged && layoutChanged) layoutChanged();
    }
};
SystemTrayView::SystemTrayView(SystemTrayActions actions, StatusBarSettings settings, std::function<void()> layoutChanged)
    : impl_(std::make_shared<Impl>(std::move(actions), std::move(settings), std::move(layoutChanged))) { impl_->Build(); }
SystemTrayView::~SystemTrayView() { Close(); }
x::FrameworkElement SystemTrayView::Root() const { return impl_->root; }
double SystemTrayView::PreferredWidth() const { return impl_->managing ? 440 : 280; }
void SystemTrayView::Refresh(tray::Snapshot snapshot)
{
    if (impl_->closed) return;
    impl_->snapshot = std::move(snapshot); impl_->Present();
}
void SystemTrayView::Close() { impl_->closed = true; impl_->actions = {}; impl_->layoutChanged = {}; }
}
