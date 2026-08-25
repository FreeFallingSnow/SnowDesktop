#include "pch.h"

#include "general_page_presenter.h"
#include "settings_presenter_controls.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
using presenter_controls::SettingRow;

namespace
{

struct SettingsCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

void InitializeCard(
    SettingsCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(12.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.root.Child(card.content);
    page.Children().Append(card.root);
}

struct HotkeySettingRow
{
    SettingRow row;
    muxc::Grid actions{nullptr};
    muxc::Button reset{nullptr};
    winrt::event_token resetToken{};

    void Initialize(HotkeyRecorder& recorder)
    {
        actions = muxc::Grid{};
        actions.ColumnSpacing(8.0);
        muxc::ColumnDefinition recorderColumn{};
        recorderColumn.Width(mux::GridLengthHelper::FromValueAndType(
            1.0, mux::GridUnitType::Star));
        muxc::ColumnDefinition resetColumn{};
        resetColumn.Width(mux::GridLengthHelper::Auto());
        actions.ColumnDefinitions().Append(recorderColumn);
        actions.ColumnDefinitions().Append(resetColumn);
        recorder.Root().HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        reset = muxc::Button{};
        reset.HorizontalAlignment(mux::HorizontalAlignment::Right);
        // The recorder owns a second line for availability/conflict status.
        // Align the reset action with the recorder button instead of centering
        // it against the combined editor-and-status height.
        reset.VerticalAlignment(mux::VerticalAlignment::Top);
        reset.VerticalContentAlignment(mux::VerticalAlignment::Center);
        reset.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
        reset.MinHeight(32.0);
        muxc::Grid::SetColumn(reset, 1);
        actions.Children().Append(recorder.Root());
        actions.Children().Append(reset);
        row.Initialize(actions);
    }
};

HotkeyChord Chord(UINT modifiers, UINT virtualKey) noexcept
{
    return {
        static_cast<std::uint32_t>(modifiers),
        static_cast<std::uint32_t>(virtualKey),
    };
}

} // namespace

struct GeneralPagePresenter::Impl
{
    explicit Impl(LocalizeCallback callback, const mux::Style& style)
        : localize(std::move(callback)), cardStyle(style)
    {
        BuildControls();
        HookEvents();
        BindHotkeyRecorders();
        RefreshLocalizedText();
    }

    LocalizeCallback localize;
    GeneralPageActions actions;
    mux::Style cardStyle{nullptr};
    muxc::StackPanel root{nullptr};
    muxc::StackPanel desktopRoot{nullptr};
    muxc::StackPanel dockShortcutRoot{nullptr};

    SettingsCard startupCard;
    SettingsCard desktopBehaviorCard;
    SettingsCard languageCard;
    SettingsCard quickNavigationCard;
    SettingsCard pageNavigationCard;
    SettingsCard desktopPassthroughCard;
    SettingsCard floatingDockCard;

    muxc::ToggleSwitch autoStartToggle{nullptr};
    muxc::ToggleSwitch softwareDesktopToggle{nullptr};
    muxc::ToggleSwitch doubleClickHideToggle{nullptr};
    muxc::ComboBox languageCombo{nullptr};
    muxc::ToggleSwitch quickNavigationToggle{nullptr};
    muxc::TextBlock quickNavigationHotkeyLabel{nullptr};
    HotkeyRecorder quickNavigationHotkey;
    muxc::ToggleSwitch pageNavigationToggle{nullptr};
    muxc::TextBlock previousPageHotkeyLabel{nullptr};
    HotkeyRecorder previousPageHotkey;
    muxc::TextBlock nextPageHotkeyLabel{nullptr};
    HotkeyRecorder nextPageHotkey;
    muxc::ToggleSwitch desktopPassthroughToggle{nullptr};
    muxc::TextBlock desktopPassthroughHint{nullptr};
    muxc::TextBlock desktopPassthroughHotkeyLabel{nullptr};
    HotkeyRecorder desktopPassthroughHotkey;
    muxc::ToggleSwitch floatingDockToggle{nullptr};
    muxc::TextBlock floatingDockHint{nullptr};
    muxc::TextBlock floatingDockHotkeyLabel{nullptr};
    HotkeyRecorder floatingDockHotkey;

    SettingRow autoStartRow;
    SettingRow softwareDesktopRow;
    SettingRow doubleClickHideRow;
    SettingRow languageRow;
    SettingRow quickNavigationToggleRow;
    HotkeySettingRow quickNavigationHotkeyRow;
    SettingRow pageNavigationToggleRow;
    HotkeySettingRow previousPageHotkeyRow;
    HotkeySettingRow nextPageHotkeyRow;
    SettingRow desktopPassthroughToggleRow;
    HotkeySettingRow desktopPassthroughHotkeyRow;
    SettingRow floatingDockToggleRow;
    HotkeySettingRow floatingDockHotkeyRow;
    muxc::InfoBar portableStartupConflict{nullptr};
    muxc::InfoBar installedStartupConflict{nullptr};
    muxc::Button openPortableStartupSettings{nullptr};
    muxc::Button openInstalledStartupSettings{nullptr};

