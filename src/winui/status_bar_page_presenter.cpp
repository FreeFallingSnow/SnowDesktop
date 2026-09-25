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
    muxc::StackPanel root, basic, leftItems, infoItems;
    muxc::Expander leftSection, infoSection;
    muxc::ComboBox edge, monitors;
    muxc::Slider scale;
    muxc::NumberBox scaleNumber;
    muxc::Button scaleReset;
    SettingRow edgeRow, monitorsRow, scaleRow;
    struct Toggle
    {
        std::string key, focus, item;
        bool StatusBarSettings::* member;
        muxc::ToggleSwitch control;
        SettingRow row;
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
    void ToggleRow(muxc::StackPanel panel, const char* key, const char* focus, bool StatusBarSettings::* member)
    {
        auto toggle = std::make_unique<Toggle>();
        toggle->key = key; toggle->focus = focus; toggle->member = member;
        toggle->control.MinWidth(0);
        toggle->row.Initialize(toggle->control);
        toggle->row.SetControlAlignment(mux::HorizontalAlignment::Right);
        panel.Children().Append(toggle->row.root);
        auto control = toggle->control;
        const auto token = control.Toggled([this, control, member](const auto&, const auto&) {
            const bool on = control.IsOn(); Emit([member, on](auto& settings) { settings.*member = on; });
        });
        revoke.push_back([control, token] { control.Toggled(token); });
        toggles.push_back(std::move(toggle));
    }
    void ScaleChanged(double percent)
    {
        if (syncing || closed || !active || !std::isfinite(percent)) return;
        percent = presenter_controls::QuantizeNumericValue(percent, 75, 300, 5);
        syncing = true; scale.Value(percent); scaleNumber.Value(percent); syncing = false;
        scaleReset.IsEnabled(percent != 100); scaleDirty = true; scalePreview.Queue(percent);
    }
    Impl(DockPagePresenter::LocalizeCallback callback, const mux::Style& style) : localize(std::move(callback))
    {
        root.Spacing(8);
        basic = Card(style);
        ToggleRow(basic, "statusBar.enabled", "statusBar.enable", &StatusBarSettings::enabled);
        edgeRow.Initialize(edge); monitorsRow.Initialize(monitors);
        edge.Width(200); monitors.Width(200);
        edgeRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        monitorsRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        muxc::Grid editor; editor.ColumnSpacing(8);
        for (int index = 0; index < 4; ++index)
        {
            muxc::ColumnDefinition column;
            column.Width(index == 0 ? mux::GridLengthHelper::FromValueAndType(1, mux::GridUnitType::Star) : mux::GridLengthHelper::Auto());
            editor.ColumnDefinitions().Append(column);
        }
        scale.Minimum(75); scale.Maximum(300); scale.StepFrequency(5);
        scale.VerticalAlignment(mux::VerticalAlignment::Center);
        scaleNumber.Minimum(75); scaleNumber.Maximum(300); scaleNumber.SmallChange(5); scaleNumber.LargeChange(25);
        scaleNumber.Width(92); scaleNumber.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
        muxc::TextBlock unit; unit.Text(L"%"); unit.VerticalAlignment(mux::VerticalAlignment::Center); unit.Opacity(.72);
        scaleReset.VerticalAlignment(mux::VerticalAlignment::Center);
        editor.Children().Append(scale);
        muxc::Grid::SetColumn(scaleNumber, 1); editor.Children().Append(scaleNumber);
        muxc::Grid::SetColumn(unit, 2); editor.Children().Append(unit);
        muxc::Grid::SetColumn(scaleReset, 3); editor.Children().Append(scaleReset);
        scaleRow.Initialize(editor);
        for (auto row : {edgeRow.root, monitorsRow.root, scaleRow.root}) basic.Children().Append(row);
        for (auto section : {leftSection, infoSection})
        {
            section.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            section.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            root.Children().Append(section);
        }
        leftItems.Spacing(12); infoItems.Spacing(12);
        leftSection.Content(leftItems); infoSection.Content(infoItems);
        ToggleRow(leftItems, "statusBar.menu", "statusBar.menu", &StatusBarSettings::menu);
        ToggleRow(leftItems, "statusBar.quickSearch", "statusBar.quickSearch", &StatusBarSettings::quickSearch);
        ToggleRow(infoItems, "statusBar.cpu", "statusBar.cpu", &StatusBarSettings::cpu);
        ToggleRow(infoItems, "statusBar.memory", "statusBar.memory", &StatusBarSettings::memory);
        ToggleRow(infoItems, "statusBar.gpu", "statusBar.gpu", &StatusBarSettings::gpu);
        ToggleRow(infoItems, "statusBar.traffic", "statusBar.traffic", &StatusBarSettings::traffic);
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
        token = scale.ValueChanged([this](const auto&, const auto&) { ScaleChanged(scale.Value()); });
        revoke.push_back([control = scale, token] { control.ValueChanged(token); });
        token = scaleNumber.ValueChanged([this](const auto&, const auto&) { ScaleChanged(scaleNumber.Value()); });
        revoke.push_back([control = scaleNumber, token] { control.ValueChanged(token); });
        token = scaleReset.Click([this](const auto&, const auto&) { ScaleChanged(100); Flush(); });
        revoke.push_back([control = scaleReset, token] { control.Click(token); });
        for (mux::UIElement control : {scale.as<mux::UIElement>(), scaleNumber.as<mux::UIElement>()})
        {
            const auto pointer = control.PointerReleased([this](const auto&, const auto&) { Flush(); });
            revoke.push_back([control, pointer] { control.PointerReleased(pointer); });
            const auto focus = control.LostFocus([this](const auto&, const auto&) { Flush(); });
            revoke.push_back([control, focus] { control.LostFocus(focus); });
            const auto key = control.KeyUp([this](const auto&, const auto&) { Flush(); });
            revoke.push_back([control, key] { control.KeyUp(key); });
        }
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
        if (!scaleDirty)
        {
            const double percent = presenter_controls::QuantizeNumericValue(value.scale * 100., 75, 300, 5);
            scale.Value(percent); scaleNumber.Value(percent); scaleReset.IsEnabled(percent != 100);
        }
        syncing = false;
    }
    void Localize()
    {
        syncing = true;
        for (auto& toggle : toggles) toggle->row.SetText(L(toggle->key));
        leftSection.Header(winrt::box_value(L("statusBar.leftItems")));
        infoSection.Header(winrt::box_value(L("statusBar.information")));
        presenter_controls::ConfigureRestoreDefaultButton(scaleReset, L("app.settings.restore_default"));
        mux::Automation::AutomationProperties::SetName(scale, L("statusBar.scale"));
        mux::Automation::AutomationProperties::SetName(scaleNumber, L("statusBar.scale"));
        edgeRow.SetText(L("statusBar.position")); monitorsRow.SetText(L("settings.dock.monitor"));
        scaleRow.SetText(L("statusBar.scale"));
        edge.Items().Clear(); monitors.Items().Clear();
        for (auto key : {"app.dock.bottom", "app.dock.top"}) edge.Items().Append(winrt::box_value(L(key)));
        for (auto key : {"app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"}) monitors.Items().Append(winrt::box_value(L(key)));
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
