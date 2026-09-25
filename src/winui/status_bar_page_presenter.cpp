#include "pch.h"
#include "status_bar_page_presenter.h"
#include "settings_presenter_controls.h"

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
using presenter_controls::SettingRow;
struct StatusBarPagePresenter::Impl
{
    DockPagePresenter::LocalizeCallback localize;
    DockPageActions actions;
    muxc::StackPanel root, basic, contents, controls, leftItems, centerItems, rightItems;
    muxc::ComboBox edge, monitors;
    muxc::Slider scale;
    SettingRow edgeRow, monitorsRow, scaleRow;
    muxc::TextBlock contentsTitle, controlsTitle, leftTitle, centerTitle, rightTitle;
    struct Toggle
    {
        std::string key, focus, item;
        bool StatusBarSettings::* member;
        muxc::ToggleSwitch control;
        muxc::Button earlier{nullptr}, later{nullptr};
        SettingRow row;
        int zone = 0;
    };
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::function<void()>> revoke;
    StatusBarSettings value;
    std::uint64_t generation = 0;
    bool syncing = false, closed = false, active = false, scaleDirty = false;
    presenter_controls::CoalescedPreviewTimer<double> scalePreview;
    std::wstring L(std::string_view key) const { return localize ? localize(key) : std::wstring{}; }
    muxc::StackPanel Card(const mux::Style& style)
    {
        muxc::Border border; if (style) border.Style(style);
        muxc::StackPanel panel; panel.Spacing(12);
        border.Child(panel); root.Children().Append(border); return panel;
    }
    void Emit(std::function<void(StatusBarSettings&)> edit, bool commit = true)
    {
        if (syncing || closed || !active || !generation || !actions.updateGeneral) return;
        edit(value);
        actions.updateGeneral(generation, commit ? SettingsUpdateMode::PreviewAndCommit : SettingsUpdateMode::Preview,
            [edit = std::move(edit)](GeneralSettings& settings) { edit(settings.statusBar); });
    }
    void ToggleRow(muxc::StackPanel panel, const char* key, const char* focus, bool StatusBarSettings::* member, int zone = 0)
    {
        auto toggle = std::make_unique<Toggle>();
        toggle->key = key; toggle->focus = focus; toggle->member = member;
        toggle->zone = zone;
        toggle->item = std::string(key).substr(std::string("statusBar.").size());
        muxc::StackPanel buttons; buttons.Orientation(muxc::Orientation::Horizontal); buttons.Spacing(6);
        buttons.Children().Append(toggle->control);
        if (zone)
        {
            toggle->earlier = muxc::Button(); toggle->later = muxc::Button();
            for (auto direction : {-1, 1})
            {
                auto button = direction < 0 ? toggle->earlier : toggle->later;
                muxc::FontIcon arrow; arrow.Glyph(direction < 0 ? L"\uE70E" : L"\uE70D"); arrow.FontSize(12);
                button.Content(arrow);
                button.Padding({6, 4, 6, 4});
                const auto item = toggle->item;
                const auto click = button.Click([this, zone, item, direction](const auto&, const auto&) {
                    Emit([zone, item, direction](auto& settings) {
                        auto& order = zone == 1 ? settings.leftOrder : settings.rightOrder;
                        const auto found = std::find(order.begin(), order.end(), item);
                        if (found == order.end()) return;
                        const auto index = found - order.begin(), next = index + direction;
                        if (next >= 0 && next < static_cast<std::ptrdiff_t>(order.size())) std::iter_swap(found, order.begin() + next);
                    });
                    Sync();
                });
                revoke.push_back([button, click] { button.Click(click); });
                buttons.Children().Append(button);
            }
        }
        toggle->row.Initialize(buttons);
        toggle->row.SetControlAlignment(mux::HorizontalAlignment::Right);
        panel.Children().Append(toggle->row.root);
        auto control = toggle->control;
        const auto token = control.Toggled([this, control, member](const auto&, const auto&) {
            const bool on = control.IsOn(); Emit([member, on](auto& settings) { settings.*member = on; });
        });
        revoke.push_back([control, token] { control.Toggled(token); });
        toggles.push_back(std::move(toggle));
    }
    Impl(DockPagePresenter::LocalizeCallback callback, const mux::Style& style) : localize(std::move(callback))
    {
        root.Spacing(8);
        basic = Card(style); contents = Card(style); controls = Card(style);
        ToggleRow(basic, "statusBar.enabled", "statusBar.enable", &StatusBarSettings::enabled);
        edgeRow.Initialize(edge); monitorsRow.Initialize(monitors); scaleRow.Initialize(scale);
        for (auto row : {edgeRow.root, monitorsRow.root, scaleRow.root}) basic.Children().Append(row);
        scale.Minimum(75); scale.Maximum(300); scale.StepFrequency(5); scale.Width(300);
        contentsTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        controlsTitle.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        contents.Children().Append(contentsTitle); controls.Children().Append(controlsTitle);
        for (auto heading : {leftTitle, centerTitle, rightTitle}) heading.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        contents.Children().Append(leftTitle); contents.Children().Append(leftItems);
        contents.Children().Append(centerTitle); contents.Children().Append(centerItems);
        contents.Children().Append(rightTitle); contents.Children().Append(rightItems);
        for (auto panel : {leftItems, centerItems, rightItems}) panel.Spacing(8);
        ToggleRow(leftItems, "statusBar.menu", "statusBar.menu", &StatusBarSettings::menu, 1);
        ToggleRow(leftItems, "statusBar.quickSearch", "statusBar.quickSearch", &StatusBarSettings::quickSearch, 1);
        ToggleRow(centerItems, "statusBar.clock", "statusBar.contents", &StatusBarSettings::clock);
        ToggleRow(rightItems, "statusBar.tray", "statusBar.tray", &StatusBarSettings::tray, 2);
        ToggleRow(rightItems, "statusBar.network", "statusBar.network", &StatusBarSettings::network, 2);
        ToggleRow(rightItems, "statusBar.volume", "statusBar.volume", &StatusBarSettings::volume, 2);
        ToggleRow(rightItems, "statusBar.battery", "statusBar.battery", &StatusBarSettings::battery, 2);
        ToggleRow(rightItems, "statusBar.controlCenter", "statusBar.controlCenter", &StatusBarSettings::controlCenter, 2);
        ToggleRow(rightItems, "statusBar.cpu", "statusBar.cpu", &StatusBarSettings::cpu, 2);
        ToggleRow(rightItems, "statusBar.memory", "statusBar.memory", &StatusBarSettings::memory, 2);
        ToggleRow(rightItems, "statusBar.gpu", "statusBar.gpu", &StatusBarSettings::gpu, 2);
        ToggleRow(rightItems, "statusBar.traffic", "statusBar.traffic", &StatusBarSettings::traffic, 2);
        ToggleRow(controls, "statusBar.audioControls", "statusBar.controls", &StatusBarSettings::audioControls);
        ToggleRow(controls, "statusBar.brightnessControls", "statusBar.brightness", &StatusBarSettings::brightnessControls);
        ToggleRow(controls, "statusBar.wifiControls", "statusBar.wifi", &StatusBarSettings::wifiControls);
        ToggleRow(controls, "statusBar.bluetoothControls", "statusBar.bluetooth", &StatusBarSettings::bluetoothControls);
        ToggleRow(controls, "statusBar.mediaControls", "statusBar.media", &StatusBarSettings::mediaControls);
        ToggleRow(controls, "statusBar.powerControls", "statusBar.power", &StatusBarSettings::powerControls);
        auto token = edge.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = edge.SelectedIndex();
            if (selected >= 0) Emit([selected](auto& settings) { settings.position = static_cast<DockPosition>(selected); });
        });
        revoke.push_back([control = edge, token] { control.SelectionChanged(token); });
        token = monitors.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = monitors.SelectedIndex();
            if (selected >= 0) Emit([selected](auto& settings) { settings.monitorScope = static_cast<DockMonitorScope>(selected); });
        });
        revoke.push_back([control = monitors, token] { control.SelectionChanged(token); });
        scalePreview.Initialize([this](double percent) {
            Emit([percent](auto& settings) { settings.scale = static_cast<float>(percent / 100.); }, false);
        });
        token = scale.ValueChanged([this](const auto&, const auto&) {
            if (syncing || closed || !active) return;
            scaleDirty = true; scalePreview.Queue(scale.Value());
        });
        revoke.push_back([control = scale, token] { control.ValueChanged(token); });
        const auto pointer = scale.PointerReleased([this](const auto&, const auto&) { Flush(); });
        revoke.push_back([control = scale, pointer] { control.PointerReleased(pointer); });
        const auto focus = scale.LostFocus([this](const auto&, const auto&) { Flush(); });
        revoke.push_back([control = scale, focus] { control.LostFocus(focus); });
        const auto key = scale.KeyUp([this](const auto&, const auto&) { Flush(); });
        revoke.push_back([control = scale, key] { control.KeyUp(key); });
        Localize();
    }
    void Flush()
    {
        if (!scaleDirty) return;
        scalePreview.Cancel(); scaleDirty = false;
        const auto percent = scale.Value();
        Emit([percent](auto& settings) { settings.scale = static_cast<float>(percent / 100.); });
    }
    void Sync()
    {
        syncing = true;
        for (auto& toggle : toggles) toggle->control.IsOn(value.*(toggle->member));
        edge.SelectedIndex(static_cast<int>(value.position)); monitors.SelectedIndex(static_cast<int>(value.monitorScope));
        if (!scaleDirty) scale.Value(value.scale * 100.);
        for (const auto zone : {1, 2})
        {
            auto panel = zone == 1 ? leftItems : rightItems;
            const auto& order = zone == 1 ? value.leftOrder : value.rightOrder;
            for (std::size_t index = 0; index < order.size(); ++index)
                for (auto& toggle : toggles) if (toggle->zone == zone && toggle->item == order[index])
                {
                    std::uint32_t previous = 0;
                    if (panel.Children().IndexOf(toggle->row.root, previous) && previous != index)
                    {
                        panel.Children().RemoveAt(previous);
                        panel.Children().InsertAt(static_cast<std::uint32_t>(index), toggle->row.root);
                    }
                    toggle->earlier.IsEnabled(index > 0); toggle->later.IsEnabled(index + 1 < order.size());
                }
        }
        syncing = false;
    }
    void Localize()
    {
        syncing = true;
        for (auto& toggle : toggles)
        {
            toggle->row.SetText(L(toggle->key));
            if (toggle->zone) for (auto direction : {-1, 1})
            {
                auto button = direction < 0 ? toggle->earlier : toggle->later;
                const auto label = L(direction < 0 ? "statusBar.moveEarlier" : "statusBar.moveLater") + L" · " + L(toggle->key);
                mux::Automation::AutomationProperties::SetName(button, label);
                muxc::ToolTipService::SetToolTip(button, winrt::box_value(label));
            }
        }
        leftTitle.Text(L("statusBar.leftItems")); centerTitle.Text(L("statusBar.centerItems")); rightTitle.Text(L("statusBar.rightItems"));
        edgeRow.SetText(L("statusBar.position")); monitorsRow.SetText(L("settings.dock.monitor"));
        scaleRow.SetText(L("statusBar.scale"));
        edge.Items().Clear(); monitors.Items().Clear();
        for (auto key : {"app.dock.bottom", "app.dock.top"}) edge.Items().Append(winrt::box_value(L(key)));
        for (auto key : {"app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"}) monitors.Items().Append(winrt::box_value(L(key)));
        contentsTitle.Text(L("statusBar.contents")); controlsTitle.Text(L("statusBar.controlCenter"));
        syncing = false; Sync();
    }
    void Close()
    {
        if (closed) return;
        Flush(); closed = true;
        scalePreview.Close();
        for (auto& remove : revoke) remove(); revoke.clear();
        actions = {};
    }
};
StatusBarPagePresenter::StatusBarPagePresenter(DockPagePresenter::LocalizeCallback localize, const mux::Style& style)
    : impl_(std::make_unique<Impl>(std::move(localize), style)) {}
