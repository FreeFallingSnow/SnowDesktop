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
    section.root.Style(style);
    section.root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    section.content = muxc::StackPanel{};
    section.content.Spacing(9.0);
    section.content.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    section.title = muxc::TextBlock{};
    section.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    section.title.TextWrapping(mux::TextWrapping::Wrap);
    section.content.Children().Append(section.title);
    section.root.Child(section.content);
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

    Section introductionSection;
    Section authorSection;
    Section copyrightSection;
    Section profileSection;
    Section projectSection;
    Section communitySection;
    Section versionSection;
    Section thirdPartySection;
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

    Section debugTitleSection;
    Section demoModeSection;
    Section initializationSection;
    Section animationSection;
    Section resetUnlockSection;
    Section crashSection;
    muxc::TextBlock debugPageDescription{nullptr};
    controls::SettingRow demoModeRow;
    muxc::ToggleSwitch demoModeToggle{nullptr};
    controls::SettingRow initializationRow;
    muxc::ToggleSwitch initializationToggle{nullptr};
    controls::SettingRow animationRow;
    muxc::StackPanel animationControls{nullptr};
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
        const muxc::StackPanel& parent =
            target == HomeAboutLink::TranslucentTb
                ? referenceSection.content : thirdPartySection.content;
        parent.Children().Append(group);
    }

    void BuildControls()
    {

        aboutRoot = muxc::StackPanel{};
        aboutRoot.Spacing(8.0);
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
        InitializeSection(referenceSection, cardStyle, aboutRoot);
        AddAttribution(HomeAboutLink::EverythingSdk, L"Everything SDK",
            L"(MIT)", L"Copyright (C) 2016 David Carpenter");
        AddAttribution(HomeAboutLink::DearImGui, L"Dear ImGui", L"(MIT)",
            L"Copyright (c) 2014-2025 Omar Cornut");
        AddAttribution(HomeAboutLink::Lua, L"Lua", L"(MIT)",
            L"Copyright (C) 1994-2024 Lua.org, PUC-Rio");
        AddAttribution(HomeAboutLink::PinyinData, L"pinyin-data", L"(MIT)",
            L"Copyright (c) 2016 mozillazg");
        AddAttribution(HomeAboutLink::TranslucentTb,
            L"TranslucentTB (modified portions)", L"(GPL-3.0-only)",
            L"Copyright (c) TranslucentTB contributors",
            L"Modified for SnowDesktop from upstream commit 322e2b7");

        debugRoot = muxc::StackPanel{};
        debugRoot.Spacing(8.0);
        InitializeSection(debugTitleSection, cardStyle, debugRoot);
        debugPageDescription = MakeBodyText(0.72);
        debugTitleSection.content.Children().Append(debugPageDescription);

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
        animationControls = muxc::StackPanel{};
        animationControls.Spacing(6.0);
        animationToggle = muxc::ToggleSwitch{};
        animationToggle.HorizontalAlignment(mux::HorizontalAlignment::Right);
        animationToggle.UseSystemFocusVisuals(true);
        animationStatus = MakeBodyText(0.72);
        animationStatus.Visibility(mux::Visibility::Collapsed);
        animationControls.Children().Append(animationToggle);
        animationControls.Children().Append(animationStatus);
        animationRow.Initialize(animationControls, 420.0);
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
        crashSection.content.Children().Append(crashExpander);
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

    void RefreshLocalizedText()
    {
        if (closed)
            return;

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
        versionRow.SetText(L("app.settings.version", L"Version"),
            L("settings.about.version.description",
                L"Check the installed version and update status."));
        SetSectionTitle(thirdPartySection,
            "app.settings.third_party_libs", L"Third-Party Libraries");
        SetSectionTitle(referenceSection,
            "app.settings.reference_programs",
            L"Upstream Code Attribution");
        for (LinkEntry& link : links)
        {
            const std::wstring text = L(link.labelKey, link.fallback);
            link.button.Content(winrt::box_value(text));
            SetAutomation(link.button, text, HomeAboutLinkUri(link.target));
        }

        SetSectionTitle(debugTitleSection,
            "app.settings.debug_page", L"Debug Page");
        debugPageDescription.Text(L("settings.page.debug.description",
            L"Diagnostics available only while Debug is unlocked."));
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
        if (newGeneration || generalRevision !=
                snapshot.domainRevisions.general)
        {
            generalRevision = snapshot.domainRevisions.general;
            demoModeEnabled = snapshot.values.general.demoModeEnabled;
        }
        hasSnapshot = true;
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
            if (focusId == "about.profile") return links[0].button;
            if (focusId == "about.project") return links[4].button;
            if (focusId == "about.website") return links[6].button;
            if (focusId == "about.community") return links[7].button;
            if (focusId == "about.thirdparty") return links[8].button;
            return versionButton;
        }
        if (page == SettingsPage::Debug)
        {
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
            checkUpdateButton.Click(checkUpdateToken);
            versionButton.Click(versionClickToken);
            demoModeToggle.Toggled(demoModeToken);
            initializationToggle.Toggled(initializationToken);
            animationToggle.Toggled(animationToken);
            resetUnlockButton.Click(resetUnlockToken);
            crashButton.Click(crashToken);
        }
        catch (...)
        {
        }
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
        impl_->active = false;
    }
}

mux::FrameworkElement HomeAboutPagePresenter::FocusTarget(
    SettingsPage page,
    std::string_view focusId) const noexcept
{
    return impl_ ? impl_->FocusTarget(page, focusId) : nullptr;
}

void HomeAboutPagePresenter::Close() noexcept
{
    if (impl_)
        impl_->Close();
}

} // namespace snowdesktop::winui
