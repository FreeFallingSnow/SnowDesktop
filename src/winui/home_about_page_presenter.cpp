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

std::wstring FormatOne(std::wstring text, std::wstring_view value)
{
    constexpr std::wstring_view token = L"{0}";
    if (const std::size_t position = text.find(token);
        position != std::wstring::npos)
    {
        text.replace(position, token.size(), value);
        return text;
    }
    if (!value.empty())
    {
        if (!text.empty())
            text.push_back(L' ');
        text.append(value);
    }
    return text;
}

struct HomeCard
{
    muxc::Button root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    muxc::StackPanel statusRow{nullptr};
    muxc::TextBlock value{nullptr};
    muxc::ProgressRing progress{nullptr};
    muxc::TextBlock description{nullptr};
    SettingsRoute route;
    winrt::event_token clickToken{};
};

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

void InitializeHomeCard(
    HomeCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page,
    SettingsRoute route)
{
    card.root = muxc::Button{};
    card.root.Style(style);
    card.root.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    card.root.HorizontalContentAlignment(
        mux::HorizontalAlignment::Stretch);
    card.root.UseSystemFocusVisuals(true);
    card.route = std::move(route);

    card.content = muxc::StackPanel{};
    card.content.Spacing(5.0);
    card.content.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.statusRow = muxc::StackPanel{};
    card.statusRow.Orientation(muxc::Orientation::Horizontal);
    card.statusRow.Spacing(8.0);
    card.value = muxc::TextBlock{};
    card.value.TextWrapping(mux::TextWrapping::Wrap);
    card.value.VerticalAlignment(mux::VerticalAlignment::Center);
    card.progress = muxc::ProgressRing{};
    card.progress.Width(18.0);
    card.progress.Height(18.0);
    card.progress.IsActive(false);
    card.progress.Visibility(mux::Visibility::Collapsed);
    card.statusRow.Children().Append(card.progress);
    card.statusRow.Children().Append(card.value);
    card.description = muxc::TextBlock{};
    card.description.Opacity(0.72);
    card.description.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.content.Children().Append(card.statusRow);
    card.content.Children().Append(card.description);
    card.root.Content(card.content);
    page.Children().Append(card.root);
}

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
    muxc::StackPanel homeRoot{nullptr};
    muxc::StackPanel aboutRoot{nullptr};
    muxc::StackPanel debugRoot{nullptr};

    HomeCard themeCard;
    HomeCard dockCard;
    HomeCard widgetCard;
    HomeCard updateCard;
    HomeCard backupCard;

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
    muxc::StackPanel versionStatusRow{nullptr};
    muxc::ProgressRing updateProgress{nullptr};
    muxc::TextBlock updateStatus{nullptr};
    muxc::InfoBar updateInfoBar{nullptr};
    muxc::Button checkUpdateButton{nullptr};

    Section debugTitleSection;
    Section demoModeSection;
    Section animationSection;
    Section crashSection;
    muxc::TextBlock debugPageDescription{nullptr};
    controls::SettingRow demoModeRow;
    muxc::ToggleSwitch demoModeToggle{nullptr};
    controls::SettingRow animationRow;
    muxc::StackPanel animationControls{nullptr};
    muxc::ToggleSwitch animationToggle{nullptr};
    muxc::TextBlock animationStatus{nullptr};
    muxc::Expander crashExpander{nullptr};
    muxc::StackPanel crashHeader{nullptr};
    muxc::TextBlock crashTitle{nullptr};
    muxc::TextBlock crashDescription{nullptr};
    muxc::Grid crashActionHost{nullptr};
    muxc::Button crashButton{nullptr};

    std::uint64_t generation = 0;
    std::uint64_t personalizationRevision = 0;
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
    bool debugUnlocked = false;
    unsigned versionClickCount = 0;

    int themePreset = kAppearancePresetDark;
    bool dockEnabled = false;
    std::wstring applicationVersion;
    std::optional<std::size_t> installedWidgetCount;
    SettingsUpdateState updateState = SettingsUpdateState::Unknown;
    std::wstring availableVersion;
    std::wstring updateDetail;
    SettingsBackupState backupState = SettingsBackupState::Unknown;
    std::size_t backupCount = 0;
    std::wstring backupDetail;
    std::wstring animationDiagnosticsStatus;

    winrt::event_token themeClickToken{};
    winrt::event_token dockClickToken{};
    winrt::event_token widgetClickToken{};
    winrt::event_token updateClickToken{};
    winrt::event_token backupClickToken{};
    winrt::event_token checkUpdateToken{};
    winrt::event_token versionClickToken{};
    winrt::event_token demoModeToken{};
    winrt::event_token animationToken{};
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
        homeRoot = muxc::StackPanel{};
        homeRoot.Spacing(8.0);
        InitializeHomeCard(themeCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::Personalization));
        InitializeHomeCard(dockCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::DockAndTaskbar));
        InitializeHomeCard(widgetCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::Widgets));
        InitializeHomeCard(updateCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::About, "about.version"));
        InitializeHomeCard(backupCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::BackupAndData));

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
            HomeAboutLink::ReleaseRepository, {}, L"GitHub (Release)");
        (void)AddLink(projectSection.content,
            HomeAboutLink::SourceRepository, {}, L"GitHub (Source)");

        InitializeSection(communitySection, cardStyle, aboutRoot);
        (void)AddLink(communitySection.content,
            HomeAboutLink::QqGroup,
            "app.settings.join_qq", L"Join QQ Group: 976422547");

        InitializeSection(versionSection, cardStyle, aboutRoot);
        versionSection.title.Visibility(mux::Visibility::Collapsed);
        versionControls = muxc::StackPanel{};
        versionControls.Spacing(7.0);
        versionButton = muxc::Button{};
        versionButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
        versionButton.UseSystemFocusVisuals(true);
        versionStatusRow = muxc::StackPanel{};
        versionStatusRow.Orientation(muxc::Orientation::Horizontal);
        versionStatusRow.Spacing(8.0);
        updateProgress = muxc::ProgressRing{};
        updateProgress.Width(18.0);
        updateProgress.Height(18.0);
        updateProgress.IsActive(false);
        updateProgress.Visibility(mux::Visibility::Collapsed);
        updateStatus = MakeBodyText(0.72);
        updateStatus.VerticalAlignment(mux::VerticalAlignment::Center);
        muxa::AutomationProperties::SetLiveSetting(updateStatus,
            muxa::Peers::AutomationLiveSetting::Polite);
        versionStatusRow.Children().Append(updateProgress);
        versionStatusRow.Children().Append(updateStatus);
        updateInfoBar = muxc::InfoBar{};
        updateInfoBar.IsClosable(false);
        updateInfoBar.IsOpen(false);
        checkUpdateButton = muxc::Button{};
        checkUpdateButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
        checkUpdateButton.UseSystemFocusVisuals(true);
        versionControls.Children().Append(versionButton);
        versionControls.Children().Append(versionStatusRow);
        versionControls.Children().Append(updateInfoBar);
        versionControls.Children().Append(checkUpdateButton);
        versionRow.Initialize(versionControls, 360.0);
        versionSection.content.Children().Append(versionRow.root);

        InitializeSection(thirdPartySection, cardStyle, aboutRoot);
        InitializeSection(referenceSection, cardStyle, aboutRoot);
        AddAttribution(HomeAboutLink::EverythingSdk, L"Everything SDK",
            L"(MIT)", L"Copyright (C) 2016 David Carpenter");
        AddAttribution(HomeAboutLink::DearImGui, L"Dear ImGui", L"(MIT)",
            L"Copyright (c) 2014-2025 Omar Cornut");
        AddAttribution(HomeAboutLink::Lua, L"Lua", L"(MIT)",
            L"Copyright (C) 1994-2024 Lua.org, PUC-Rio");
        AddAttribution(HomeAboutLink::Spdlog, L"spdlog", L"(MIT)",
            L"Copyright (c) 2016-present, Gabi Melman");
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
        const auto navigate = [this](HomeCard& card) {
            const SettingsRoute route = card.route;
            card.clickToken = card.root.Click(
                [this, route](const auto&, const auto&) {
                    if (CanInvoke() && actions.navigate)
                        actions.navigate(route);
                });
        };
        navigate(themeCard);
        themeClickToken = themeCard.clickToken;
        navigate(dockCard);
        dockClickToken = dockCard.clickToken;
        navigate(widgetCard);
        widgetClickToken = widgetCard.clickToken;
        navigate(updateCard);
        updateClickToken = updateCard.clickToken;
        navigate(backupCard);
        backupClickToken = backupCard.clickToken;

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
                if (updateState != SettingsUpdateState::Checking)
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

    [[nodiscard]] std::wstring ThemeSummary() const
    {
        switch (NormalizeAppearancePresetId(themePreset))
        {
        case kAppearancePresetLight:
            return L("app.settings.light", L"Light");
        case kAppearancePresetGlassDark:
            return L("app.settings.dark_glass", L"Dark Glass");
        case kAppearancePresetGlassLight:
            return L("app.settings.light_glass", L"Light Glass");
        case kAppearancePresetAcrylicDark:
            return L("app.settings.dark_acrylic", L"Dark Acrylic");
        case kAppearancePresetAcrylicLight:
            return L("app.settings.light_acrylic", L"Light Acrylic");
        case kAppearancePresetCustom:
            return L("app.settings.custom", L"Custom");
        default:
            return L("app.settings.dark", L"Dark");
        }
    }

    [[nodiscard]] std::wstring UpdateSummary() const
    {
        if (!updateDetail.empty())
            return updateDetail;
        switch (updateState)
        {
        case SettingsUpdateState::Checking:
            return L("app.settings.checking", L"Checking...");
        case SettingsUpdateState::UpToDate:
            return L("app.settings.already_latest", L"Already up to date");
        case SettingsUpdateState::UpdateAvailable:
            return FormatOne(L("app.settings.new_version",
                L"New version v{0} available"), availableVersion);
        case SettingsUpdateState::ManagedByStore:
            return L("app.settings.store_managed_updates",
                L"Updates are managed automatically by Microsoft Store");
        case SettingsUpdateState::Failed:
            return L("settings.home.update.failed",
                L"Could not check for updates");
        default:
            return L("settings.home.update.unknown", L"Not checked");
        }
    }

    [[nodiscard]] std::wstring BackupSummary() const
    {
        if (!backupDetail.empty())
            return backupDetail;
        switch (backupState)
        {
        case SettingsBackupState::Empty:
            return L("app.settings.no_backups", L"No backups yet");
        case SettingsBackupState::Ready:
            return backupCount != 0
                ? std::to_wstring(backupCount)
                : L("settings.home.backup.ready", L"Backups available");
        case SettingsBackupState::Running:
            return L("app.settings.checking", L"Working...");
        case SettingsBackupState::Succeeded:
            return L("app.settings.create_full_backup_success",
                L"The complete backup was created.");
        case SettingsBackupState::Failed:
            return L("app.settings.create_full_backup_failed",
                L"Could not create the complete backup.");
        default:
            return L("settings.home.backup.unknown", L"Status unavailable");
        }
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

    void SetHomeCardText(
        HomeCard& card,
        std::string_view titleKey,
        std::wstring_view titleFallback,
        std::string_view descriptionKey,
        std::wstring_view descriptionFallback)
    {
        card.title.Text(L(titleKey, titleFallback));
        card.description.Text(L(descriptionKey, descriptionFallback));
        SetAutomation(card.root, card.title.Text(), card.description.Text());
    }

    void RenderStatus()
    {
        if (closed)
            return;
        themeCard.value.Text(ThemeSummary());
        dockCard.value.Text(dockEnabled
            ? L("app.settings.widgets_enabled", L"Enabled")
            : L("app.settings.widgets_disabled", L"Disabled"));
        widgetCard.value.Text(installedWidgetCount
            ? std::to_wstring(*installedWidgetCount)
            : L("settings.home.widgets.unknown", L"Status unavailable"));

        const bool updateRunning =
            updateState == SettingsUpdateState::Checking;
        updateCard.progress.IsActive(updateRunning);
        updateCard.progress.Visibility(updateRunning
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        updateCard.value.Text(UpdateSummary());
        const bool backupRunning =
            backupState == SettingsBackupState::Running;
        backupCard.progress.IsActive(backupRunning);
        backupCard.progress.Visibility(backupRunning
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        backupCard.value.Text(BackupSummary());

        const std::wstring version = applicationVersion.empty()
            ? L("settings.about.version.unknown", L"Version unavailable")
            : L"SnowDesktop v" + applicationVersion;
        versionButton.Content(winrt::box_value(version));
        SetAutomation(versionButton, version,
            L("settings.page.debug.description",
                L"Click five times to unlock Debug."));
        updateProgress.IsActive(updateRunning);
        updateProgress.Visibility(updateRunning
            ? mux::Visibility::Visible : mux::Visibility::Collapsed);
        updateStatus.Text(UpdateSummary());
        checkUpdateButton.IsEnabled(!updateRunning);
        SetButtonText(checkUpdateButton,
            "app.settings.check_update", L"Check for Updates");
        const bool showUpdateInfo =
            updateState == SettingsUpdateState::UpdateAvailable ||
            updateState == SettingsUpdateState::Failed;
        updateInfoBar.IsOpen(showUpdateInfo);
        updateInfoBar.Severity(
            updateState == SettingsUpdateState::Failed
                ? muxc::InfoBarSeverity::Error
                : muxc::InfoBarSeverity::Informational);
        updateInfoBar.Message(showUpdateInfo ? UpdateSummary() : L"");

        updatingControls = true;
        demoModeToggle.IsOn(demoModeEnabled);
        animationToggle.IsOn(animationDiagnosticsEnabled);
        updatingControls = false;
        animationStatus.Text(animationDiagnosticsStatus);
        animationStatus.Visibility(
            animationDiagnosticsEnabled &&
                !animationDiagnosticsStatus.empty()
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);

        const auto updateCardHelp = [](const HomeCard& card) {
            std::wstring help = card.description.Text().c_str();
            if (!help.empty() && !card.value.Text().empty())
                help.append(L" ");
            help.append(card.value.Text().c_str());
            muxa::AutomationProperties::SetHelpText(card.root, help);
        };
        updateCardHelp(themeCard);
        updateCardHelp(dockCard);
        updateCardHelp(widgetCard);
        updateCardHelp(updateCard);
        updateCardHelp(backupCard);
        SetAutomation(updateStatus, updateStatus.Text());
    }

    void RefreshLocalizedText()
    {
        if (closed)
            return;
        SetHomeCardText(themeCard,
            "settings.home.theme", L"Current theme",
            "settings.home.theme.description",
            L"Review colors, materials and appearance.");
        SetHomeCardText(dockCard,
            "settings.home.dock", L"Dock status",
            "settings.home.dock.description",
            L"Open Dock and taskbar settings.");
        SetHomeCardText(widgetCard,
            "settings.home.widgets", L"Installed widgets",
            "settings.home.widgets.description",
            L"Manage widgets and their settings.");
        SetHomeCardText(updateCard,
            "settings.home.update", L"Updates",
            "settings.home.update.description",
            L"Review the installed version and update status.");
        SetHomeCardText(backupCard,
            "settings.home.backup", L"Backup status",
            "settings.home.backup.description",
            L"Create or restore a backup.");

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
        SetSectionTitle(animationSection,
            "app.settings.animation_diagnostics",
            L"Animation diagnostics (this session)");
        animationRow.SetText(
            L("app.settings.animation_diagnostics",
                L"Animation diagnostics (this session)"),
            L("app.settings.animation_diagnostics_desc"));
        SetAutomation(animationToggle,
            animationRow.label.Text(), animationRow.help.Text());
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
        SetAutomation(updateInfoBar,
            L("app.settings.version", L"Version"));
        RenderStatus();
    }

    void ResetStatusForGeneration()
    {
        statusRevision = 0;
        hasStatusRevision = false;
        applicationVersion.clear();
        installedWidgetCount.reset();
        updateState = SettingsUpdateState::Unknown;
        availableVersion.clear();
        updateDetail.clear();
        backupState = SettingsBackupState::Unknown;
        backupCount = 0;
        backupDetail.clear();
        animationDiagnosticsEnabled = false;
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
        if (newGeneration || personalizationRevision !=
                snapshot.domainRevisions.personalization)
        {
            personalizationRevision =
                snapshot.domainRevisions.personalization;
            themePreset = snapshot.values.personalization.backgroundPreset;
        }
        if (newGeneration || generalRevision !=
                snapshot.domainRevisions.general)
        {
            generalRevision = snapshot.domainRevisions.general;
            dockEnabled = snapshot.values.general.dockEnabled;
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
        if (patch.installedWidgetCount)
            installedWidgetCount = *patch.installedWidgetCount;
        if (patch.updateState)
        {
            if (*patch.updateState != updateState)
            {
                updateDetail.clear();
                if (*patch.updateState !=
                    SettingsUpdateState::UpdateAvailable)
                {
                    availableVersion.clear();
                }
            }
            updateState = *patch.updateState;
        }
        if (patch.availableVersion)
            availableVersion = *patch.availableVersion;
        if (patch.updateDetail)
            updateDetail = *patch.updateDetail;
        if (patch.backupState)
        {
            if (*patch.backupState != backupState)
                backupDetail.clear();
            backupState = *patch.backupState;
        }
        if (patch.backupCount)
            backupCount = *patch.backupCount;
        if (patch.backupDetail)
            backupDetail = *patch.backupDetail;
        if (patch.animationDiagnosticsEnabled)
            animationDiagnosticsEnabled =
                *patch.animationDiagnosticsEnabled;
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
        if (page == SettingsPage::Home)
        {
            if (focusId == "home.theme") return themeCard.root;
            if (focusId == "home.dock") return dockCard.root;
            if (focusId == "home.widgets") return widgetCard.root;
            if (focusId == "home.update") return updateCard.root;
            if (focusId == "home.backup") return backupCard.root;
            return themeCard.root;
        }
        if (page == SettingsPage::About)
        {
            if (focusId == "about.profile") return links[0].button;
            if (focusId == "about.project") return links[4].button;
            if (focusId == "about.community") return links[6].button;
            if (focusId == "about.thirdparty") return links[7].button;
            return versionButton;
        }
        if (page == SettingsPage::Debug)
        {
            if (focusId == "debug.demo_mode") return demoModeToggle;
            if (focusId == "debug.animation") return animationToggle;
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
            themeCard.root.Click(themeClickToken);
            dockCard.root.Click(dockClickToken);
            widgetCard.root.Click(widgetClickToken);
            updateCard.root.Click(updateClickToken);
            backupCard.root.Click(backupClickToken);
            for (LinkEntry& link : links)
                link.button.Click(link.clickToken);
            checkUpdateButton.Click(checkUpdateToken);
            versionButton.Click(versionClickToken);
            demoModeToggle.Toggled(demoModeToken);
            animationToggle.Toggled(animationToken);
            crashButton.Click(crashToken);
            updateCard.progress.IsActive(false);
            backupCard.progress.IsActive(false);
            updateProgress.IsActive(false);
            updateInfoBar.IsOpen(false);
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
        impl_->actions = std::move(actions);
}

mux::UIElement HomeAboutPagePresenter::HomeContent() const noexcept
{
    return impl_ ? impl_->homeRoot : nullptr;
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
    impl_->active = page == SettingsPage::Home ||
        page == SettingsPage::About || page == SettingsPage::Debug;
    impl_->activePage = page;
}

void HomeAboutPagePresenter::Deactivate() noexcept
{
    if (impl_ && !impl_->closed)
        impl_->active = false;
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