    std::vector<SettingsLanguageOption> languageOptions;
    std::string selectedLanguage = "system";
    std::uint64_t generation = 0;
    std::uint64_t generalRevision = 0;
    std::uint64_t navigationRevision = 0;
    std::uint64_t dockRevision = 0;
    bool dockEnabled = false;
    bool hasSnapshot = false;
    bool updatingControls = false;
    bool active = false;
    bool closed = false;

    winrt::event_token autoStartToken{};
    winrt::event_token softwareDesktopToken{};
    winrt::event_token doubleClickHideToken{};
    winrt::event_token languageSelectionToken{};
    winrt::event_token quickNavigationToken{};
    winrt::event_token pageNavigationToken{};
    winrt::event_token desktopPassthroughToken{};
    winrt::event_token floatingDockToken{};
    winrt::event_token openPortableStartupSettingsToken{};
    winrt::event_token openInstalledStartupSettingsToken{};

    [[nodiscard]] std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);
        desktopRoot = muxc::StackPanel{};
        desktopRoot.Spacing(8.0);
        dockShortcutRoot = muxc::StackPanel{};
        dockShortcutRoot.Spacing(8.0);

        InitializeCard(startupCard, cardStyle, root);
        autoStartToggle = muxc::ToggleSwitch{};
        autoStartToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        autoStartRow.Initialize(autoStartToggle);
        autoStartRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        startupCard.content.Children().Append(autoStartRow.root);
        portableStartupConflict = muxc::InfoBar{};
        portableStartupConflict.Severity(muxc::InfoBarSeverity::Warning);
        portableStartupConflict.IsClosable(false);
        portableStartupConflict.IsOpen(false);
        openPortableStartupSettings = muxc::Button{};
        portableStartupConflict.ActionButton(openPortableStartupSettings);
        installedStartupConflict = muxc::InfoBar{};
        installedStartupConflict.Severity(muxc::InfoBarSeverity::Warning);
        installedStartupConflict.IsClosable(false);
        installedStartupConflict.IsOpen(false);
        openInstalledStartupSettings = muxc::Button{};
        installedStartupConflict.ActionButton(openInstalledStartupSettings);
        startupCard.content.Children().Append(portableStartupConflict);
        startupCard.content.Children().Append(installedStartupConflict);

        InitializeCard(desktopBehaviorCard, cardStyle, desktopRoot);
        softwareDesktopToggle = muxc::ToggleSwitch{};
        softwareDesktopToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        doubleClickHideToggle = muxc::ToggleSwitch{};
        doubleClickHideToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        softwareDesktopRow.Initialize(softwareDesktopToggle);
        doubleClickHideRow.Initialize(doubleClickHideToggle);
        softwareDesktopRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        doubleClickHideRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        desktopBehaviorCard.content.Children().Append(softwareDesktopRow.root);
        desktopBehaviorCard.content.Children().Append(doubleClickHideRow.root);

        InitializeCard(languageCard, cardStyle, root);
        languageCombo = muxc::ComboBox{};
        languageCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        languageCombo.MaxWidth(520.0);
        languageRow.Initialize(languageCombo);
        languageCard.content.Children().Append(languageRow.root);

        InitializeCard(quickNavigationCard, cardStyle, root);
        quickNavigationToggle = muxc::ToggleSwitch{};
        quickNavigationToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        quickNavigationToggleRow.Initialize(quickNavigationToggle);
        quickNavigationToggleRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        quickNavigationCard.content.Children().Append(
            quickNavigationToggleRow.root);
        quickNavigationHotkeyLabel = muxc::TextBlock{};
        quickNavigationHotkeyRow.Initialize(quickNavigationHotkey);
        quickNavigationCard.content.Children().Append(
            quickNavigationHotkeyRow.row.root);

        InitializeCard(pageNavigationCard, cardStyle, root);
        pageNavigationToggle = muxc::ToggleSwitch{};
        pageNavigationToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        pageNavigationToggleRow.Initialize(pageNavigationToggle);
        pageNavigationToggleRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        pageNavigationCard.content.Children().Append(
            pageNavigationToggleRow.root);
        previousPageHotkeyLabel = muxc::TextBlock{};
        previousPageHotkeyRow.Initialize(previousPageHotkey);
        pageNavigationCard.content.Children().Append(
            previousPageHotkeyRow.row.root);
        nextPageHotkeyLabel = muxc::TextBlock{};
        nextPageHotkeyRow.Initialize(nextPageHotkey);
        pageNavigationCard.content.Children().Append(
            nextPageHotkeyRow.row.root);

