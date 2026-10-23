#include "pch.h"

#include "home_about_page_presenter.h"
#include "settings_presenter_controls.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace controls = presenter_controls;

namespace
{

struct Section
{
    muxc::Border root{nullptr};
    muxc::Border surface{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
};

struct LinkEntry
{
    HomeAboutLink target = HomeAboutLink::SourceRepository;
    std::string labelKey;
    std::wstring fallback;
    muxc::HyperlinkButton button{nullptr};
    winrt::event_token clickToken{};
};

void InitializeSection(
    Section& section,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    section.root = muxc::Border{};
    section.root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    section.content = muxc::StackPanel{};
    section.content.Spacing(9.0);
    section.content.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    section.title = muxc::TextBlock{};
    section.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    section.title.TextWrapping(mux::TextWrapping::Wrap);
    section.surface = presenter_controls::InitializeSettingsGroup(
        section.root, section.title, section.content, style, true);
    page.Children().Append(section.root);
}

muxc::TextBlock MakeBodyText(double opacity = 1.0)
{
    muxc::TextBlock text;
    text.TextWrapping(mux::TextWrapping::Wrap);
    text.Opacity(opacity);
    return text;
}

void SetAutomation(
    const mux::DependencyObject& object,
    std::wstring_view name,
    std::wstring_view help = {})
{
    muxa::AutomationProperties::SetName(object, std::wstring(name));
    muxa::AutomationProperties::SetHelpText(object, std::wstring(help));
}

} // namespace

struct HomeAboutPagePresenter::Impl
{
    explicit Impl(
        LocalizeCallback callback,
        const mux::Style& style,
        const mux::Style& navigationStyle)
        : localize(std::move(callback)), cardStyle(style),
          navigationCardStyle(navigationStyle)
    {
        BuildControls();
        HookEvents();
        RefreshLocalizedText();
        RenderStatus();
    }

    LocalizeCallback localize;
    HomeAboutPageActions actions;
    mux::Style cardStyle{nullptr};
    mux::Style navigationCardStyle{nullptr};
    muxc::StackPanel aboutRoot{nullptr};
    muxc::StackPanel debugRoot{nullptr};
    muxc::Expander credits{nullptr};

    Section advancedSection;
    controls::SettingRow advancedRow;
    muxc::ToggleSwitch advancedToggle{nullptr};
    winrt::event_token advancedToken{};
    Section introductionSection;
    Section authorSection;
    Section copyrightSection;
    Section profileSection;
    Section projectSection;
    Section communitySection;
    Section versionSection;
    Section thirdPartySection;
    Section dataSection;
    Section fontSection;
    Section referenceSection;
    muxc::TextBlock introductionText{nullptr};
    muxc::TextBlock authorText{nullptr};
    muxc::TextBlock copyrightText{nullptr};
    muxc::TextBlock licenseText{nullptr};
    std::vector<LinkEntry> links;

    controls::SettingRow versionRow;
    muxc::StackPanel versionControls{nullptr};
    muxc::Button versionButton{nullptr};
    muxc::Button checkUpdateButton{nullptr};

    struct LogEntry
    {
        snowdesktop::diagnostics::ModuleDescriptor descriptor;
        controls::SettingRow row;
        muxc::ToggleSwitch toggle{nullptr};
        winrt::event_token token{};
    };
    Section logSection;
    muxc::TextBlock logHelp{nullptr};
    controls::SettingRow allLogsRow;
    muxc::ToggleSwitch allLogsToggle{nullptr};
    winrt::event_token allLogsToken{};
    std::vector<LogEntry> logEntries;
    std::uint32_t logModules = 0;
    Section debugProfileSection;
    controls::SettingRow profileRow;
    controls::SettingRow desktopDirectoryRow;
    controls::SettingRow clearProfileRow;
    muxc::ToggleSwitch profileToggle{nullptr};
    muxc::TextBlock profileDataPath{nullptr};
    muxc::Button chooseDesktopButton{nullptr};
    muxc::Button clearProfileButton{nullptr};
    bool profileEnabled = false;
    std::wstring profileDirectory;
    std::wstring simulatedDesktop;
    winrt::event_token profileToken{}, chooseDesktopToken{}, clearProfileToken{};
    Section forceRestartSection;
    controls::SettingRow forceRestartToggleRow;
    controls::SettingRow forceRestartChordRow;
    muxc::ToggleSwitch forceRestartToggle{nullptr};
    HotkeyRecorder forceRestartRecorder;
    bool forceRestartEnabled = true;
    HotkeyChord forceRestartChord{MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, 'S'};
    winrt::event_token forceRestartToken{};
    Section demoModeSection;
    Section initializationSection;
    Section animationSection;
    Section resetUnlockSection;
    Section crashSection;
    controls::SettingRow demoModeRow;
    muxc::ToggleSwitch demoModeToggle{nullptr};
    controls::SettingRow initializationRow;
    muxc::ToggleSwitch initializationToggle{nullptr};
    controls::SettingRow animationRow;
    muxc::ToggleSwitch animationToggle{nullptr};
    muxc::TextBlock animationStatus{nullptr};
    controls::SettingRow resetUnlockRow;
    muxc::Button resetUnlockButton{nullptr};
    muxc::Expander crashExpander{nullptr};
    muxc::StackPanel crashHeader{nullptr};
    muxc::TextBlock crashTitle{nullptr};
    muxc::TextBlock crashDescription{nullptr};
    muxc::Grid crashActionHost{nullptr};
    muxc::Button crashButton{nullptr};

