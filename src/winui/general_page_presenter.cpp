#include "pch.h"

#include "general_page_presenter.h"

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

void AppendHotkey(
    const SettingsCard& card,
    const muxc::TextBlock& label,
    HotkeyRecorder& recorder)
{
    label.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(label);
    recorder.Root().HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    card.content.Children().Append(recorder.Root());
}

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

    SettingsCard startupCard;
    SettingsCard languageCard;
    SettingsCard quickNavigationCard;
    SettingsCard pageNavigationCard;
    SettingsCard desktopPassthroughCard;
    SettingsCard floatingDockCard;

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

    winrt::event_token softwareDesktopToken{};
    winrt::event_token doubleClickHideToken{};
    winrt::event_token languageSelectionToken{};
    winrt::event_token quickNavigationToken{};
    winrt::event_token pageNavigationToken{};
    winrt::event_token desktopPassthroughToken{};
    winrt::event_token floatingDockToken{};

    [[nodiscard]] std::wstring L(std::string_view key) const
    {
        return localize ? localize(key) : std::wstring{};
    }

    void BuildControls()
    {
        root = muxc::StackPanel{};
        root.Spacing(8.0);

        InitializeCard(startupCard, cardStyle, root);
        softwareDesktopToggle = muxc::ToggleSwitch{};
        softwareDesktopToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        doubleClickHideToggle = muxc::ToggleSwitch{};
        doubleClickHideToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        startupCard.content.Children().Append(softwareDesktopToggle);
        startupCard.content.Children().Append(doubleClickHideToggle);

        InitializeCard(languageCard, cardStyle, root);
        languageCombo = muxc::ComboBox{};
        languageCombo.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        languageCombo.MaxWidth(520.0);
        languageCard.content.Children().Append(languageCombo);

        InitializeCard(quickNavigationCard, cardStyle, root);
        quickNavigationToggle = muxc::ToggleSwitch{};
        quickNavigationToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        quickNavigationCard.content.Children().Append(quickNavigationToggle);
        quickNavigationHotkeyLabel = muxc::TextBlock{};
        AppendHotkey(quickNavigationCard, quickNavigationHotkeyLabel,
            quickNavigationHotkey);

        InitializeCard(pageNavigationCard, cardStyle, root);
        pageNavigationToggle = muxc::ToggleSwitch{};
        pageNavigationToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        pageNavigationCard.content.Children().Append(pageNavigationToggle);
        previousPageHotkeyLabel = muxc::TextBlock{};
        AppendHotkey(pageNavigationCard, previousPageHotkeyLabel,
            previousPageHotkey);
        nextPageHotkeyLabel = muxc::TextBlock{};
        AppendHotkey(pageNavigationCard, nextPageHotkeyLabel,
            nextPageHotkey);

        InitializeCard(desktopPassthroughCard, cardStyle, root);
        desktopPassthroughToggle = muxc::ToggleSwitch{};
        desktopPassthroughToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        desktopPassthroughHint = muxc::TextBlock{};
        desktopPassthroughHint.Opacity(0.72);
        desktopPassthroughHint.TextWrapping(mux::TextWrapping::Wrap);
        desktopPassthroughCard.content.Children().Append(
            desktopPassthroughToggle);
        desktopPassthroughCard.content.Children().Append(
            desktopPassthroughHint);
        desktopPassthroughHotkeyLabel = muxc::TextBlock{};
        AppendHotkey(desktopPassthroughCard,
            desktopPassthroughHotkeyLabel, desktopPassthroughHotkey);

        InitializeCard(floatingDockCard, cardStyle, root);
        floatingDockToggle = muxc::ToggleSwitch{};
        floatingDockToggle.HorizontalAlignment(
            mux::HorizontalAlignment::Stretch);
        floatingDockHint = muxc::TextBlock{};
        floatingDockHint.Opacity(0.72);
        floatingDockHint.TextWrapping(mux::TextWrapping::Wrap);
        floatingDockCard.content.Children().Append(floatingDockToggle);
        floatingDockCard.content.Children().Append(floatingDockHint);
        floatingDockHotkeyLabel = muxc::TextBlock{};
        AppendHotkey(floatingDockCard, floatingDockHotkeyLabel,
            floatingDockHotkey);
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

    void SetRecorderEnabled(HotkeyRecorder& recorder, bool enabled)
    {
        if (!enabled && recorder.IsCapturing())
            recorder.CancelCapture();
        recorder.Root().IsEnabled(enabled);
    }

    void UpdateDependentEnabledStates()
    {
        SetRecorderEnabled(
            quickNavigationHotkey, quickNavigationToggle.IsOn());
        SetRecorderEnabled(
            previousPageHotkey, pageNavigationToggle.IsOn());
        SetRecorderEnabled(
            nextPageHotkey, pageNavigationToggle.IsOn());
        SetRecorderEnabled(desktopPassthroughHotkey,
            desktopPassthroughToggle.IsOn());
        floatingDockToggle.IsEnabled(dockEnabled);
        SetRecorderEnabled(floatingDockHotkey,
            dockEnabled && floatingDockToggle.IsOn());
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

        SetCardText(startupCard, "app.settings.software_desktop");
        SetCardText(languageCard, "app.settings.language");
        SetCardText(quickNavigationCard, "app.settings.quick_navigation");
        SetCardText(pageNavigationCard,
            "app.settings.page_navigation_keyboard");
        SetCardText(desktopPassthroughCard,
            "app.settings.desktop_passthrough_hotkey");
        SetCardText(floatingDockCard,
            "app.dock.floating_shortcut_mode");

        softwareDesktopToggle.Header(winrt::box_value(
            L("app.settings.software_desktop")));
        doubleClickHideToggle.Header(winrt::box_value(
            L("app.settings.double_click_hide")));
        languageCombo.Header(winrt::box_value(L("app.settings.language")));
        quickNavigationToggle.Header(winrt::box_value(
            L("app.settings.enable_global_navigation")));
        pageNavigationToggle.Header(winrt::box_value(
            L("app.settings.page_navigation_keyboard")));
        desktopPassthroughToggle.Header(winrt::box_value(
            L("app.settings.desktop_passthrough_hotkey")));
        desktopPassthroughHint.Text(
            L("app.settings.desktop_passthrough_hotkey_hint"));
        floatingDockToggle.Header(winrt::box_value(
            L("app.dock.floating_shortcut_mode")));
        floatingDockHint.Text(L("app.dock.floating_shortcut_hint"));

        quickNavigationHotkeyLabel.Text(L("app.settings.hotkey"));
        previousPageHotkeyLabel.Text(
            L("app.settings.page_navigation_previous"));
        nextPageHotkeyLabel.Text(L("app.settings.page_navigation_next"));
        desktopPassthroughHotkeyLabel.Text(L("app.settings.hotkey"));
        floatingDockHotkeyLabel.Text(L("app.settings.hotkey"));

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

        for (const auto& control : {
                 softwareDesktopToggle, doubleClickHideToggle,
                 quickNavigationToggle, pageNavigationToggle,
                 desktopPassthroughToggle, floatingDockToggle})
        {
            muxa::AutomationProperties::SetName(
                control, winrt::unbox_value_or<winrt::hstring>(
                    control.Header(), winrt::hstring{}));
        }
        muxa::AutomationProperties::SetName(
            languageCombo, L("app.settings.language"));

        RebuildLanguageOptions();
        updatingControls = wasUpdating;
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed) return;
        const bool newGeneration =
            !hasSnapshot || snapshot.generation != generation;
        generation = snapshot.generation;
        const bool wasUpdating = updatingControls;
        updatingControls = true;

        if (newGeneration ||
            snapshot.domainRevisions.general != generalRevision)
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
            softwareDesktopToggle.Toggled(softwareDesktopToken);
            doubleClickHideToggle.Toggled(doubleClickHideToken);
            languageCombo.SelectionChanged(languageSelectionToken);
            quickNavigationToggle.Toggled(quickNavigationToken);
            pageNavigationToggle.Toggled(pageNavigationToken);
            desktopPassthroughToggle.Toggled(desktopPassthroughToken);
            floatingDockToggle.Toggled(floatingDockToken);
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

    registerAliases(impl_->softwareDesktopToggle,
        {"general.startup", "general.softwareDesktop"});
    registerAliases(impl_->languageCombo, {"general.language"});
    registerAliases(impl_->quickNavigationToggle,
        {"general.hotkeys", "general.quickNavigation.enabled"});
    registerAliases(impl_->quickNavigationHotkey.FocusTarget(),
        {"general.quickNavigation.hotkey"});
    registerAliases(impl_->pageNavigationToggle,
        {"general.navigation", "general.pageNavigation.enabled"});
    registerAliases(impl_->previousPageHotkey.FocusTarget(),
        {"general.pageNavigation.previous"});
    registerAliases(impl_->nextPageHotkey.FocusTarget(),
        {"general.pageNavigation.next"});
    registerAliases(impl_->desktopPassthroughToggle,
        {"general.desktopPassthrough.enabled"});
    registerAliases(impl_->desktopPassthroughHotkey.FocusTarget(),
        {"general.desktopPassthrough.hotkey"});
    registerAliases(impl_->floatingDockToggle,
        {"general.floatingDock.enabled"});
    registerAliases(impl_->floatingDockHotkey.FocusTarget(),
        {"general.floatingDock.hotkey"});
    registerAliases(impl_->doubleClickHideToggle,
        {"general.doubleClickHide"});
}

void GeneralPagePresenter::Activate() noexcept
{
    if (impl_ && !impl_->closed) impl_->active = true;
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
