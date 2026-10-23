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
    muxc::TextBlock relatedTitle;
    muxc::Button dockAppearanceLink;
    muxc::Border relatedAppearance;
    muxc::StackPanel relatedContent;
    muxc::Button calendarLink;
    SettingRow calendarLinkRow;
    SettingRow relatedAppearanceRow;
    muxc::Border independentAppearance;
    muxc::ContentControl independentHost;
    muxc::StackPanel independentBody;
    muxc::StackPanel root, basic, leftItems, infoItems, appearanceRoot;
    bool centralizedAppearance = false;
    muxc::Expander leftSection, infoSection, controlCenterSection, rulesSection;
    muxc::TextBlock layoutTitle, clickTitle;
    muxc::ComboBox edge, monitors, clockPanel, controlPanel, inputPanel, timeNotificationPosition, controlDisplay;
    muxc::Slider scale;
    muxc::NumberBox scaleNumber;
    muxc::Button scaleReset;
    SettingRow edgeRow, monitorsRow, scaleRow, clockPanelRow, controlPanelRow, inputPanelRow, timeNotificationPositionRow, controlDisplayRow;
    muxc::TextBlock defaultThemeTitle, rulesTitle, rulesHint;
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
    muxc::StackPanel Card(const mux::Style& style, mux::FrameworkElement heading = nullptr)
    {
        muxc::Border border;
        muxc::StackPanel panel;
        presenter_controls::InitializeSettingsGroup(border, heading, panel, style);
        root.Children().Append(border); return panel;
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
        control.root.Spacing(0);
        if (member) control.root.Margin({16, 0, 0, 0});
        control.combo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        control.combo.MaxWidth(520);
        control.editor = PanelAppearanceEditor::Create(localize,
            [this, member](const auto& appearance, bool commit) {
                Emit([member, appearance](auto& settings) {
                    auto& theme = Theme(settings, member);
                    theme.appearance = appearance;
                    theme.customized = true;
                }, commit);
            }, false, true);
        const std::string target = member == &StatusBarSettings::noWindow ? "statusBar/noWindow" :
            member == &StatusBarSettings::maximizedWindow ? "statusBar/maximizedWindow" : "statusBar";
        control.themes = std::make_unique<ThemeLibraryControls>(localize, target, false, control.combo, 10, 9);
        if (member) control.themes->SetInlineSurface();
        control.row.Initialize(control.themes->SelectionContent());
        presenter_controls::StyleSettingsSubrow(control.row.root, true, 0);
        control.root.Children().Append(control.row.root);
        control.root.Children().Append(control.themes->Content());
        control.root.Children().Append(control.editor->Content());
        control.root.Children().Append(control.themes->SaveContent());
        presenter_controls::JoinSettingsSections(control.root);
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
        Title(rule->title);
        rule->enabledRow.Initialize(rule->enabled);
        presenter_controls::StyleSettingsSubrow(rule->enabledRow.root, rules.empty(), 0);
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
        root.Spacing(24);
        appearanceRoot.Spacing(8);
        basic = Card(style);
        ToggleRow(basic, "statusBar.enabled", "statusBar.enable", &StatusBarSettings::enabled);
        Title(layoutTitle);
        auto layout = Card(style, layoutTitle);
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
        scaleNumber.Width(presenter_controls::kSettingNumberWidth); scaleNumber.SpinButtonPlacementMode(muxc::NumberBoxSpinButtonPlacementMode::Compact);
        muxc::TextBlock unit; unit.Text(L"%"); unit.VerticalAlignment(mux::VerticalAlignment::Center); unit.Opacity(.72);
        scaleReset.VerticalAlignment(mux::VerticalAlignment::Center);
        editor.Children().Append(scale);
        muxc::Grid::SetColumn(scaleNumber, 1); editor.Children().Append(scaleNumber);
        muxc::Grid::SetColumn(unit, 2); editor.Children().Append(unit);
        muxc::Grid::SetColumn(scaleReset, 3); editor.Children().Append(scaleReset);
        scaleRow.Initialize(editor);
        for (auto row : {edgeRow.root, monitorsRow.root, scaleRow.root}) layout.Children().Append(row);
        mergedHeight = std::make_unique<MergedBarHeightEditor>(localize);
        layout.Children().Append(mergedHeight->Content());
        for (auto section : {leftSection, infoSection, controlCenterSection})
        {
            section.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            section.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
            section.Padding({0, 0, 0, 0});
            section.IsExpanded(false);
        }
        root.Children().Append(leftSection);
        Title(clickTitle);
        auto clickBehavior = Card(style, clickTitle);
        root.Children().Append(controlCenterSection);
        root.Children().Append(infoSection);
        root.Children().Append(appearanceRoot);
        Title(relatedTitle);
        presenter_controls::InitializeSettingsGroup(relatedAppearance, relatedTitle, relatedContent, style);
        relatedAppearanceRow.Initialize(dockAppearanceLink, 0.0);
        relatedAppearanceRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        relatedContent.Children().Append(relatedAppearanceRow.root);
        presenter_controls::UpdateSettingsSubrows(relatedContent, 0.0);
        appearanceRoot.Children().Append(independentAppearance);
        leftItems.Spacing(2); infoItems.Spacing(2);
        leftSection.Content(leftItems); infoSection.Content(infoItems);
        ToggleRow(leftItems, "statusBar.menu", "statusBar.menu", &StatusBarSettings::menu);
        ToggleRow(leftItems, "statusBar.quickSearch", "statusBar.quickSearch", &StatusBarSettings::quickSearch);
        ToggleRow(leftItems, "statusBar.taskView", "statusBar.taskView", &StatusBarSettings::taskView);
        ToggleRow(infoItems, "statusBar.cpu", "statusBar.cpu", &StatusBarSettings::cpu);
        ToggleRow(infoItems, "statusBar.memory", "statusBar.memory", &StatusBarSettings::memory);
        ToggleRow(infoItems, "statusBar.gpu", "statusBar.gpu", &StatusBarSettings::gpu);
        ToggleRow(infoItems, "statusBar.traffic", "statusBar.traffic", &StatusBarSettings::traffic);
        auto timeNotification = leftItems;
        muxc::StackPanel controlCenter; controlCenter.Spacing(2);
        controlCenterSection.Content(controlCenter);
        ToggleRow(leftItems, "statusBar.inputMethod", "statusBar.inputMethod", &StatusBarSettings::inputMethod);
        ToggleRow(leftItems, "statusBar.clock", "statusBar.clock", &StatusBarSettings::clock);
        ToggleRow(leftItems, "statusBar.notifications", "statusBar.notifications", &StatusBarSettings::notifications);
        controlDisplay.Width(200);
        controlDisplayRow.Initialize(controlDisplay);
        controlDisplayRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        leftItems.Children().Append(controlDisplayRow.root);
        ToggleRow(leftItems, "statusBar.batteryPercentage", "statusBar.batteryPercentage", &StatusBarSettings::batteryPercentage);
        inputPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        inputPanelRow.Initialize(inputPanel);
        ToggleRow(controlCenter, "statusBar.audioControls", "statusBar.audioControls", &StatusBarSettings::audioControls);
        ToggleRow(controlCenter, "statusBar.brightnessControls", "statusBar.brightnessControls", &StatusBarSettings::brightnessControls);
        ToggleRow(controlCenter, "statusBar.wifiControls", "statusBar.wifiControls", &StatusBarSettings::wifiControls);
        ToggleRow(controlCenter, "statusBar.bluetoothControls", "statusBar.bluetoothControls", &StatusBarSettings::bluetoothControls);
        ToggleRow(controlCenter, "statusBar.mediaControls", "statusBar.mediaControls", &StatusBarSettings::mediaControls);
        ToggleRow(controlCenter, "statusBar.powerControls", "statusBar.powerControls", &StatusBarSettings::powerControls);
        timeNotificationPosition.Width(200);
        timeNotificationPositionRow.Initialize(timeNotificationPosition);
        timeNotificationPositionRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        timeNotification.Children().Append(timeNotificationPositionRow.root);
        ToggleRow(timeNotification, "statusBar.clockShowSeconds", "statusBar.clockShowSeconds", &StatusBarSettings::clockShowSeconds);
        clockPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        controlPanel.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        clockPanelRow.Initialize(clockPanel); controlPanelRow.Initialize(controlPanel);
        ToggleRow(clickBehavior, "statusBar.showDesktop", "statusBar.showDesktop", &StatusBarSettings::showDesktop);
        clickBehavior.Children().Append(clockPanelRow.root);
        clickBehavior.Children().Append(inputPanelRow.root);
        if (systemQuickSettings) clickBehavior.Children().Append(controlPanelRow.root);
        const auto positionSelection = timeNotificationPosition.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = timeNotificationPosition.SelectedIndex();
            if (selected < 0 || selected > 1) return;
            Emit([selected](auto& settings) { settings.timeNotificationRightAligned = selected == 1; });
        });
        revoke.push_back([control = timeNotificationPosition, positionSelection] { control.SelectionChanged(positionSelection); });
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
        const auto displaySelection = controlDisplay.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = controlDisplay.SelectedIndex();
            if (selected < 0 || selected > 2) return;
            Emit([selected](auto& settings) {
                settings.controlCenter = selected != 2;
                settings.controlCenterSingleIcon = selected == 1;
            });
        });
        revoke.push_back([control = controlDisplay, displaySelection] { control.SelectionChanged(displaySelection); });
        const auto inputSelection = inputPanel.SelectionChanged([this](const auto&, const auto&) {
            const auto selected = inputPanel.SelectedIndex();
            if (selected >= 0 && selected <= 1)
                Emit([selected](auto& settings) { settings.inputMethodSystemPanel = selected == 1; });
        });
        revoke.push_back([control = inputPanel, inputSelection] { control.SelectionChanged(inputSelection); });
        independentAppearance.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        independentBody.Spacing(0);
        const auto appearanceLinkToken = dockAppearanceLink.Click([this](const auto&, const auto&) {
            if (active && navigate) navigate(SettingsRoute::ForPage(SettingsPage::Dock, "personalization.dockAppearance"));
        });
        revoke.push_back([control = dockAppearanceLink, appearanceLinkToken] { control.Click(appearanceLinkToken); });
        independentHost.Content(independentBody);
        independentHost.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        independentAppearance.Child(independentHost);
        muxc::StackPanel appearance;
        appearance.Spacing(0);
        muxc::Border appearanceGroup;
        Title(defaultThemeTitle);
        presenter_controls::InitializeSettingsGroup(appearanceGroup, defaultThemeTitle, appearance, style);
        independentBody.Children().Append(appearanceGroup);
        InitializeTheme(defaultTheme, appearance);

        muxc::StackPanel scenarios;
        scenarios.Spacing(0);
        Title(rulesTitle);
        rulesSection.Header(rulesTitle);
        rulesSection.Content(scenarios);
        rulesSection.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        rulesSection.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        rulesSection.Padding({0, 0, 0, 0});
        rulesSection.IsExpanded(false);
        appearance.Children().Append(presenter_controls::SettingsDisclosureRow(rulesSection));
        rulesHint.TextWrapping(mux::TextWrapping::Wrap); rulesHint.Opacity(.72); rulesHint.FontSize(12);
        InitializeRule(scenarios, "statusBar.noWindow", "statusBar.noWindow", &StatusBarSettings::noWindow);
        rules.front()->enabledRow.text.Children().Append(rulesHint);
        InitializeRule(scenarios, "statusBar.maximizedWindow", "statusBar.maximizedWindow", &StatusBarSettings::maximizedWindow);
        calendarLinkRow.Initialize(calendarLink);
        relatedContent.Children().Append(calendarLinkRow.root);
        presenter_controls::UpdateSettingsSubrows(relatedContent, 0);
        root.Children().Append(relatedAppearance);
        const auto calendarToken = calendarLink.Click([this](const auto&, const auto&) {
            if (active && navigate) navigate(SettingsRoute::ForPage(SettingsPage::Calendar, "calendar.events"));
        });
        revoke.push_back([control = calendarLink, calendarToken] { control.Click(calendarToken); });
        for (const auto panel : {basic, layout, leftItems, infoItems, controlCenter, clickBehavior})
            presenter_controls::UpdateSettingsSubrows(panel, 0);
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
        const auto placement = L("statusBar.systemPanelPlacement");
        clockPanelRow.SetText(L("statusBar.clockPanel"), value.clockSystemPanel ? placement : L"");
        controlPanelRow.SetText(L("statusBar.controlCenterPanel"),
            systemQuickSettings && value.controlCenterSystemPanel && !value.clockSystemPanel ? placement : L"");
        mux::Automation::AutomationProperties::SetHelpText(clockPanel, clockPanelRow.help.Text());
        mux::Automation::AutomationProperties::SetHelpText(controlPanel,
            systemQuickSettings && value.controlCenterSystemPanel ? placement : L"");
    }
    void SyncAppearanceContext()
    {
        const bool showStandaloneNotice = availability.anyMerged && !centralizedAppearance;
        const bool showCalendar = value.clock && !value.clockSystemPanel;
        relatedAppearanceRow.root.Visibility(showStandaloneNotice ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        calendarLinkRow.root.Visibility(showCalendar ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        relatedAppearance.Visibility(showStandaloneNotice || showCalendar ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        presenter_controls::UpdateSettingsSubrows(relatedContent, 0);
        relatedAppearanceRow.SetText(L("settings.bars.openDockAppearance"), L("statusBar.theme.mergedHint"));
        defaultTheme.row.SetText(L("app.settings.theme"), centralizedAppearance && availability.anyMerged
            ? L("statusBar.theme.mergedHint") : L"");
        mux::Automation::AutomationProperties::SetHelpText(defaultTheme.combo, defaultTheme.row.help.Text());
    }
    void Sync(bool force = false)
    {
        syncing = true;
        for (auto& toggle : toggles) toggle->control.IsOn(value.*(toggle->member));
        edge.SelectedIndex(static_cast<int>(value.position)); monitors.SelectedIndex(static_cast<int>(value.monitorScope));
        clockPanel.SelectedIndex(value.clockSystemPanel ? 1 : 0);
        controlPanel.SelectedIndex(systemQuickSettings && value.controlCenterSystemPanel ? 1 : 0);
        controlDisplay.SelectedIndex(!value.controlCenter ? 2 : value.controlCenterSingleIcon ? 1 : 0);
        inputPanel.SelectedIndex(value.inputMethodSystemPanel ? 1 : 0);
        timeNotificationPosition.SelectedIndex(value.timeNotificationRightAligned ? 1 : 0);
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
        scaleRow.SetEnabled(value.enabled);
        clockPanelRow.SetEnabled(value.enabled); controlPanelRow.SetEnabled(value.enabled);
        inputPanelRow.SetEnabled(value.enabled && value.inputMethod);
        timeNotificationPositionRow.SetEnabled(value.enabled);
        controlDisplayRow.SetEnabled(value.enabled);
        for (auto& toggle : toggles)
            toggle->row.SetEnabled(toggle->member == &StatusBarSettings::enabled ||
                value.enabled);
        independentHost.IsEnabled(value.enabled);
        independentHost.Opacity(value.enabled ? 1.0 : .62);
        SyncAppearanceContext();

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
        layoutTitle.Text(L("statusBar.layout"));
        clickTitle.Text(L("statusBar.clickBehavior"));
        leftSection.Header(winrt::box_value(L("statusBar.barItems")));
        infoSection.Header(winrt::box_value(L("statusBar.information")));
        controlCenterSection.Header(winrt::box_value(L("statusBar.controlCenterContent")));
        controlDisplayRow.SetText(L("statusBar.controlCenterDisplay"));
        mux::Automation::AutomationProperties::SetName(controlDisplay, controlDisplayRow.label.Text());
        controlDisplay.Items().Clear();
        for (const auto key : {"app.interact.shown", "statusBar.controlCenterSingleIcon", "app.interact.hidden"})
            controlDisplay.Items().Append(winrt::box_value(L(key)));
        timeNotificationPositionRow.SetText(L("statusBar.timeNotificationPosition"));
        mux::Automation::AutomationProperties::SetName(timeNotificationPosition, timeNotificationPositionRow.label.Text());
        timeNotificationPosition.Items().Clear();
        timeNotificationPosition.Items().Append(winrt::box_value(L("app.settings.taskbar_center")));
        timeNotificationPosition.Items().Append(winrt::box_value(L("app.dock.right")));
        clockPanelRow.SetText(L("statusBar.clockPanel")); controlPanelRow.SetText(L("statusBar.controlCenterPanel"));
        inputPanelRow.SetText(L("statusBar.inputMethodPanel"));
        mux::Automation::AutomationProperties::SetName(inputPanel, inputPanelRow.label.Text());
        inputPanel.Items().Clear();
        inputPanel.Items().Append(winrt::box_value(L("statusBar.panelCustom")));
        inputPanel.Items().Append(winrt::box_value(L("statusBar.panelSystemInput")));
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
        relatedTitle.Text(L("settings.common.relatedSettings"));
        calendarLinkRow.SetText(L("settings.calendar.page"), L("settings.related.calendar"));
        calendarLink.Content(winrt::box_value(L("start.openSettings")));
        dockAppearanceLink.Content(winrt::box_value(L("start.openSettings")));
        mergedHeight->RefreshLocalizedText();
        defaultThemeTitle.Text(L(centralizedAppearance ? "settings.nav.statusBar" : "statusBar.defaultAppearance"));
        rulesTitle.Text(L("settings.taskbar.scenarioOverrides"));
        mux::Automation::AutomationProperties::SetName(rulesSection, rulesTitle.Text());
        rulesHint.Text(L("statusBar.scenarioOverrides.description"));
        LocalizeTheme(defaultTheme);
        mux::Automation::AutomationProperties::SetName(defaultTheme.combo, defaultThemeTitle.Text());
        for (auto& rule : rules)
        {
            rule->title.Text(L(rule->key));
            rule->enabledRow.SetText(L(rule->key));
            mux::Automation::AutomationProperties::SetName(rule->enabled, rule->title.Text());
            mux::Automation::AutomationProperties::SetHelpText(rule->enabled, rulesHint.Text());
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
mux::UIElement StatusBarPagePresenter::Content() const
{
    impl_->centralizedAppearance = false;
    impl_->defaultThemeTitle.Text(impl_->L("statusBar.defaultAppearance"));
    impl_->SyncAppearanceContext();
    uint32_t index = 0;
    if (!impl_->root.Children().IndexOf(impl_->appearanceRoot, index))
    {
        impl_->root.Children().IndexOf(impl_->relatedAppearance, index);
        impl_->root.Children().InsertAt(index, impl_->appearanceRoot);
    }
    return impl_->root;
}
mux::UIElement StatusBarPagePresenter::AppearanceContent() const
{
    impl_->centralizedAppearance = true;
    impl_->defaultThemeTitle.Text(impl_->L("settings.nav.statusBar"));
    impl_->SyncAppearanceContext();
    uint32_t index = 0;
    if (impl_->root.Children().IndexOf(impl_->appearanceRoot, index))
        impl_->root.Children().RemoveAt(index);
    return impl_->appearanceRoot;
}
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
    (void)focusId;
}
void StatusBarPagePresenter::Deactivate() { impl_->mergedHeight->Flush(); impl_->Flush(); impl_->active = false; }
void StatusBarPagePresenter::Close() { impl_->Close(); }
mux::FrameworkElement StatusBarPagePresenter::FocusTarget(
    std::string_view focusId, SettingsFocusPrerequisite* prerequisite) const
{
    if (prerequisite) *prerequisite = {};
    mux::FrameworkElement result{nullptr};
    RegisterFocusTargets([&](std::string id, const mux::FrameworkElement& target) {
        if (id == focusId) result = target;
    });
    if (!result) return nullptr;
    const auto enablingTarget = [&](bool StatusBarSettings::* member) -> mux::FrameworkElement {
        for (const auto& toggle : impl_->toggles)
        {
            if (toggle->member != member) continue;
            if (prerequisite)
            {
                prerequisite->messageKey = "settings.search.prerequisite.enable";
                prerequisite->controlName = toggle->row.label.Text().c_str();
            }
            // The centralized editor does not mount the behavior-page switch.
            // Keep prerequisite navigation on a visible card in this page.
            return impl_->centralizedAppearance ? impl_->defaultTheme.row.root.as<mux::FrameworkElement>()
                : toggle->control.as<mux::FrameworkElement>();
        }
        return result;
    };
    // Sync disables these rows through their ContentControl hosts. Inspect the
    // same model prerequisites, rather than IsEnabled on the non-Control row.
    if (focusId != "statusBar.enable" && !impl_->value.enabled)
        return enablingTarget(&StatusBarSettings::enabled);
    if (focusId == "statusBar.inputMethodPanel" && !impl_->value.inputMethod)
        return enablingTarget(&StatusBarSettings::inputMethod);
    if (result && focusId == "statusBar.mergedBarHeight" && !impl_->availability.anyMerged && prerequisite)
    {
        // A merged bar depends on both bars' current layout, not just an
        // on/off choice. Keep the existing position target and explain that
        // dependency only when navigation actually requests this setting.
        prerequisite->messageKey = "settings.search.prerequisite.dependency";
        prerequisite->controlName = impl_->edgeRow.label.Text().c_str();
    }
    return result;
}
void StatusBarPagePresenter::RegisterFocusTargets(const std::function<void(std::string, const mux::FrameworkElement&)>& target) const
{
    target("statusBar.position", impl_->edgeRow.root); target("statusBar.monitor", impl_->monitorsRow.root);
    target("statusBar.scale", impl_->scaleRow.root);
    target("statusBar.mergedBarHeight", impl_->availability.anyMerged
        ? impl_->mergedHeight->FocusTarget().as<mux::FrameworkElement>() : impl_->edgeRow.root);
    target("statusBar.clockPanel", impl_->clockPanelRow.root);
    target("statusBar.inputMethodPanel", impl_->inputPanelRow.root);
    target("statusBar.timeNotificationPosition", impl_->timeNotificationPositionRow.root);
    target("statusBar.controlCenterDisplay", impl_->controlDisplayRow.root);
    if (impl_->systemQuickSettings) target("statusBar.controlCenterPanel", impl_->controlPanelRow.root);
    target("statusBar.theme", impl_->defaultTheme.row.root);
    for (auto& rule : impl_->rules) target(rule->focus, rule->enabledRow.root);
    for (auto& toggle : impl_->toggles) target(toggle->focus, toggle->row.root);
}
}