    std::uint64_t generation = 0;
    std::uint64_t generalRevision = 0;
    std::uint64_t statusRevision = 0;
    bool hasSnapshot = false;
    bool hasStatusRevision = false;
    bool active = false;
    SettingsPage activePage = SettingsPage::Home;
    bool closed = false;
    bool updatingControls = false;
    bool demoModeEnabled = false;
    bool animationDiagnosticsEnabled = false;
    bool temporaryInitializationEnabled = false;
    bool debugUnlocked = false;
    unsigned versionClickCount = 0;

    std::wstring applicationVersion;
    bool packaged = false;
    std::wstring animationDiagnosticsStatus;

    winrt::event_token checkUpdateToken{};
    winrt::event_token versionClickToken{};
    winrt::event_token demoModeToken{};
    winrt::event_token initializationToken{};
    winrt::event_token animationToken{};
    winrt::event_token resetUnlockToken{};
    winrt::event_token crashToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
    {
        if (localize && !key.empty())
        {
            std::wstring value = localize(key);
            if (!value.empty())
                return value;
        }
        return std::wstring(fallback);
    }

    muxc::HyperlinkButton AddLink(
        const muxc::StackPanel& parent,
        HomeAboutLink target,
        std::string labelKey,
        std::wstring fallback)
    {
        LinkEntry entry;
        entry.target = target;
        entry.labelKey = std::move(labelKey);
        entry.fallback = std::move(fallback);
        entry.button = muxc::HyperlinkButton{};
        entry.button.HorizontalAlignment(mux::HorizontalAlignment::Left);
        entry.button.UseSystemFocusVisuals(true);
        parent.Children().Append(entry.button);
        links.push_back(std::move(entry));
        return links.back().button;
    }

    void AddAttribution(
        const Section& section,
        HomeAboutLink target,
        std::wstring label,
        std::wstring license,
        std::wstring copyright,
        std::wstring modification = {})
    {
        muxc::StackPanel group;
        group.Spacing(2.0);
        muxc::StackPanel heading;
        heading.Orientation(muxc::Orientation::Horizontal);
        heading.Spacing(8.0);
        (void)AddLink(heading, target, {}, std::move(label));
        muxc::TextBlock licenseTextBlock = MakeBodyText(0.68);
        licenseTextBlock.Text(std::move(license));
        licenseTextBlock.VerticalAlignment(mux::VerticalAlignment::Center);
        heading.Children().Append(licenseTextBlock);
        group.Children().Append(heading);
        muxc::TextBlock copyrightBlock = MakeBodyText(0.68);
        copyrightBlock.Text(std::move(copyright));
        copyrightBlock.Margin(mux::Thickness{12.0, 0.0, 0.0, 0.0});
        group.Children().Append(copyrightBlock);
        if (!modification.empty())
        {
            muxc::TextBlock modificationBlock = MakeBodyText(0.68);
            modificationBlock.Text(std::move(modification));
            modificationBlock.Margin(
                mux::Thickness{12.0, 0.0, 0.0, 0.0});
            group.Children().Append(modificationBlock);
        }
        section.content.Children().Append(group);
    }