        InitializeCard(desktopPassthroughCard, cardStyle, desktopRoot);
        desktopPassthroughToggle = muxc::ToggleSwitch{};
        desktopPassthroughToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        desktopPassthroughHint = muxc::TextBlock{};
        desktopPassthroughHint.Opacity(0.72);
        desktopPassthroughHint.TextWrapping(mux::TextWrapping::Wrap);
        desktopPassthroughToggleRow.Initialize(desktopPassthroughToggle);
        desktopPassthroughToggleRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        desktopPassthroughCard.content.Children().Append(
            desktopPassthroughToggleRow.root);
        desktopPassthroughHotkeyLabel = muxc::TextBlock{};
        desktopPassthroughHotkeyRow.Initialize(desktopPassthroughHotkey);
        desktopPassthroughCard.content.Children().Append(
            desktopPassthroughHotkeyRow.row.root);
        InitializeCard(floatingDockCard, cardStyle, dockShortcutRoot);
        floatingDockToggle = muxc::ToggleSwitch{};
        floatingDockToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Right);
        floatingDockHint = muxc::TextBlock{};
        floatingDockHint.Opacity(0.72);
        floatingDockHint.TextWrapping(mux::TextWrapping::Wrap);
        floatingDockToggleRow.Initialize(floatingDockToggle);
        floatingDockToggleRow.SetControlAlignment(
            mux::HorizontalAlignment::Right);
        floatingDockCard.content.Children().Append(floatingDockToggleRow.root);
        floatingDockHotkeyLabel = muxc::TextBlock{};
        floatingDockHotkeyRow.Initialize(floatingDockHotkey);
        floatingDockCard.content.Children().Append(
            floatingDockHotkeyRow.row.root);
    }

    template <typename Edit>
    void CommitGeneral(Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.commitGeneral)
        {
            actions.commitGeneral(generation,
                GeneralPageActions::GeneralEdit(std::move(edit)));
        }
    }

    template <typename Edit>
    void CommitNavigation(Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.commitNavigation)
        {
            actions.commitNavigation(generation,
                GeneralPageActions::NavigationEdit(std::move(edit)));
        }
    }

    template <typename Edit>
    void CommitDock(Edit edit)
    {
        if (!closed && active && hasSnapshot && !updatingControls &&
            actions.commitDock)
        {
            actions.commitDock(generation,
                GeneralPageActions::DockEdit(std::move(edit)));
        }
    }

    void HookEvents()
    {
        autoStartToken = autoStartToggle.Toggled(
            [this](const auto&, const auto&) {
                if (closed || updatingControls || !active || !hasSnapshot ||
                    !actions.setAutoStart)
                {
                    return;
                }
                const bool enabled = autoStartToggle.IsOn();
                actions.setAutoStart(generation, enabled);
            });
        const auto openStartupApps = [this](const auto&, const auto&) {
            if (!closed && active && hasSnapshot &&
                actions.openStartupAppsSettings)
            {
                actions.openStartupAppsSettings(generation);
            }
        };
        openPortableStartupSettingsToken =
            openPortableStartupSettings.Click(openStartupApps);
        openInstalledStartupSettingsToken =
            openInstalledStartupSettings.Click(openStartupApps);
        softwareDesktopToken = softwareDesktopToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = softwareDesktopToggle.IsOn();
                CommitGeneral([enabled](GeneralSettings& settings) {
                    settings.softwareDesktopEnabled = enabled;
                });
            });
        doubleClickHideToken = doubleClickHideToggle.Toggled(
            [this](const auto&, const auto&) {
                const bool enabled = doubleClickHideToggle.IsOn();
                CommitGeneral([enabled](GeneralSettings& settings) {
                    settings.doubleClickHideDesktop = enabled;
                });
            });
        languageSelectionToken = languageCombo.SelectionChanged(
            [this](const auto&, const auto&) {
                if (updatingControls || closed || !active || !hasSnapshot)
                    return;
                const int index = languageCombo.SelectedIndex();
                if (index < 0 ||
                    static_cast<std::size_t>(index) >= languageOptions.size())
                    return;
                const std::string code =
                    languageOptions[static_cast<std::size_t>(index)].code;
                selectedLanguage = code;
                CommitGeneral([code](GeneralSettings& settings) {
                    strncpy_s(settings.language, sizeof(settings.language),
                        code.c_str(), _TRUNCATE);
                });
            });
        quickNavigationToken = quickNavigationToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentEnabledStates();
                const bool enabled = quickNavigationToggle.IsOn();
                CommitNavigation([enabled](NavigationSettings& settings) {
                    settings.enabled = enabled;
                });
            });
        pageNavigationToken = pageNavigationToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentEnabledStates();
                const bool enabled = pageNavigationToggle.IsOn();
                CommitGeneral([enabled](GeneralSettings& settings) {
                    settings.pageNavigationKeyboardEnabled = enabled;
                });
            });
        desktopPassthroughToken = desktopPassthroughToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentEnabledStates();
                const bool enabled = desktopPassthroughToggle.IsOn();
                CommitGeneral([enabled](GeneralSettings& settings) {
                    settings.desktopPassthroughHotkeyEnabled = enabled;
                });
            });
        floatingDockToken = floatingDockToggle.Toggled(
            [this](const auto&, const auto&) {
                UpdateDependentEnabledStates();
                const bool enabled = floatingDockToggle.IsOn();
                CommitDock([enabled](DockSettings& settings) {
                    settings.floatingShortcutMode = enabled;
                });
            });
        quickNavigationHotkeyRow.resetToken =
            quickNavigationHotkeyRow.reset.Click(
                [this](const auto&, const auto&) {
                    CommitNavigation([](NavigationSettings& settings) {
                        settings.modifiers = MOD_CONTROL | MOD_ALT;
                        settings.virtualKey = VK_SPACE;
                    });
                });
        previousPageHotkeyRow.resetToken =
            previousPageHotkeyRow.reset.Click(
                [this](const auto&, const auto&) {
                    CommitGeneral([](GeneralSettings& settings) {
                        settings.pageNavigationPreviousModifiers = 0;
                        settings.pageNavigationPreviousVirtualKey = VK_PRIOR;
                    });
                });
        nextPageHotkeyRow.resetToken = nextPageHotkeyRow.reset.Click(
            [this](const auto&, const auto&) {
                CommitGeneral([](GeneralSettings& settings) {
                    settings.pageNavigationNextModifiers = 0;
                    settings.pageNavigationNextVirtualKey = VK_NEXT;
                });
            });
        desktopPassthroughHotkeyRow.resetToken =
            desktopPassthroughHotkeyRow.reset.Click(
                [this](const auto&, const auto&) {
                    CommitGeneral([](GeneralSettings& settings) {
                        settings.desktopPassthroughHotkeyModifiers =
                            MOD_CONTROL | MOD_ALT;
                        settings.desktopPassthroughHotkeyVirtualKey =
                            VK_OEM_3;
                    });
                });
        floatingDockHotkeyRow.resetToken =
            floatingDockHotkeyRow.reset.Click(
                [this](const auto&, const auto&) {
                    CommitDock([](DockSettings& settings) {
                        settings.floatingHotkeyModifiers =
                            MOD_CONTROL | MOD_ALT;
                        settings.floatingHotkeyVirtualKey = 'D';
                    });
                });
    }

    void BindHotkeyRecorder(
        HotkeyRecorder& recorder,
        SettingsHostActions::HotkeyTarget target,
        HotkeyRecorder::CommittedCallback committed)
    {
        recorder.SetAvailabilityProbe(
            [this, target](HotkeyChord chord,
                           std::uint64_t expectedGeneration,
                           std::uint64_t requestId,
                           HotkeyRecorder::AvailabilityCompletion completion) {
                if (closed || !active || !hasSnapshot ||
                    expectedGeneration != generation ||
                    !actions.probeHotkey)
                {
                    completion(false,
                        L("app.settings.hotkey_status_in_use") + L" — " +
                            L("app.settings.hotkey_conflict_system"));
                    return;
                }
                actions.probeHotkey(target, chord, expectedGeneration,
                    requestId, std::move(completion));
            });
        recorder.SetCommittedCallback(std::move(committed));
    }

    void BindHotkeyRecorders()
    {
        BindHotkeyRecorder(quickNavigationHotkey,
            SettingsHostActions::HotkeyTarget::QuickNavigation,
            [this](HotkeyChord chord) {
                CommitNavigation([chord](NavigationSettings& settings) {
                    settings.modifiers = chord.modifiers;
                    settings.virtualKey = chord.virtualKey;
                });
            });
        BindHotkeyRecorder(previousPageHotkey,
            SettingsHostActions::HotkeyTarget::PagePrevious,
            [this](HotkeyChord chord) {
                CommitGeneral([chord](GeneralSettings& settings) {
                    settings.pageNavigationPreviousModifiers = chord.modifiers;
                    settings.pageNavigationPreviousVirtualKey =
                        chord.virtualKey;
                });
            });
        BindHotkeyRecorder(nextPageHotkey,
            SettingsHostActions::HotkeyTarget::PageNext,
            [this](HotkeyChord chord) {
                CommitGeneral([chord](GeneralSettings& settings) {
                    settings.pageNavigationNextModifiers = chord.modifiers;
                    settings.pageNavigationNextVirtualKey = chord.virtualKey;
                });
            });
        BindHotkeyRecorder(desktopPassthroughHotkey,
            SettingsHostActions::HotkeyTarget::DesktopPassthrough,
            [this](HotkeyChord chord) {
                CommitGeneral([chord](GeneralSettings& settings) {
                    settings.desktopPassthroughHotkeyModifiers =
                        chord.modifiers;
                    settings.desktopPassthroughHotkeyVirtualKey =
                        chord.virtualKey;
                });
            });
        BindHotkeyRecorder(floatingDockHotkey,
            SettingsHostActions::HotkeyTarget::FloatingDock,
            [this](HotkeyChord chord) {
                CommitDock([chord](DockSettings& settings) {
                    settings.floatingHotkeyModifiers = chord.modifiers;
                    settings.floatingHotkeyVirtualKey = chord.virtualKey;
                });
            });
    }

    void PatchGeneral(const GeneralSettings& settings)
    {
        autoStartToggle.IsOn(settings.autoStartEnabled);
        softwareDesktopToggle.IsOn(settings.softwareDesktopEnabled);
        doubleClickHideToggle.IsOn(settings.doubleClickHideDesktop);
        pageNavigationToggle.IsOn(
            settings.pageNavigationKeyboardEnabled);
        previousPageHotkey.SetValue(Chord(
            settings.pageNavigationPreviousModifiers,
            settings.pageNavigationPreviousVirtualKey), generation);
        nextPageHotkey.SetValue(Chord(
            settings.pageNavigationNextModifiers,
            settings.pageNavigationNextVirtualKey), generation);
        desktopPassthroughToggle.IsOn(
            settings.desktopPassthroughHotkeyEnabled);
        desktopPassthroughHotkey.SetValue(Chord(
            settings.desktopPassthroughHotkeyModifiers,
            settings.desktopPassthroughHotkeyVirtualKey), generation);
        dockEnabled = settings.dockEnabled;
        selectedLanguage = settings.language;
        SelectLanguage();
    }

    void PatchNavigation(const NavigationSettings& settings)
    {
        quickNavigationToggle.IsOn(settings.enabled);
        quickNavigationHotkey.SetValue(
            Chord(settings.modifiers, settings.virtualKey), generation);
    }

    void PatchDock(const DockSettings& settings)
    {
        floatingDockToggle.IsOn(settings.floatingShortcutMode);
        floatingDockHotkey.SetValue(Chord(
            settings.floatingHotkeyModifiers,
            settings.floatingHotkeyVirtualKey), generation);
    }

    void SetRecorderEnabled(
        HotkeyRecorder& recorder,
        bool enabled,
        bool localDesktopHotkey = false)
    {
        if (!enabled && recorder.IsCapturing())
            recorder.CancelCapture();
        recorder.SetValidationContext(enabled, localDesktopHotkey);
        recorder.Root().IsEnabled(enabled);
    }

    void UpdateDependentEnabledStates()
    {
        UpdateConditionalHintVisibility();
        SetRecorderEnabled(
            quickNavigationHotkey, quickNavigationToggle.IsOn());
        quickNavigationHotkeyRow.row.SetEnabled(
            quickNavigationToggle.IsOn());
        SetRecorderEnabled(
            previousPageHotkey, pageNavigationToggle.IsOn(), true);
        SetRecorderEnabled(
            nextPageHotkey, pageNavigationToggle.IsOn(), true);
        previousPageHotkeyRow.row.SetEnabled(pageNavigationToggle.IsOn());
        nextPageHotkeyRow.row.SetEnabled(pageNavigationToggle.IsOn());
        SetRecorderEnabled(desktopPassthroughHotkey,
            desktopPassthroughToggle.IsOn());
        desktopPassthroughHotkeyRow.row.SetEnabled(
            desktopPassthroughToggle.IsOn());
        floatingDockToggle.IsEnabled(dockEnabled);
        floatingDockToggleRow.SetEnabled(dockEnabled);
        SetRecorderEnabled(floatingDockHotkey,
            dockEnabled && floatingDockToggle.IsOn());
        floatingDockHotkeyRow.row.SetEnabled(
            dockEnabled && floatingDockToggle.IsOn());
    }

    void UpdateConditionalHintVisibility()
    {
        desktopPassthroughToggleRow.help.Visibility(
            desktopPassthroughToggle.IsOn()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        floatingDockToggleRow.help.Visibility(
            floatingDockToggle.IsOn()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
    }

    void SelectLanguage()
    {
        int selectedIndex = 0;
        for (std::size_t index = 0; index < languageOptions.size(); ++index)
        {
            if (languageOptions[index].code == selectedLanguage)
            {
                selectedIndex = static_cast<int>(index);
                break;
            }
        }
        languageCombo.SelectedIndex(selectedIndex);
    }

    void RebuildLanguageOptions()
    {
        languageOptions = actions.languageCatalog
            ? actions.languageCatalog()
            : std::vector<SettingsLanguageOption>{};
        const auto system = std::find_if(
            languageOptions.begin(), languageOptions.end(),
            [](const SettingsLanguageOption& option) {
                return option.code == "system";
            });
        if (system == languageOptions.end())
        {
            languageOptions.insert(languageOptions.begin(),
                {"system", L("app.settings.language_system")});
        }
        languageCombo.Items().Clear();
        for (const auto& option : languageOptions)
            languageCombo.Items().Append(winrt::box_value(option.label));
        SelectLanguage();
    }

    HotkeyRecorderText HotkeyText(std::wstring automationName) const
    {
        HotkeyRecorderText text;
        text.automationName = std::move(automationName);
        text.idleHint = L("app.settings.hotkey_capture_help");
        text.captureHint = L("app.settings.hotkey_press");
        text.checking = L("app.settings.hotkey_status_recording");
        text.available = L("app.settings.hotkey_available");
        text.conflict = L("app.settings.hotkey_status_conflict");
        text.cleared = L("app.settings.hotkey_not_set");
        text.none = L("app.settings.hotkey_not_set");
        text.disabled = L("app.settings.hotkey_status_disabled");
        text.captureActive = L("app.settings.hotkey_capture_active");
        text.notSetWarning = L("app.settings.hotkey_not_set_warning");
        text.availableStatus = L("app.settings.hotkey_status_available");
        text.inUseStatus = L("app.settings.hotkey_status_in_use");
        text.noModifierStatus =
            L("app.settings.hotkey_status_no_modifier");
        text.noModifierWarning =
            L("app.settings.hotkey_no_modifier_warning");
        text.systemConflict = L("app.settings.hotkey_conflict_system");
        return text;
    }

    void SetCardText(SettingsCard& card, std::string_view key)
    {
        card.title.Text(L(key));
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
    }

    void RefreshLocalizedText()
    {
        if (closed) return;
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        SetCardText(startupCard, "settings.general.startup");
        SetCardText(desktopBehaviorCard, "settings.desktop.behavior");
        SetCardText(languageCard, "app.settings.language");
        SetCardText(quickNavigationCard, "app.settings.quick_navigation");
        SetCardText(pageNavigationCard,
            "settings.general.pageNavigation");
        SetCardText(desktopPassthroughCard,
            "settings.desktop.passthrough");
        SetCardText(floatingDockCard,
            "settings.dock.floatingShortcut");

        autoStartRow.SetText(L("app.settings.auto_start"));
        softwareDesktopRow.SetText(L("app.settings.software_desktop"));
        doubleClickHideRow.SetText(L("app.settings.double_click_hide"));
        languageRow.SetText(L("app.settings.language"));
        quickNavigationToggleRow.SetText(
            L("app.settings.enable_global_navigation"));
        quickNavigationHotkeyRow.row.SetText(
            L("app.settings.hotkey"),
            L("app.settings.hotkey_capture_help"));
        pageNavigationToggleRow.SetText(
            L("app.settings.page_navigation_keyboard"));
        previousPageHotkeyRow.row.SetText(
            L("app.settings.page_navigation_previous"),
            L("app.settings.hotkey_capture_help"));
        nextPageHotkeyRow.row.SetText(
            L("app.settings.page_navigation_next"),
            L("app.settings.hotkey_capture_help"));
        desktopPassthroughToggleRow.SetText(
            L("app.settings.desktop_passthrough_hotkey"),
            L("app.settings.desktop_passthrough_hotkey_hint"));
        desktopPassthroughHotkeyRow.row.SetText(
            L("app.settings.hotkey"),
            L("app.settings.hotkey_capture_help"));
        floatingDockToggleRow.SetText(
            L("app.dock.floating_shortcut_mode"),
            L("app.dock.floating_shortcut_hint"));
        floatingDockHotkeyRow.row.SetText(
            L("app.settings.hotkey"),
            L("app.settings.hotkey_capture_help"));

        const std::wstring restoreDefault =
            L("app.settings.restore_default");
        for (const auto& button : {
                 quickNavigationHotkeyRow.reset,
                 previousPageHotkeyRow.reset,
                 nextPageHotkeyRow.reset,
                 desktopPassthroughHotkeyRow.reset,
                 floatingDockHotkeyRow.reset})
        {
            presenter_controls::ConfigureRestoreDefaultButton(
                button, restoreDefault);
        }

        quickNavigationHotkey.SetText(HotkeyText(
            L("app.settings.quick_navigation") + L" — " +
                L("app.settings.hotkey")));
        previousPageHotkey.SetText(HotkeyText(
            L("app.settings.page_navigation_previous")));
        nextPageHotkey.SetText(HotkeyText(
            L("app.settings.page_navigation_next")));
        desktopPassthroughHotkey.SetText(HotkeyText(
            L("app.settings.desktop_passthrough_hotkey")));
        floatingDockHotkey.SetText(HotkeyText(
            L("app.dock.floating_shortcut_mode")));

        muxa::AutomationProperties::SetName(
            autoStartToggle, autoStartRow.label.Text());
        muxa::AutomationProperties::SetName(
            softwareDesktopToggle, softwareDesktopRow.label.Text());
        muxa::AutomationProperties::SetName(
            doubleClickHideToggle, doubleClickHideRow.label.Text());
        muxa::AutomationProperties::SetName(
            quickNavigationToggle, quickNavigationToggleRow.label.Text());
        muxa::AutomationProperties::SetName(
            pageNavigationToggle, pageNavigationToggleRow.label.Text());
        muxa::AutomationProperties::SetName(
            desktopPassthroughToggle,
            desktopPassthroughToggleRow.label.Text());
        muxa::AutomationProperties::SetName(
            floatingDockToggle, floatingDockToggleRow.label.Text());
        muxa::AutomationProperties::SetName(
            languageCombo, L("app.settings.language"));

        const auto openStartupSettings = winrt::box_value(
            L("app.settings.auto_start_open_windows_settings"));
        openPortableStartupSettings.Content(openStartupSettings);
        openInstalledStartupSettings.Content(openStartupSettings);
        muxa::AutomationProperties::SetName(openPortableStartupSettings,
            L("app.settings.auto_start_open_windows_settings"));
        muxa::AutomationProperties::SetName(openInstalledStartupSettings,
            L("app.settings.auto_start_open_windows_settings"));

        RebuildLanguageOptions();
        RefreshStartupConflict();
        UpdateConditionalHintVisibility();
        updatingControls = wasUpdating;
    }

    void RefreshStartupConflict()
    {
        GeneralStartupConflict conflict;
        if (actions.queryStartupConflict)
            conflict = actions.queryStartupConflict();

        const bool showPortableConflict = conflict.kind ==
            GeneralStartupConflictKind::PortableVersionOwnsStartup;
        const bool showInstalledConflict = conflict.kind ==
            GeneralStartupConflictKind::InstalledVersionOwnsStartup;
        portableStartupConflict.Visibility(showPortableConflict
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        installedStartupConflict.Visibility(showInstalledConflict
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        portableStartupConflict.IsOpen(showPortableConflict);
        installedStartupConflict.IsOpen(showInstalledConflict);

        std::wstring portableMessage =
            L("app.settings.auto_start_other_version");
        if (const auto marker = portableMessage.find(L"{0}");
            marker != std::wstring::npos)
        {
            portableMessage.replace(marker, 3, conflict.ownerCommand);
        }
        portableStartupConflict.Message(portableMessage);
        installedStartupConflict.Message(
            L("app.settings.auto_start_installed_version_active"));
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed) return;
        const bool newGeneration =
            !hasSnapshot || snapshot.generation != generation;
        const bool generalChanged = newGeneration ||
            snapshot.domainRevisions.general != generalRevision;
        generation = snapshot.generation;
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        if (generalChanged)
        {
            PatchGeneral(snapshot.values.general);
            generalRevision = snapshot.domainRevisions.general;
        }
        if (newGeneration ||
            snapshot.domainRevisions.navigation != navigationRevision)
        {
            PatchNavigation(snapshot.values.navigation);
            navigationRevision = snapshot.domainRevisions.navigation;
        }
        if (newGeneration ||
            snapshot.domainRevisions.dock != dockRevision)
        {
            PatchDock(snapshot.values.dock);
            dockRevision = snapshot.domainRevisions.dock;
        }
        hasSnapshot = true;
        UpdateDependentEnabledStates();
        if (generalChanged)
            RefreshStartupConflict();
        updatingControls = wasUpdating;
    }

    void CancelCaptures() noexcept
    {
        try
        {
            for (HotkeyRecorder* recorder : {
                     &quickNavigationHotkey,
                     &previousPageHotkey,
                     &nextPageHotkey,
                     &desktopPassthroughHotkey,
                     &floatingDockHotkey})
            {
                recorder->CancelCapture();
            }
        }
        catch (...)
        {
        }
    }

    void Close() noexcept
    {
        if (closed) return;
        active = false;
        CancelCaptures();
        closed = true;
        try
        {
            autoStartToggle.Toggled(autoStartToken);
            softwareDesktopToggle.Toggled(softwareDesktopToken);
            doubleClickHideToggle.Toggled(doubleClickHideToken);
            languageCombo.SelectionChanged(languageSelectionToken);
            quickNavigationToggle.Toggled(quickNavigationToken);
            pageNavigationToggle.Toggled(pageNavigationToken);
            desktopPassthroughToggle.Toggled(desktopPassthroughToken);
            floatingDockToggle.Toggled(floatingDockToken);
            quickNavigationHotkeyRow.reset.Click(
                quickNavigationHotkeyRow.resetToken);
            previousPageHotkeyRow.reset.Click(
                previousPageHotkeyRow.resetToken);
            nextPageHotkeyRow.reset.Click(nextPageHotkeyRow.resetToken);
            desktopPassthroughHotkeyRow.reset.Click(
                desktopPassthroughHotkeyRow.resetToken);
            floatingDockHotkeyRow.reset.Click(
                floatingDockHotkeyRow.resetToken);
            openPortableStartupSettings.Click(
                openPortableStartupSettingsToken);
            openInstalledStartupSettings.Click(
                openInstalledStartupSettingsToken);
        }
        catch (...)
        {
        }
        quickNavigationHotkey.Close();
        previousPageHotkey.Close();
        nextPageHotkey.Close();
        desktopPassthroughHotkey.Close();
        floatingDockHotkey.Close();
        actions = {};
        localize = {};
    }
};

GeneralPagePresenter::GeneralPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle)
    : impl_(std::make_unique<Impl>(std::move(localize), cardStyle))
{
}

GeneralPagePresenter::~GeneralPagePresenter()
{
    Close();
}

void GeneralPagePresenter::SetActions(GeneralPageActions actions)
{
    if (!impl_ || impl_->closed) return;
    impl_->actions = std::move(actions);
    impl_->RefreshLocalizedText();
}

muxc::StackPanel GeneralPagePresenter::Root() const noexcept
{
    return impl_ ? impl_->root : nullptr;
}

muxc::StackPanel GeneralPagePresenter::DesktopBehaviorContent() const noexcept
{
    return impl_ ? impl_->desktopRoot : nullptr;
}

muxc::StackPanel GeneralPagePresenter::DockShortcutContent() const noexcept
{
    return impl_ ? impl_->dockShortcutRoot : nullptr;
}

void GeneralPagePresenter::ApplySnapshot(const SettingsSnapshot& snapshot)
{
    if (impl_) impl_->ApplySnapshot(snapshot);
}

void GeneralPagePresenter::RefreshLocalizedText()
{
    if (impl_) impl_->RefreshLocalizedText();
}

void GeneralPagePresenter::RegisterFocusTargets(
    const FocusRegistrar& registrar) const
{
    if (!impl_ || !registrar) return;
    const auto registerAliases = [&registrar](
        const mux::FrameworkElement& element,
        std::initializer_list<std::string_view> ids) {
        for (const auto id : ids)
            registrar(std::string(id), element);
    };

    registerAliases(impl_->autoStartToggle,
        {"general.startup", "general.autoStart"});
    registerAliases(impl_->softwareDesktopToggle,
        {"desktop.softwareDesktop", "general.softwareDesktop"});
    registerAliases(impl_->languageCombo, {"general.language"});
    registerAliases(impl_->quickNavigationToggle,
        {"general.hotkeys", "general.quickNavigation",
            "general.quickNavigation.enabled"});
    registerAliases(impl_->quickNavigationHotkey.FocusTarget(),
        {"general.quickNavigation.hotkey"});
    registerAliases(impl_->pageNavigationToggle,
        {"general.navigation", "general.pageNavigation",
            "general.pageNavigation.enabled"});
    registerAliases(impl_->previousPageHotkey.FocusTarget(),
        {"general.pageNavigation.previous"});
    registerAliases(impl_->nextPageHotkey.FocusTarget(),
        {"general.pageNavigation.next"});
    registerAliases(impl_->desktopPassthroughToggle,
        {"desktop.passthrough", "general.desktopPassthrough",
            "general.desktopPassthrough.enabled"});
    registerAliases(impl_->desktopPassthroughHotkey.FocusTarget(),
        {"desktop.passthrough.hotkey",
            "general.desktopPassthrough.hotkey"});
    registerAliases(impl_->floatingDockToggle,
        {"dock.floatingShortcutMode", "general.floatingDock",
            "general.floatingDock.enabled"});
    registerAliases(impl_->floatingDockHotkey.FocusTarget(),
        {"dock.floatingShortcutMode.hotkey",
            "general.floatingDock.hotkey"});
    registerAliases(impl_->doubleClickHideToggle,
        {"desktop.doubleClickHide", "general.doubleClickHide"});
}

void GeneralPagePresenter::Activate() noexcept
{
    if (!impl_ || impl_->closed) return;
    impl_->active = true;
    try
    {
        // Snapshots are applied before the route presenter is activated.
        // Re-probe every saved chord now that the generation gate is live.
        impl_->UpdateDependentEnabledStates();
        impl_->RefreshStartupConflict();
    }
    catch (...)
    {
    }
}

void GeneralPagePresenter::Deactivate() noexcept
{
    if (!impl_ || impl_->closed) return;
    impl_->active = false;
    impl_->CancelCaptures();
}

bool GeneralPagePresenter::IsHotkeyCaptureActive() const noexcept
{
    if (!impl_ || impl_->closed || !impl_->active) return false;
    return impl_->quickNavigationHotkey.IsCapturing() ||
        impl_->previousPageHotkey.IsCapturing() ||
        impl_->nextPageHotkey.IsCapturing() ||
        impl_->desktopPassthroughHotkey.IsCapturing() ||
        impl_->floatingDockHotkey.IsCapturing();
}

void GeneralPagePresenter::CaptureRegisteredHotkey(
    std::uint32_t modifiers,
    std::uint32_t virtualKey)
{
    if (!impl_ || impl_->closed || !impl_->active) return;
    for (HotkeyRecorder* recorder : {
             &impl_->quickNavigationHotkey,
             &impl_->previousPageHotkey,
             &impl_->nextPageHotkey,
             &impl_->desktopPassthroughHotkey,
             &impl_->floatingDockHotkey})
    {
        if (recorder->IsCapturing())
        {
            recorder->CaptureRegisteredHotkey(modifiers, virtualKey);
            return;
        }
    }
}

void GeneralPagePresenter::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui
