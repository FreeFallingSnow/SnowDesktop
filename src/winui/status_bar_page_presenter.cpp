#include "pch.h"
#include "status_bar_page_presenter.h"
#include "settings_presenter_controls.h"
#include "system/status_bar/status_bar_shell_shortcut.h"
#include "panel_appearance_editor.h"
#include "theme_library_controls.h"
#include "system/status_bar/status_bar_appearance.h"
#include "merged_bar_height_editor.h"

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
using presenter_controls::SettingRow;
struct StatusBarPagePresenter::Impl
{
    DockPagePresenter::LocalizeCallback localize;
    DockPageActions actions;
    std::function<void(SettingsRoute)> navigate;
    std::unique_ptr<MergedBarHeightEditor> mergedHeight;
    BarSettingsAvailability availability;
    int monitorCount = 0;
    muxc::TextBlock mergedNotice;
    muxc::HyperlinkButton dockAppearanceLink;
    muxc::Expander independentAppearance;
    muxc::ContentControl independentHost;
    muxc::StackPanel independentBody;
    muxc::StackPanel root, basic, leftItems, infoItems;
    muxc::Expander leftSection, infoSection;
    muxc::ComboBox edge, monitors, clockPanel, controlPanel;
    muxc::Slider scale;
    muxc::NumberBox scaleNumber;
    muxc::Button scaleReset;
    SettingRow edgeRow, monitorsRow, scaleRow, clockPanelRow, controlPanelRow;
    muxc::TextBlock defaultThemeTitle, rulesTitle, rulesHint, behaviorTitle, behaviorHint;
    const bool systemQuickSettings = StatusBarSupportsSystemQuickSettings();
    struct ThemeControl
    {
        muxc::StackPanel root;
        muxc::ComboBox combo;
        SettingRow row;
        std::shared_ptr<PanelAppearanceEditor> editor;
        std::unique_ptr<ThemeLibraryControls> themes;
    };
    struct RuleControl
    {
        std::string key, focus;
        StatusBarAppearanceRule StatusBarSettings::* member;
        muxc::TextBlock title;
        muxc::ToggleSwitch enabled;
        SettingRow enabledRow;
        ThemeControl theme;
    };
    ThemeControl defaultTheme;
    std::vector<std::unique_ptr<RuleControl>> rules;
    PersonalizationSettings globalAppearance;
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
    std::uint64_t generation = 0, generalRevision = 0, personalizationRevision = 0, dockRevision = 0;
    bool syncing = false, closed = false, active = false, scaleDirty = false, hasSnapshot = false;
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
    static SurfaceTheme& Theme(StatusBarSettings& settings,
        StatusBarAppearanceRule StatusBarSettings::* member)
    {
        return member ? (settings.*member).theme : settings.theme;
    }
    void InitializeTheme(ThemeControl& control, muxc::StackPanel parent,
        StatusBarAppearanceRule StatusBarSettings::* member = nullptr)
    {
        control.root.Spacing(12);
        control.combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        control.combo.MaxWidth(520);
        control.editor = PanelAppearanceEditor::Create(localize,
            [this, member](const auto& appearance, bool commit) {
                Emit([member, appearance](auto& settings) {
                    auto& theme = Theme(settings, member);
                    theme.appearance = appearance;
                    theme.customized = true;
                }, commit);
            });
        const std::string target = member == &StatusBarSettings::noWindow ? "statusBar/noWindow" :
            member == &StatusBarSettings::maximizedWindow ? "statusBar/maximizedWindow" : "statusBar";
        control.themes = std::make_unique<ThemeLibraryControls>(localize, target, false, control.combo, 10, 9);
        control.row.Initialize(control.themes->SelectionContent());
        control.root.Children().Append(control.row.root);
        control.root.Children().Append(control.themes->Content());
        control.root.Children().Append(control.editor->Content());
        control.root.Children().Append(control.themes->SaveContent());
        control.themes->SetCustomContent({control.editor->Content()});
        parent.Children().Append(control.root);
        const auto combo = control.combo;
        const auto editor = control.editor;
        const auto token = combo.SelectionChanged([this, member, combo, editor, themes = control.themes.get()](const auto&, const auto&) {
            if (syncing || closed || !active) return;
            if (themes->ApplySelection()) return;
            const int index = combo.SelectedIndex();
            if (index < 0 || index >= static_cast<int>(StatusBarThemeModes.size())) return;
            editor->Flush();
            const auto global = globalAppearance;
            Emit([member, index, global](auto& settings) {
                auto& theme = Theme(settings, member);
                const int mode = StatusBarThemeModes[static_cast<std::size_t>(index)];
                SelectSurfaceThemeMode(theme, mode, ResolveStatusBarAppearance(theme, global));
            });
        });
        revoke.push_back([combo, token] { combo.SelectionChanged(token); });
    }
    static void Title(muxc::TextBlock title)
    {
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        title.TextWrapping(mux::TextWrapping::Wrap);
    }
    void InitializeRule(muxc::StackPanel parent, const char* key, const char* focus,
        StatusBarAppearanceRule StatusBarSettings::* member)
    {
        auto rule = std::make_unique<RuleControl>();
        rule->key = key; rule->focus = focus; rule->member = member;
        Title(rule->title); rule->title.Margin({0,12,0,0});
        parent.Children().Append(rule->title);
        rule->enabledRow.Initialize(rule->enabled);
        rule->enabledRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        parent.Children().Append(rule->enabledRow.root);
        InitializeTheme(rule->theme, parent, member);
        const auto control = rule->enabled;
        const auto editor = rule->theme.editor;
        const auto token = control.Toggled([this, member, control, editor](const auto&, const auto&) {
            if (syncing || closed || !active) return;
            const bool enabled = control.IsOn();
            editor->Flush();
            Emit([member, enabled](auto& settings) { (settings.*member).enabled = enabled; });
        });
        revoke.push_back([control, token] { control.Toggled(token); });
        rules.push_back(std::move(rule));
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
        syncing = true; scale.Value(percent); presenter_controls::SetNumberBoxValue(scaleNumber, percent); syncing = false;
        scaleReset.IsEnabled(percent != 100); scaleDirty = true; scalePreview.Queue(percent);
    }
    Impl(DockPagePresenter::LocalizeCallback callback, const mux::Style& style,
        std::function<void(SettingsRoute)> navigateCallback)
        : localize(std::move(callback)), navigate(std::move(navigateCallback))
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
        mergedHeight = std::make_unique<MergedBarHeightEditor>(localize);
        basic.Children().Append(mergedHeight->Content());
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
        ToggleRow(leftItems, "statusBar.taskView", "statusBar.taskView", &StatusBarSettings::taskView);
        ToggleRow(infoItems, "statusBar.inputMethod", "statusBar.inputMethod", &StatusBarSettings::inputMethod);
        ToggleRow(infoItems, "statusBar.cpu", "statusBar.cpu", &StatusBarSettings::cpu);
        ToggleRow(infoItems, "statusBar.memory", "statusBar.memory", &StatusBarSettings::memory);
        ToggleRow(infoItems, "statusBar.gpu", "statusBar.gpu", &StatusBarSettings::gpu);
        ToggleRow(infoItems, "statusBar.traffic", "statusBar.traffic", &StatusBarSettings::traffic);
        auto behavior = Card(style);
        Title(behaviorTitle); behavior.Children().Append(behaviorTitle);
        clockPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        controlPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        clockPanelRow.Initialize(clockPanel); controlPanelRow.Initialize(controlPanel);
        behavior.Children().Append(clockPanelRow.root);
        if (systemQuickSettings) behavior.Children().Append(controlPanelRow.root);
        behaviorHint.TextWrapping(mux::TextWrapping::Wrap); behaviorHint.Opacity(.72);
        behavior.Children().Append(behaviorHint);
        const auto clockSelection = clockPanel.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = clockPanel.SelectedIndex();
            if (selected >= 0 && selected <= 1) Emit([selected](auto& settings) { settings.clockSystemPanel = selected == 1; });
            SyncBehaviorHint();
        });
        revoke.push_back([control = clockPanel, clockSelection] { control.SelectionChanged(clockSelection); });
        const auto controlSelection = controlPanel.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = controlPanel.SelectedIndex();
            if (systemQuickSettings && selected >= 0 && selected <= 1)
                Emit([selected](auto& settings) { settings.controlCenterSystemPanel = selected == 1; });
            SyncBehaviorHint();
        });
        revoke.push_back([control = controlPanel, controlSelection] { control.SelectionChanged(controlSelection); });
        independentAppearance.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        independentAppearance.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        independentBody.Spacing(12);
        mergedNotice.TextWrapping(mux::TextWrapping::Wrap);
        root.Children().Append(mergedNotice);
        dockAppearanceLink.HorizontalAlignment(mux::HorizontalAlignment::Left);
        root.Children().Append(dockAppearanceLink);
        const auto appearanceLinkToken = dockAppearanceLink.Click([this](const auto&, const auto&) {
            if (active && navigate) navigate(SettingsRoute::ForPage(SettingsPage::Dock, "personalization.dockAppearance"));
        });
        revoke.push_back([control = dockAppearanceLink, appearanceLinkToken] { control.Click(appearanceLinkToken); });
        independentHost.Content(independentBody);
        independentHost.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        independentAppearance.Content(independentHost);
        root.Children().Append(independentAppearance);
        auto appearance = independentBody;
        Title(defaultThemeTitle); appearance.Children().Append(defaultThemeTitle);
        InitializeTheme(defaultTheme, appearance);

        auto scenarios = independentBody;
        Title(rulesTitle); scenarios.Children().Append(rulesTitle);
        rulesHint.TextWrapping(mux::TextWrapping::Wrap); rulesHint.Opacity(.72);
        scenarios.Children().Append(rulesHint);
        InitializeRule(scenarios, "statusBar.noWindow", "statusBar.noWindow", &StatusBarSettings::noWindow);
        InitializeRule(scenarios, "statusBar.maximizedWindow", "statusBar.maximizedWindow", &StatusBarSettings::maximizedWindow);
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
        defaultTheme.editor->Flush();
        for (auto& rule : rules) rule->theme.editor->Flush();
        if (!scaleDirty) return;
        scalePreview.Cancel(); scaleDirty = false;
        const auto percent = scale.Value();
        Emit([percent](auto& settings) { settings.scale = static_cast<float>(percent / 100.); });
    }
    void SyncTheme(ThemeControl& control, const SurfaceTheme& theme, bool force)
    {
        control.combo.SelectedIndex(StatusBarThemeSelection(theme.mode));
        control.editor->SetValue(ResolveStatusBarAppearance(theme, globalAppearance), force);
        control.editor->Content().Visibility(theme.mode == 4 ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        control.themes->SaveContent().Visibility(control.editor->Content().Visibility());
    }
    void SyncBehaviorHint()
    {
        const bool usesSystemPanel = value.clockSystemPanel ||
            (systemQuickSettings && value.controlCenterSystemPanel);
        behaviorHint.Visibility(usesSystemPanel ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    void Sync(bool force = false)
    {
        syncing = true;
        for (auto& toggle : toggles) toggle->control.IsOn(value.*(toggle->member));
        edge.SelectedIndex(static_cast<int>(value.position)); monitors.SelectedIndex(static_cast<int>(value.monitorScope));
        clockPanel.SelectedIndex(value.clockSystemPanel ? 1 : 0);
        controlPanel.SelectedIndex(systemQuickSettings && value.controlCenterSystemPanel ? 1 : 0);
        SyncBehaviorHint();
        if (!scaleDirty)
        {
            const double percent = presenter_controls::QuantizeNumericValue(value.scale * 100., 75, 300, 5);
            scale.Value(percent); presenter_controls::SyncNumberBoxValue(scaleNumber, percent); scaleReset.IsEnabled(percent != 100);
        }
        SyncTheme(defaultTheme, value.theme, force);
        for (auto& rule : rules)
        {
            const auto& current = value.*(rule->member);
            rule->enabled.IsOn(current.enabled);
            rule->theme.root.Visibility(current.enabled ? mux::Visibility::Visible : mux::Visibility::Collapsed);
            SyncTheme(rule->theme, current.theme, force);
        }
        edgeRow.SetEnabled(value.enabled); monitorsRow.SetEnabled(value.enabled);
        scaleRow.SetEnabled(value.enabled && !availability.allStatusMerged);
        clockPanelRow.SetEnabled(value.enabled); controlPanelRow.SetEnabled(value.enabled);
        for (auto& toggle : toggles)
            toggle->row.SetEnabled(toggle->member == &StatusBarSettings::enabled ||
                (value.enabled && !(toggle->member == &StatusBarSettings::quickSearch && availability.allStatusMerged)));
        independentHost.IsEnabled(value.enabled && !availability.allStatusMerged);
        independentHost.Opacity(value.enabled && !availability.allStatusMerged ? 1.0 : .62);
        mergedNotice.Visibility(availability.anyMerged ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        dockAppearanceLink.Visibility(availability.anyMerged ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        mergedNotice.Text(L(availability.allStatusMerged ? "settings.bars.inheritedAppearance" : "settings.bars.partialAppearance"));

        syncing = false;
    }
    void LocalizeTheme(ThemeControl& control)
    {
        control.row.SetText(L("app.settings.theme"));
        mux::Automation::AutomationProperties::SetName(control.combo, control.row.label.Text());
        control.combo.Items().Clear();
        for (const auto key : {"app.settings.taskbar_follow_global", "app.settings.dark", "app.settings.light",
            "app.settings.dark_glass", "app.settings.light_glass", "app.settings.dark_acrylic",
            "app.settings.light_acrylic", "statusBar.transparentDarkText", "statusBar.transparentLightText", "app.settings.custom"})
            control.combo.Items().Append(winrt::box_value(L(key)));
        control.editor->RefreshLocalizedText();
    }
    void Localize()
    {
        Flush();
        syncing = true;
        for (auto& toggle : toggles) toggle->row.SetText(L(toggle->key));
        leftSection.Header(winrt::box_value(L("statusBar.leftItems")));
        infoSection.Header(winrt::box_value(L("statusBar.information")));
        behaviorTitle.Text(L("statusBar.clickBehavior"));
        behaviorHint.Text(L("statusBar.systemPanelPlacement"));
        clockPanelRow.SetText(L("statusBar.clockPanel")); controlPanelRow.SetText(L("statusBar.controlCenterPanel"));
        mux::Automation::AutomationProperties::SetName(clockPanel, clockPanelRow.label.Text());
        mux::Automation::AutomationProperties::SetName(controlPanel, controlPanelRow.label.Text());
        clockPanel.Items().Clear(); controlPanel.Items().Clear();
        clockPanel.Items().Append(winrt::box_value(L("statusBar.panelCustom")));
        clockPanel.Items().Append(winrt::box_value(L(systemQuickSettings ? "statusBar.panelSystemCalendar" : "statusBar.panelSystemDateTime")));
        controlPanel.Items().Append(winrt::box_value(L("statusBar.panelCustom")));
        if (systemQuickSettings) controlPanel.Items().Append(winrt::box_value(L("statusBar.panelSystemControls")));
        presenter_controls::ConfigureRestoreDefaultButton(scaleReset, L("app.settings.restore_default"));
        mux::Automation::AutomationProperties::SetName(scale, L("statusBar.scale"));
        mux::Automation::AutomationProperties::SetName(scaleNumber, L("statusBar.scale"));
        edgeRow.SetText(L("statusBar.position")); monitorsRow.SetText(L("settings.dock.monitor"));
        scaleRow.SetText(L("statusBar.scale"));
        independentAppearance.Header(winrt::box_value(L("settings.bars.independentAppearance")));
        dockAppearanceLink.Content(winrt::box_value(L("settings.bars.openDockAppearance")));
        mergedHeight->RefreshLocalizedText();
        defaultThemeTitle.Text(L("statusBar.defaultAppearance"));
        rulesTitle.Text(L("settings.taskbar.scenarioOverrides"));
        rulesHint.Text(L("statusBar.scenarioOverrides.description"));
        LocalizeTheme(defaultTheme);
        mux::Automation::AutomationProperties::SetName(defaultTheme.combo, defaultThemeTitle.Text());
        for (auto& rule : rules)
        {
            rule->title.Text(L(rule->key));
            rule->enabledRow.SetText(L("app.settings.widgets_enabled"));
            mux::Automation::AutomationProperties::SetName(rule->enabled, rule->title.Text());
            LocalizeTheme(rule->theme);
            mux::Automation::AutomationProperties::SetName(rule->theme.combo,
                L(rule->key) + L" / " + L("app.settings.theme"));
        }
        edge.Items().Clear(); monitors.Items().Clear();
        for (auto key : {"app.dock.bottom", "app.dock.top"}) edge.Items().Append(winrt::box_value(L(key)));
        for (auto key : {"app.dock.first_screen", "app.dock.last_screen", "app.dock.all_screens"}) monitors.Items().Append(winrt::box_value(L(key)));
        syncing = false; Sync();
    }
    void Close()
    {
        if (closed) return;
        mergedHeight->Flush(); mergedHeight->Close();
        Flush(); closed = true;
        scalePreview.Close();
        defaultTheme.themes->Close();
        for (auto& rule : rules) rule->theme.themes->Close();
        defaultTheme.editor->Close();
        for (auto& rule : rules) rule->theme.editor->Close();
        for (auto& remove : revoke) remove(); revoke.clear();
        actions = {};
    }
};
StatusBarPagePresenter::StatusBarPagePresenter(DockPagePresenter::LocalizeCallback localize, const mux::Style& style,
    std::function<void(SettingsRoute)> navigate)
    : impl_(std::make_unique<Impl>(std::move(localize), style, std::move(navigate))) {}
StatusBarPagePresenter::~StatusBarPagePresenter() { Close(); }
void StatusBarPagePresenter::SetActions(DockPageActions actions)
{
    impl_->mergedHeight->SetActions(actions); impl_->actions = std::move(actions);
    const auto configure = [state = impl_.get()](ThemeLibraryControls& themes) {
        themes.SetChanged([state] {
            state->defaultTheme.themes->Refresh();
            for (auto& rule : state->rules) rule->theme.themes->Refresh();
        });
        themes.SetActions(state->actions.themeLibrary, state->actions.themeAsync, [state] {
            if (!state->active || state->closed || !state->hasSnapshot) return false;
            state->Flush(); state->defaultTheme.editor->Flush();
            for (auto& rule : state->rules) rule->theme.editor->Flush();
            return true;
        });
    };
    configure(*impl_->defaultTheme.themes);
    for (auto& rule : impl_->rules) configure(*rule->theme.themes);
}
mux::UIElement StatusBarPagePresenter::Content() const { return impl_->root; }
void StatusBarPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_->closed) return;
    const bool replaceSession = !impl_->hasSnapshot || impl_->generation != snapshot.generation;
    const int monitorCount = GetSystemMetrics(SM_CMONITORS);
    if (!replaceSession && impl_->generalRevision == snapshot.domainRevisions.general &&
        impl_->personalizationRevision == snapshot.domainRevisions.personalization && impl_->dockRevision == snapshot.domainRevisions.dock &&
        impl_->monitorCount == monitorCount) return;
    if (replaceSession)
    { impl_->scalePreview.Cancel(); impl_->scaleDirty = false; }
    impl_->generation = snapshot.generation;
    impl_->defaultTheme.themes->SetGeneration(snapshot.generation);
    for (auto& rule : impl_->rules) rule->theme.themes->SetGeneration(snapshot.generation);
    impl_->value = snapshot.values.general.statusBar;
    impl_->availability = ResolveBarSettingsAvailability(snapshot.values.general.dockEnabled, snapshot.values.dock,
        snapshot.values.general.statusBar, monitorCount);
    impl_->monitorCount = monitorCount;
    impl_->mergedHeight->Update(snapshot);
    impl_->dockRevision = snapshot.domainRevisions.dock;
    impl_->globalAppearance = snapshot.values.personalization;
    impl_->Sync(replaceSession);
    impl_->syncing = true;
    impl_->defaultTheme.themes->SyncSelection(StatusBarThemeSelection(impl_->value.theme.mode), snapshot.values);
    for (auto& rule : impl_->rules)
        rule->theme.themes->SyncSelection(StatusBarThemeSelection((impl_->value.*rule->member).theme.mode), snapshot.values);
    impl_->syncing = false;
    impl_->generalRevision = snapshot.domainRevisions.general;
    impl_->personalizationRevision = snapshot.domainRevisions.personalization;
    impl_->hasSnapshot = true;
}
void StatusBarPagePresenter::RefreshLocalizedText()
{
    impl_->Localize(); impl_->defaultTheme.themes->LocalizeText();
    for (auto& rule : impl_->rules) rule->theme.themes->LocalizeText();
}
void StatusBarPagePresenter::Activate(std::string_view focusId)
{
    impl_->active = true;
    impl_->defaultTheme.themes->Refresh();
    for (auto& rule : impl_->rules) rule->theme.themes->Refresh();
    if (focusId == "statusBar.theme" || focusId == "statusBar.fullscreen" || focusId == "statusBar.maximizedWindow")
        impl_->independentAppearance.IsExpanded(true);
}
void StatusBarPagePresenter::Deactivate() { impl_->mergedHeight->Flush(); impl_->Flush(); impl_->active = false; }
void StatusBarPagePresenter::Close() { impl_->Close(); }
void StatusBarPagePresenter::RegisterFocusTargets(const std::function<void(std::string, const mux::FrameworkElement&)>& target) const
{
    target("statusBar.position", impl_->edgeRow.root); target("statusBar.monitor", impl_->monitorsRow.root);
    target("statusBar.scale", impl_->scaleRow.root);
    target("statusBar.mergedBarHeight", impl_->mergedHeight->FocusTarget());
    target("statusBar.clockPanel", impl_->clockPanelRow.root);
    if (impl_->systemQuickSettings) target("statusBar.controlCenterPanel", impl_->controlPanelRow.root);
    target("statusBar.theme", impl_->defaultTheme.row.root);
    for (auto& rule : impl_->rules) target(rule->focus, rule->enabledRow.root);
    for (auto& toggle : impl_->toggles) target(toggle->focus, toggle->row.root);
}
}