    void BuildControls()
    {

        aboutRoot = muxc::StackPanel{};
        aboutRoot.Spacing(24.0);
        InitializeSection(introductionSection, cardStyle, aboutRoot);
        introductionText = MakeBodyText(0.78);
        introductionSection.content.Children().Append(introductionText);

        InitializeSection(authorSection, cardStyle, aboutRoot);
        authorText = MakeBodyText();
        authorText.Text(L"逍遥飘雪（郭云哲）"); // l10n-allow: fixed author name
        authorSection.content.Children().Append(authorText);

        InitializeSection(copyrightSection, cardStyle, aboutRoot);
        copyrightText = MakeBodyText(0.78);
        licenseText = MakeBodyText(0.78);
        copyrightSection.content.Children().Append(copyrightText);
        copyrightSection.content.Children().Append(licenseText);

        InitializeSection(profileSection, cardStyle, aboutRoot);
        (void)AddLink(profileSection.content,
            HomeAboutLink::Bilibili, {}, L"Bilibili");
        (void)AddLink(profileSection.content,
            HomeAboutLink::AuthorGitHub, {}, L"GitHub");
        (void)AddLink(profileSection.content,
            HomeAboutLink::Douyin, "app.settings.douyin", L"Douyin");
        (void)AddLink(profileSection.content,
            HomeAboutLink::Xiaohongshu,
            "app.settings.xiaohongshu", L"Xiaohongshu");

        InitializeSection(projectSection, cardStyle, aboutRoot);
        (void)AddLink(projectSection.content,
            HomeAboutLink::SourceRepository, {}, L"GitHub");
        (void)AddLink(projectSection.content,
            HomeAboutLink::OfficialWebsite,
            "settings.about.officialWebsite", L"Official website");

        InitializeSection(communitySection, cardStyle, aboutRoot);
        (void)AddLink(communitySection.content,
            HomeAboutLink::QqGroup,
            "app.settings.join_qq", L"Join QQ Group: 976422547");

        InitializeSection(versionSection, cardStyle, aboutRoot);
        versionSection.title.Visibility(mux::Visibility::Collapsed);
        versionControls = muxc::StackPanel{};
        versionControls.Spacing(7.0);
        versionControls.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        muxc::StackPanel versionActions;
        versionActions.Orientation(muxc::Orientation::Horizontal);
        versionActions.Spacing(8.0);
        versionActions.HorizontalAlignment(mux::HorizontalAlignment::Right);
        versionButton = muxc::Button{};
        versionButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        versionButton.VerticalAlignment(mux::VerticalAlignment::Center);
        versionButton.UseSystemFocusVisuals(true);
        checkUpdateButton = muxc::Button{};
        checkUpdateButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        checkUpdateButton.VerticalAlignment(mux::VerticalAlignment::Center);
        checkUpdateButton.UseSystemFocusVisuals(true);
        versionActions.Children().Append(versionButton);
        versionActions.Children().Append(checkUpdateButton);
        versionControls.Children().Append(versionActions);
        versionRow.Initialize(versionControls);
        versionSection.content.Children().Append(versionRow.root);

        InitializeSection(thirdPartySection, cardStyle, aboutRoot);
        InitializeSection(dataSection, cardStyle, aboutRoot);
        InitializeSection(fontSection, cardStyle, aboutRoot);
        InitializeSection(referenceSection, cardStyle, aboutRoot);
        AddAttribution(thirdPartySection, HomeAboutLink::EverythingSdk, L"Everything SDK",
            L"(MIT)", L"Copyright (C) 2016 David Carpenter");
        AddAttribution(thirdPartySection, HomeAboutLink::DearImGui, L"Dear ImGui", L"(MIT)",
            L"Copyright (c) 2014-2025 Omar Cornut");
        AddAttribution(thirdPartySection, HomeAboutLink::Lua, L"Lua", L"(MIT)",
            L"Copyright (C) 1994-2024 Lua.org, PUC-Rio");
        AddAttribution(dataSection, HomeAboutLink::PinyinData, L"pinyin-data", L"(MIT)",
            L"Copyright (c) 2016 mozillazg");
        AddAttribution(fontSection, HomeAboutLink::MiSans, L"MiSans Regular · 4.009", L"(MiSans font license)",
            L"Copyright © 2020-2025 Beijing Xiaomi Mobile Software Co., Ltd.", L"SnowDesktop uses MiSans as an optional interface font; original font file unchanged.");
        AddAttribution(fontSection, HomeAboutLink::HarmonyOsSans, L"HarmonyOS Sans SC · 1.0", L"(HarmonyOS Sans Fonts License Agreement)",
            L"Copyright 2021 Huawei Device Co., Ltd.", L"SnowDesktop uses HarmonyOS Sans as an optional interface font; original font files unchanged.");
        AddAttribution(referenceSection, HomeAboutLink::Yasb, L"YASB · d6d1e6d553b0", L"(MIT)",
            L"Copyright (c) 2024 amnweb · Copyright (c) 2021 denBot");
        AddAttribution(referenceSection, HomeAboutLink::TranslucentTb,
            L"TranslucentTB (modified portions)", L"(GPL-3.0-only)",
            L"Copyright (c) TranslucentTB contributors",
            L"Modified for SnowDesktop from upstream commit 322e2b7");

        // Version/update and support are the frequent About-page tasks.
        // Keep legal notices available in one explicitly named disclosure.
        aboutRoot.Children().Clear();
        for (const auto& section : {versionSection.root, introductionSection.root,
                 projectSection.root, communitySection.root, authorSection.root,
                 profileSection.root, copyrightSection.root})
            aboutRoot.Children().Append(section);
        muxc::StackPanel creditGroups;
        creditGroups.Spacing(24);
        for (const auto* section : {&thirdPartySection, &dataSection,
                 &fontSection, &referenceSection})
        {
            section->surface.Padding({0, 0, 0, 0});
            section->surface.BorderThickness({0, 0, 0, 0});
            section->surface.Background(nullptr);
            creditGroups.Children().Append(section->root);
        }
        credits = muxc::Expander{};
        credits.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        credits.HorizontalContentAlignment(mux::HorizontalAlignment::Stretch);
        credits.IsExpanded(false);
        credits.Content(creditGroups);
        aboutRoot.Children().Append(credits);
        InitializeSection(advancedSection, cardStyle, aboutRoot);
        advancedSection.title.Visibility(mux::Visibility::Collapsed);
        advancedToggle = muxc::ToggleSwitch{};
        advancedToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        advancedRow.Initialize(advancedToggle);
        advancedSection.content.Children().Append(advancedRow.root);

        debugRoot = muxc::StackPanel{};
        debugRoot.Spacing(8.0);
        InitializeSection(forceRestartSection, cardStyle, debugRoot);
        forceRestartSection.title.Visibility(mux::Visibility::Collapsed);
        forceRestartToggle = muxc::ToggleSwitch{};
        forceRestartToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        forceRestartToggleRow.Initialize(forceRestartToggle);
        forceRestartSection.content.Children().Append(forceRestartToggleRow.root);
        forceRestartRecorder.Root().HorizontalAlignment(mux::HorizontalAlignment::Right);
        forceRestartChordRow.Initialize(forceRestartRecorder.Root());
        forceRestartChordRow.SetCompactEditor();
        forceRestartChordRow.SetControlAlignment(mux::HorizontalAlignment::Right);
        forceRestartSection.content.Children().Append(forceRestartChordRow.root);
        forceRestartRecorder.SetDefaultValue(
            {MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, 'S'});

        InitializeSection(debugProfileSection, cardStyle, debugRoot);
        profileToggle = muxc::ToggleSwitch{};
        profileToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        profileToggle.UseSystemFocusVisuals(true);
        profileRow.Initialize(profileToggle, 180.0);
        profileDataPath = MakeBodyText(0.72);
        profileDataPath.IsTextSelectionEnabled(true);
        chooseDesktopButton = muxc::Button{};
        chooseDesktopButton.UseSystemFocusVisuals(true);
        desktopDirectoryRow.Initialize(chooseDesktopButton);
        clearProfileButton = muxc::Button{};
        clearProfileButton.UseSystemFocusVisuals(true);
        clearProfileRow.Initialize(clearProfileButton);
        debugProfileSection.content.Children().Append(profileRow.root);
        debugProfileSection.content.Children().Append(profileDataPath);
        debugProfileSection.content.Children().Append(desktopDirectoryRow.root);
        debugProfileSection.content.Children().Append(clearProfileRow.root);

        InitializeSection(logSection, cardStyle, debugRoot);
        logHelp = MakeBodyText(0.72);
        logSection.content.Children().Append(logHelp);
        allLogsToggle = muxc::ToggleSwitch{};
        allLogsToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        allLogsToggle.UseSystemFocusVisuals(true);
        allLogsRow.Initialize(allLogsToggle, 180.0);
        logSection.content.Children().Append(allLogsRow.root);
        logEntries.reserve(snowdesktop::diagnostics::LogModules.size());
        for (const auto& descriptor : snowdesktop::diagnostics::LogModules)
        {
            LogEntry entry;
            entry.descriptor = descriptor;
            entry.toggle = muxc::ToggleSwitch{};
            entry.toggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
            entry.toggle.UseSystemFocusVisuals(true);
            entry.row.Initialize(entry.toggle, 180.0);
            logSection.content.Children().Append(entry.row.root);
            logEntries.push_back(std::move(entry));
        }

        InitializeSection(demoModeSection, cardStyle, debugRoot);
        demoModeSection.title.Visibility(mux::Visibility::Collapsed);
        demoModeToggle = muxc::ToggleSwitch{};
        demoModeToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        demoModeToggle.UseSystemFocusVisuals(true);
        demoModeRow.Initialize(demoModeToggle, 180.0);
        demoModeSection.content.Children().Append(demoModeRow.root);

        InitializeSection(initializationSection, cardStyle, debugRoot);
        initializationSection.title.Visibility(mux::Visibility::Collapsed);
        initializationToggle = muxc::ToggleSwitch{};
        initializationToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        initializationToggle.UseSystemFocusVisuals(true);
        initializationRow.Initialize(initializationToggle, 180.0);
        initializationSection.content.Children().Append(initializationRow.root);

        InitializeSection(animationSection, cardStyle, debugRoot);
        animationSection.title.Visibility(mux::Visibility::Collapsed);
        animationToggle = muxc::ToggleSwitch{};
        animationToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        animationToggle.UseSystemFocusVisuals(true);
        animationStatus = MakeBodyText(0.72);
        animationStatus.Visibility(mux::Visibility::Collapsed);
        animationRow.Initialize(animationToggle);
        animationRow.text.Children().Append(animationStatus);
        animationSection.content.Children().Append(animationRow.root);

        InitializeSection(resetUnlockSection, cardStyle, debugRoot);
        resetUnlockSection.title.Visibility(mux::Visibility::Collapsed);
        resetUnlockButton = muxc::Button{};
        resetUnlockButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        resetUnlockButton.UseSystemFocusVisuals(true);
        resetUnlockRow.Initialize(resetUnlockButton);
        resetUnlockSection.content.Children().Append(resetUnlockRow.root);

        InitializeSection(crashSection, cardStyle, debugRoot);
        crashSection.title.Visibility(mux::Visibility::Collapsed);
        crashExpander = muxc::Expander{};
        crashExpander.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        crashExpander.HorizontalContentAlignment(
            mux::HorizontalAlignment::Stretch);
        crashExpander.IsExpanded(false);
        crashHeader = muxc::StackPanel{};
        crashHeader.Spacing(3.0);
        crashTitle = MakeBodyText();
        crashTitle.FontWeight(
            winrt::Windows::UI::Text::FontWeights::SemiBold());
        crashDescription = MakeBodyText(0.68);
        crashHeader.Children().Append(crashTitle);
        crashHeader.Children().Append(crashDescription);
        crashExpander.Header(crashHeader);
        crashActionHost = muxc::Grid{};
        crashButton = muxc::Button{};
        crashButton.HorizontalAlignment(mux::HorizontalAlignment::Right);
        crashButton.UseSystemFocusVisuals(true);
        crashActionHost.Children().Append(crashButton);
        crashExpander.Content(crashActionHost);
        crashSection.root.Child(crashExpander);
    }