StatusBarPagePresenter::~StatusBarPagePresenter() { Close(); }
void StatusBarPagePresenter::SetActions(DockPageActions actions) { impl_->actions = std::move(actions); }
mux::UIElement StatusBarPagePresenter::Content() const { return impl_->root; }
void StatusBarPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    const bool replaceSession = impl_->generation != snapshot.generation;
    if (replaceSession)
    { impl_->scalePreview.Cancel(); impl_->scaleDirty = false; }
    impl_->generation = snapshot.generation;
    impl_->value = snapshot.values.general.statusBar;
    impl_->Sync();
}
void StatusBarPagePresenter::RefreshLocalizedText() { impl_->Localize(); }
void StatusBarPagePresenter::Activate() { impl_->active = true; }
void StatusBarPagePresenter::Deactivate() { impl_->Flush(); impl_->active = false; }
void StatusBarPagePresenter::Close() { impl_->Close(); }
void StatusBarPagePresenter::RegisterFocusTargets(const std::function<void(std::string, const mux::FrameworkElement&)>& target) const
{
    target("statusBar.position", impl_->edgeRow.root); target("statusBar.monitor", impl_->monitorsRow.root);
    target("statusBar.scale", impl_->scaleRow.root);
    for (auto& toggle : impl_->toggles) target(toggle->focus, toggle->row.root);
}
}