    void HookEvents()
    {
        for (LinkEntry& link : links)
        {
            const HomeAboutLink target = link.target;
            link.clickToken = link.button.Click(
                [this, target](const auto&, const auto&) {
                    if (CanInvokeAbout() && actions.openLink)
                        actions.openLink(generation, target);
                });
        }

        advancedToken = advancedToggle.Toggled([this](const auto&, const auto&) {
            if (updatingControls || !CanInvokeAbout() || !actions.updateGeneral) return;
            const bool value = advancedToggle.IsOn();
            actions.updateGeneral(generation, SettingsUpdateMode::PreviewAndCommit, [value](GeneralSettings& settings) { settings.showAdvancedSettings = value; });
        });
        checkUpdateToken = checkUpdateButton.Click(
            [this](const auto&, const auto&) {
                if (packaged)
                    Invoke(HomeAboutCommand::CheckForUpdates);
            });
        versionClickToken = versionButton.Click(
            [this](const auto&, const auto&) {
                if (!CanInvokeAbout() || debugUnlocked)
                    return;
                ++versionClickCount;
                if (versionClickCount < 5 || !actions.unlockDebug)
                    return;
                if (!actions.unlockDebug(generation))
                    return;
                debugUnlocked = true;
                if (actions.navigate)
                {
                    actions.navigate(
                        SettingsRoute::ForPage(SettingsPage::Debug));
                }
            });
        allLogsToken = allLogsToggle.Toggled([this](const auto&, const auto&) {
            if (updatingControls || !CanInvokeDebug() || !actions.updateGeneral) return;
            const bool enabled = allLogsToggle.IsOn();
            actions.updateGeneral(generation, SettingsUpdateMode::Commit,
                [enabled](GeneralSettings& settings) {
                    settings.diagnosticLogModules = enabled
                        ? snowdesktop::diagnostics::AllLogModules : 0u;
                });
        });
        for (auto& entry : logEntries)
        {
            const auto module = entry.descriptor.module;
            const auto toggle = entry.toggle;
            entry.token = toggle.Toggled([this, module, toggle](const auto&, const auto&) {
                if (updatingControls || !CanInvokeDebug() || !actions.updateGeneral) return;
                const bool enabled = toggle.IsOn();
                actions.updateGeneral(generation, SettingsUpdateMode::Commit,
                    [module, enabled](GeneralSettings& settings) {
                        const auto bit = snowdesktop::diagnostics::ModuleBit(module);
                        if (enabled) settings.diagnosticLogModules |= bit;
                        else settings.diagnosticLogModules &= ~bit;
                    });
            });
        }
        profileToken = profileToggle.Toggled([this](const auto&, const auto&) {
            if (updatingControls || !CanInvokeDebug() || !actions.setDebugProfileEnabled) return;
            actions.setDebugProfileEnabled(generation, profileToggle.IsOn());
            RenderStatus();
        });
        chooseDesktopToken = chooseDesktopButton.Click([this](const auto&, const auto&) {
            if (CanInvokeDebug() && actions.chooseDebugDesktop) actions.chooseDebugDesktop(generation);
        });
        clearProfileToken = clearProfileButton.Click([this](const auto&, const auto&) {
            if (CanInvokeDebug() && actions.clearDebugProfile) actions.clearDebugProfile(generation);
        });
        forceRestartToken = forceRestartToggle.Toggled([this](const auto&, const auto&) {
            if (updatingControls || !CanInvokeDebug() || !actions.updateGeneral) return;
            const bool enabled = forceRestartToggle.IsOn();
            actions.updateGeneral(generation, SettingsUpdateMode::Commit,
                [enabled](GeneralSettings& settings) { settings.forceRestartHotkeyEnabled = enabled; });
        });
        forceRestartRecorder.SetAvailabilityProbe([this](HotkeyChord chord,
            std::uint64_t expectedGeneration, std::uint64_t requestId,
            HotkeyRecorder::AvailabilityCompletion completion) {
            if (closed || !hasSnapshot || expectedGeneration != generation || !actions.probeHotkey)
            {
                completion(false, L("app.settings.hotkey_conflict_system"));
                return;
            }
            actions.probeHotkey(SettingsHostActions::HotkeyTarget::ForceRestart,
                chord, expectedGeneration, requestId, std::move(completion));
        });
        forceRestartRecorder.SetCommittedCallback([this](HotkeyChord chord) {
            if (!CanInvokeDebug() || !actions.updateGeneral) return;
            actions.updateGeneral(generation, SettingsUpdateMode::Commit,
                [chord](GeneralSettings& settings) {
                    settings.forceRestartHotkeyModifiers = chord.modifiers;
                    settings.forceRestartHotkeyVirtualKey = chord.virtualKey;
                });
        });
        demoModeToken = demoModeToggle.Toggled(
            [this](const auto&, const auto&) {
                if (updatingControls || !CanInvokeDebug() ||
                    !actions.updateGeneral)
                {
                    return;
                }
                const bool enabled = demoModeToggle.IsOn();
                actions.updateGeneral(generation,
                    SettingsUpdateMode::PreviewAndCommit,
                    [enabled](GeneralSettings& settings) {
                        settings.demoModeEnabled = enabled;
                    });
            });
        initializationToken = initializationToggle.Toggled(
            [this](const auto&, const auto&) {
                if (updatingControls || !CanInvokeDebug() || !actions.setTemporaryInitialization)
                    return;
                actions.setTemporaryInitialization(generation, initializationToggle.IsOn());
                RenderStatus();
            });
        animationToken = animationToggle.Toggled(
            [this](const auto&, const auto&) {
                if (updatingControls || !CanInvokeDebug() ||
                    !actions.setAnimationDiagnostics)
                {
                    return;
                }
                actions.setAnimationDiagnostics(
                    generation, animationToggle.IsOn());
            });
        resetUnlockToken = resetUnlockButton.Click(
            [this](const auto&, const auto&) {
                if (CanInvokeDebug() && actions.requestResetUnlockConfirmation)
                    actions.requestResetUnlockConfirmation(generation);
            });
        crashToken = crashButton.Click(
            [this](const auto&, const auto&) {
                if (CanInvokeDebug() &&
                    actions.requestCrashTestConfirmation)
                {
                    actions.requestCrashTestConfirmation(generation);
                }
            });
    }

    [[nodiscard]] bool CanInvoke() const noexcept
    {
        return !closed && active && hasSnapshot;
    }

    [[nodiscard]] bool CanInvokeAbout() const noexcept
    {
        return CanInvoke() && activePage == SettingsPage::About;
    }

    [[nodiscard]] bool CanInvokeDebug() const noexcept
    {
        return CanInvoke() && activePage == SettingsPage::Debug;
    }

    void Invoke(HomeAboutCommand command)
    {
        if (CanInvokeAbout() && actions.invoke)
            actions.invoke(generation, command);
    }

    void SetButtonText(
        const muxc::Button& button,
        std::string_view key,
        std::wstring_view fallback)
    {
        const std::wstring text = L(key, fallback);
        button.Content(winrt::box_value(text));
        SetAutomation(button, text);
    }

    void SetSectionTitle(
        const Section& section,
        std::string_view key,
        std::wstring_view fallback)
    {
        const std::wstring text = L(key, fallback);
        section.title.Text(text);
        SetAutomation(section.root, text);
    }

    void RenderStatus()
    {
        if (closed)
            return;
        const std::wstring version = applicationVersion.empty()
            ? L("settings.about.version.unknown", L"Version unavailable")
            : L"SnowDesktop v" + applicationVersion;
        versionButton.Content(winrt::box_value(version));
        SetAutomation(versionButton, version,
            L("settings.page.debug.description",
                L"Click five times to unlock Debug."));
        checkUpdateButton.Visibility(packaged
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        SetButtonText(checkUpdateButton,
            "app.settings.check_update", L"Check for Updates");
        updatingControls = true;
        profileToggle.IsOn(profileEnabled);
        profileToggle.IsEnabled(!temporaryInitializationEnabled);
        chooseDesktopButton.IsEnabled(!temporaryInitializationEnabled);
        clearProfileButton.IsEnabled(!temporaryInitializationEnabled);
        initializationToggle.IsEnabled(!profileEnabled);
        profileDataPath.Text(L("settings.debug.profile.data") + L"\n" + profileDirectory);
        desktopDirectoryRow.SetText(L("settings.debug.profile.desktop"), simulatedDesktop);
        allLogsToggle.IsOn(snowdesktop::diagnostics::NormalizeLogModules(logModules) ==
            snowdesktop::diagnostics::AllLogModules);
        for (const auto& entry : logEntries)
            entry.toggle.IsOn((logModules & snowdesktop::diagnostics::ModuleBit(entry.descriptor.module)) != 0);
        forceRestartToggle.IsOn(forceRestartEnabled);
        demoModeToggle.IsOn(demoModeEnabled);
        animationToggle.IsOn(animationDiagnosticsEnabled);
        initializationToggle.IsOn(temporaryInitializationEnabled);
        updatingControls = false;
        animationStatus.Text(animationDiagnosticsStatus);
        animationStatus.Visibility(
            animationDiagnosticsEnabled &&
                !animationDiagnosticsStatus.empty()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);

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
        text.apply = L("app.settings.apply");
        text.cancel = L("app.settings.cancel");
        text.edit = L("app.settings.hotkey_edit");
        text.clear = L("app.settings.widget.clear");
        text.restoreDefault = L("app.settings.restore_default");
        return text;
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        advancedRow.SetText(L("modules.showAdvanced"), L("modules.showAdvancedDescription"));
        credits.Header(winrt::box_value(L("settings.about.licensesAndCredits", L"Third-party notices and credits")));

        SetSectionTitle(introductionSection,
            "app.settings.about_snowdesktop", L"About SnowDesktop");
        introductionText.Text(L("app.settings.about_description"));
        SetSectionTitle(authorSection,
            "app.settings.author", L"Author");
        SetSectionTitle(copyrightSection,
            "app.settings.copyright", L"Copyright & License");
        copyrightText.Text(L("app.settings.copyright_notice"));
        licenseText.Text(L("app.settings.license_notice"));
        SetSectionTitle(profileSection,
            "app.settings.personal_homepages", L"Personal Profiles");
        SetSectionTitle(projectSection,
            "app.settings.project_url", L"Project URL");
        SetSectionTitle(communitySection,
            "app.settings.community", L"Community");
        SetSectionTitle(versionSection,
            "app.settings.version", L"Version");
        versionRow.SetText(L("app.settings.version", L"Version"));
        SetSectionTitle(thirdPartySection,
            "app.settings.third_party_libs", L"Libraries and SDKs");
        SetSectionTitle(dataSection,
            "app.settings.third_party_data", L"Data");
        SetSectionTitle(fontSection,
            "app.settings.third_party_fonts", L"Interface Fonts");
        SetSectionTitle(referenceSection,
            "app.settings.reference_programs",
            L"Referenced and Adapted Projects");
        for (LinkEntry& link : links)
        {
            const std::wstring text = L(link.labelKey, link.fallback);
            link.button.Content(winrt::box_value(text));
            SetAutomation(link.button, text, HomeAboutLinkUri(link.target));
        }

        SetSectionTitle(logSection, "settings.debug.logs.title", L"Detailed logs");
        logHelp.Text(L("settings.debug.logs.description"));
        allLogsRow.SetText(L("settings.debug.logs.all"));
        SetAutomation(allLogsToggle, allLogsRow.label.Text());
        muxa::AutomationProperties::SetAutomationId(allLogsToggle, L"debug.logs.all");
        for (auto& entry : logEntries)
        {
            entry.row.SetText(L(entry.descriptor.labelKey));
            SetAutomation(entry.toggle, entry.row.label.Text());
            muxa::AutomationProperties::SetAutomationId(entry.toggle,
                winrt::to_hstring("debug.logs." + std::string(entry.descriptor.id)));
        }
        forceRestartToggleRow.SetText(L("settings.debug.forceRestartHotkey"),
            L("settings.debug.forceRestartHotkey.description"));
        SetAutomation(forceRestartToggle, forceRestartToggleRow.label.Text(),
            forceRestartToggleRow.help.Text());
        forceRestartChordRow.SetText(L("app.settings.hotkey"));
        forceRestartRecorder.SetText(HotkeyText(L("settings.debug.forceRestartHotkey")));
        SetSectionTitle(debugProfileSection, "settings.debug.profile.title", L"Debug environment");
        profileRow.SetText(L("settings.debug.profile.enabled"), L("settings.debug.profile.description"));
        SetAutomation(profileToggle, profileRow.label.Text(), profileRow.help.Text());
        SetButtonText(chooseDesktopButton, "settings.debug.profile.choose", L"Choose folder");
        SetAutomation(chooseDesktopButton, L("settings.debug.profile.desktop"), L("settings.debug.profile.chooseHint"));
        clearProfileRow.SetText(L("settings.debug.profile.clear"), L("settings.debug.profile.clearHint"));
        SetButtonText(clearProfileButton, "settings.debug.profile.clear", L"Clear debug data");
        SetAutomation(clearProfileButton, clearProfileRow.label.Text(), clearProfileRow.help.Text());
        SetSectionTitle(demoModeSection,
            "app.settings.demo_mode", L"Demo mode");
        demoModeRow.SetText(L("app.settings.demo_mode", L"Demo mode"),
            L("app.settings.demo_mode_hint"));
        SetAutomation(demoModeToggle,
            demoModeRow.label.Text(), demoModeRow.help.Text());
        SetSectionTitle(initializationSection,
            "settings.debug.initialization", L"Temporary initialization");
        initializationRow.SetText(
            L("settings.debug.initialization", L"Temporary initialization"),
            L("settings.debug.initialization.description"));
        SetAutomation(initializationToggle,
            initializationRow.label.Text(), initializationRow.help.Text());
        SetSectionTitle(animationSection,
            "app.settings.animation_diagnostics",
            L"Animation diagnostics (this session)");
        animationRow.SetText(
            L("app.settings.animation_diagnostics",
                L"Animation diagnostics (this session)"),
            L("app.settings.animation_diagnostics_desc"));
        SetAutomation(animationToggle,
            animationRow.label.Text(), animationRow.help.Text());
        resetUnlockRow.SetText(
            L("settings.debug.resetUnlock", L"Clear unlock state"),
            L("settings.debug.resetUnlock.description"));
        SetButtonText(resetUnlockButton,
            "settings.debug.resetUnlock", L"Clear unlock state");
        muxa::AutomationProperties::SetHelpText(
            resetUnlockButton, resetUnlockRow.help.Text());
        SetSectionTitle(crashSection,
            "app.settings.crash_test", L"Crash Test");
        crashTitle.Text(L("app.settings.crash_test", L"Crash Test"));
        crashDescription.Text(L("app.settings.crash_test_desc"));
        SetButtonText(crashButton,
            "app.settings.trigger_crash",
            L"Trigger Crash (Access Violation)");
        muxa::AutomationProperties::SetHelpText(
            crashButton, crashDescription.Text());
        SetAutomation(crashExpander,
            crashTitle.Text(), crashDescription.Text());
        RenderStatus();
    }

    void ResetStatusForGeneration()
    {
        statusRevision = 0;
        hasStatusRevision = false;
        applicationVersion.clear();
        packaged = false;
        animationDiagnosticsEnabled = false;
        temporaryInitializationEnabled = false;
        profileEnabled = false;
        profileDirectory.clear();
        simulatedDesktop.clear();
        animationDiagnosticsStatus.clear();
    }

    void ApplySnapshot(const SettingsSnapshot& snapshot)
    {
        if (closed)
            return;
        const bool newGeneration =
            !hasSnapshot || generation != snapshot.generation;
        if (newGeneration)
        {
            generation = snapshot.generation;
            ResetStatusForGeneration();
        }
        hasSnapshot = true;
        if (newGeneration || generalRevision !=
                snapshot.domainRevisions.general)
        {
            generalRevision = snapshot.domainRevisions.general;
            updatingControls = true;
            advancedToggle.IsOn(snapshot.values.general.showAdvancedSettings);
            updatingControls = false;
            logModules = snapshot.values.general.diagnosticLogModules;
            snowdesktop::diagnostics::SetLogModules(logModules);
            demoModeEnabled = snapshot.values.general.demoModeEnabled;
            forceRestartEnabled = snapshot.values.general.forceRestartHotkeyEnabled;
            forceRestartChord = {snapshot.values.general.forceRestartHotkeyModifiers,
                snapshot.values.general.forceRestartHotkeyVirtualKey};
            forceRestartRecorder.SetValidationContext(forceRestartEnabled, false);
            forceRestartRecorder.Root().IsEnabled(forceRestartEnabled);
            forceRestartChordRow.SetEnabled(forceRestartEnabled);
            forceRestartRecorder.SetValue(forceRestartChord, generation);
        }
        RenderStatus();
    }

    [[nodiscard]] bool ApplyStatusPatch(
        const HomeAboutStatusPatch& patch)
    {
        if (closed || !hasSnapshot || patch.generation != generation ||
            (hasStatusRevision && patch.revision <= statusRevision))
        {
            return false;
        }
        statusRevision = patch.revision;
        hasStatusRevision = true;
        if (patch.applicationVersion)
            applicationVersion = *patch.applicationVersion;
        if (patch.packaged)
            packaged = *patch.packaged;
        if (patch.animationDiagnosticsEnabled)
            animationDiagnosticsEnabled =
                *patch.animationDiagnosticsEnabled;
        if (patch.temporaryInitializationEnabled)
            temporaryInitializationEnabled = *patch.temporaryInitializationEnabled;
        if (patch.debugProfileEnabled) profileEnabled = *patch.debugProfileEnabled;
        if (patch.debugDataDirectory) profileDirectory = *patch.debugDataDirectory;
        if (patch.debugDesktopDirectory) simulatedDesktop = *patch.debugDesktopDirectory;
        if (patch.animationDiagnosticsStatus)
            animationDiagnosticsStatus =
                *patch.animationDiagnosticsStatus;
        RenderStatus();
        return true;
    }

    [[nodiscard]] mux::FrameworkElement FocusTarget(
        SettingsPage page,
        std::string_view focusId) const noexcept
    {
        if (page == SettingsPage::About)
        {
            const auto link = [&](HomeAboutLink target) -> mux::FrameworkElement {
                for (const auto& entry : links)
                    if (entry.target == target) return entry.button;
                return versionButton;
            };
            if (focusId == "about.showAdvancedSettings") return advancedToggle;
            if (focusId == "about.profile") return link(HomeAboutLink::Bilibili);
            if (focusId == "about.project") return link(HomeAboutLink::SourceRepository);
            if (focusId == "about.website") return link(HomeAboutLink::OfficialWebsite);
            if (focusId == "about.community") return link(HomeAboutLink::QqGroup);
            if (focusId == "about.thirdparty") return link(HomeAboutLink::EverythingSdk);
            return versionButton;
        }
        if (page == SettingsPage::Debug)
        {
            if (focusId == "debug.forceRestartHotkey") return forceRestartToggle;
            if (focusId == "debug.forceRestartHotkey.hotkey")
                return forceRestartEnabled ? forceRestartRecorder.FocusTarget() : forceRestartToggle;
            if (focusId == "debug.profile") return profileToggle;
            if (focusId == "debug.desktop") return chooseDesktopButton;
            if (focusId == "debug.clearProfile") return clearProfileButton;
            for (const auto& entry : logEntries)
                if (focusId == "debug.logs." + std::string(entry.descriptor.id)) return entry.toggle;
            if (focusId == "debug.logs" || focusId == "debug.logs.all") return allLogsToggle;
            if (focusId == "debug.demo_mode") return demoModeToggle;
            if (focusId == "debug.initialization") return initializationToggle;
            if (focusId == "debug.animation") return animationToggle;
            if (focusId == "debug.resetUnlock") return resetUnlockButton;
            if (focusId == "debug.crash") return crashExpander;
            return animationToggle;
        }
        return nullptr;
    }

    void Close() noexcept
    {
        if (closed)
            return;
        closed = true;
        active = false;
        try
        {
            for (LinkEntry& link : links)
                link.button.Click(link.clickToken);
            advancedToggle.Toggled(advancedToken);
            checkUpdateButton.Click(checkUpdateToken);
            versionButton.Click(versionClickToken);
            allLogsToggle.Toggled(allLogsToken);
            for (const auto& entry : logEntries) entry.toggle.Toggled(entry.token);
            profileToggle.Toggled(profileToken);
            chooseDesktopButton.Click(chooseDesktopToken);
            clearProfileButton.Click(clearProfileToken);
            forceRestartToggle.Toggled(forceRestartToken);
            demoModeToggle.Toggled(demoModeToken);
            initializationToggle.Toggled(initializationToken);
            animationToggle.Toggled(animationToken);
            resetUnlockButton.Click(resetUnlockToken);
            crashButton.Click(crashToken);
        }
        catch (...)
        {
        }
        forceRestartRecorder.Close();
        actions = {};
        localize = {};
    }
};

HomeAboutPagePresenter::HomeAboutPagePresenter(
    LocalizeCallback localize,
    const mux::Style& cardStyle,
    const mux::Style& navigationCardStyle)
    : impl_(std::make_unique<Impl>(
          std::move(localize), cardStyle, navigationCardStyle))
{
}

HomeAboutPagePresenter::~HomeAboutPagePresenter()
{
    Close();
}

void HomeAboutPagePresenter::SetActions(HomeAboutPageActions actions)
{
    if (impl_ && !impl_->closed)
    {
        impl_->actions = std::move(actions);
    }
}

mux::UIElement HomeAboutPagePresenter::AboutContent() const noexcept
{
    return impl_ ? impl_->aboutRoot : nullptr;
}

mux::UIElement HomeAboutPagePresenter::DebugContent() const noexcept
{
    return impl_ ? impl_->debugRoot : nullptr;
}

void HomeAboutPagePresenter::ApplySnapshot(
    const SettingsSnapshot& snapshot)
{
    if (impl_)
        impl_->ApplySnapshot(snapshot);
}

bool HomeAboutPagePresenter::ApplyStatusPatch(
    const HomeAboutStatusPatch& patch)
{
    return impl_ && impl_->ApplyStatusPatch(patch);
}

void HomeAboutPagePresenter::RefreshLocalizedText()
{
    if (impl_)
        impl_->RefreshLocalizedText();
}

void HomeAboutPagePresenter::Activate(SettingsPage page) noexcept
{
    if (!impl_ || impl_->closed)
        return;
    impl_->active = page == SettingsPage::About || page == SettingsPage::Debug;
    impl_->activePage = page;
}

void HomeAboutPagePresenter::Deactivate() noexcept
{
    if (impl_ && !impl_->closed)
    {
        impl_->forceRestartRecorder.CancelCapture();
        impl_->active = false;
    }
}

mux::FrameworkElement HomeAboutPagePresenter::FocusTarget(
    SettingsPage page,
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(page, focusId) : nullptr;
}

bool HomeAboutPagePresenter::IsHotkeyCaptureActive() const noexcept
{
    return impl_ && !impl_->closed && impl_->active &&
        impl_->forceRestartRecorder.IsCapturing();
}

void HomeAboutPagePresenter::CaptureRegisteredHotkey(UINT modifiers, UINT virtualKey)
{
    if (IsHotkeyCaptureActive())
        impl_->forceRestartRecorder.CaptureRegisteredHotkey(modifiers, virtualKey);
}

void HomeAboutPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui
