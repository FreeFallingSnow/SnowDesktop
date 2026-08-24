#include "pch.h"

#include "home_about_page_presenter.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>

#include <utility>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;

namespace
{

std::wstring FormatOne(std::wstring text, std::wstring_view value)
{
    const std::wstring token = L"{0}";
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

struct AboutCard
{
    muxc::Border root{nullptr};
    muxc::StackPanel content{nullptr};
    muxc::TextBlock title{nullptr};
    muxc::TextBlock description{nullptr};
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

void InitializeAboutCard(
    AboutCard& card,
    const mux::Style& style,
    const muxc::StackPanel& page)
{
    card.root = muxc::Border{};
    card.root.Style(style);
    card.content = muxc::StackPanel{};
    card.content.Spacing(10.0);
    card.title = muxc::TextBlock{};
    card.title.FontWeight(
        winrt::Windows::UI::Text::FontWeights::SemiBold());
    card.title.TextWrapping(mux::TextWrapping::Wrap);
    card.description = muxc::TextBlock{};
    card.description.Opacity(0.72);
    card.description.TextWrapping(mux::TextWrapping::Wrap);
    card.content.Children().Append(card.title);
    card.content.Children().Append(card.description);
    card.root.Child(card.content);
    page.Children().Append(card.root);
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

    HomeCard themeCard;
    HomeCard dockCard;
    HomeCard widgetCard;
    HomeCard updateCard;
    HomeCard backupCard;

    AboutCard versionCard;
    AboutCard projectCard;
    AboutCard thirdPartyCard;
    muxc::StackPanel versionStatusRow{nullptr};
    muxc::TextBlock installedVersion{nullptr};
    muxc::ProgressRing updateProgress{nullptr};
    muxc::TextBlock updateStatus{nullptr};
    muxc::InfoBar updateInfoBar{nullptr};
    muxc::Button checkUpdateButton{nullptr};
    muxc::Button projectButton{nullptr};
    muxc::Button licenseButton{nullptr};
    muxc::Button thirdPartyButton{nullptr};

    std::uint64_t generation = 0;
    std::uint64_t personalizationRevision = 0;
    std::uint64_t generalRevision = 0;
    std::uint64_t statusRevision = 0;
    bool hasSnapshot = false;
    bool hasStatusRevision = false;
    bool active = false;
    SettingsPage activePage = SettingsPage::Home;
    bool closed = false;

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

    winrt::event_token themeClickToken{};
    winrt::event_token dockClickToken{};
    winrt::event_token widgetClickToken{};
    winrt::event_token updateClickToken{};
    winrt::event_token backupClickToken{};
    winrt::event_token checkUpdateToken{};
    winrt::event_token projectToken{};
    winrt::event_token licenseToken{};
    winrt::event_token thirdPartyToken{};

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback) const
    {
        if (localize)
        {
            std::wstring value = localize(key);
            if (!value.empty())
                return value;
        }
        return std::wstring(fallback);
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
            SettingsRoute::ForPage(
                SettingsPage::About, "about.version"));
        InitializeHomeCard(backupCard, navigationCardStyle, homeRoot,
            SettingsRoute::ForPage(SettingsPage::BackupAndData));

        aboutRoot = muxc::StackPanel{};
        aboutRoot.Spacing(8.0);
        InitializeAboutCard(versionCard, cardStyle, aboutRoot);
        versionStatusRow = muxc::StackPanel{};
        versionStatusRow.Orientation(muxc::Orientation::Horizontal);
        versionStatusRow.Spacing(8.0);
        installedVersion = muxc::TextBlock{};
        installedVersion.TextWrapping(mux::TextWrapping::Wrap);
        installedVersion.VerticalAlignment(mux::VerticalAlignment::Center);
        updateProgress = muxc::ProgressRing{};
        updateProgress.Width(18.0);
        updateProgress.Height(18.0);
        updateProgress.IsActive(false);
        updateProgress.Visibility(mux::Visibility::Collapsed);
        updateStatus = muxc::TextBlock{};
        updateStatus.TextWrapping(mux::TextWrapping::Wrap);
        updateStatus.VerticalAlignment(mux::VerticalAlignment::Center);
        muxa::AutomationProperties::SetLiveSetting(updateStatus,
            muxa::Peers::AutomationLiveSetting::Polite);
        versionStatusRow.Children().Append(installedVersion);
        versionStatusRow.Children().Append(updateProgress);
        versionStatusRow.Children().Append(updateStatus);
        versionCard.content.Children().Append(versionStatusRow);
        updateInfoBar = muxc::InfoBar{};
        updateInfoBar.IsClosable(false);
        updateInfoBar.IsOpen(false);
        versionCard.content.Children().Append(updateInfoBar);
        checkUpdateButton = muxc::Button{};
        checkUpdateButton.HorizontalAlignment(
            mux::HorizontalAlignment::Left);
        checkUpdateButton.UseSystemFocusVisuals(true);
        versionCard.content.Children().Append(checkUpdateButton);

        InitializeAboutCard(projectCard, cardStyle, aboutRoot);
        projectButton = muxc::Button{};
        projectButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
        projectButton.UseSystemFocusVisuals(true);
        licenseButton = muxc::Button{};
        licenseButton.HorizontalAlignment(mux::HorizontalAlignment::Left);
        licenseButton.UseSystemFocusVisuals(true);
        projectCard.content.Children().Append(projectButton);
        projectCard.content.Children().Append(licenseButton);

        InitializeAboutCard(thirdPartyCard, cardStyle, aboutRoot);
        thirdPartyButton = muxc::Button{};
        thirdPartyButton.HorizontalAlignment(
            mux::HorizontalAlignment::Left);
        thirdPartyButton.UseSystemFocusVisuals(true);
        thirdPartyCard.content.Children().Append(thirdPartyButton);
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

        checkUpdateToken = checkUpdateButton.Click(
            [this](const auto&, const auto&) {
                Invoke(HomeAboutCommand::CheckForUpdates);
            });
        projectToken = projectButton.Click(
            [this](const auto&, const auto&) {
                Invoke(HomeAboutCommand::OpenProject);
            });
        licenseToken = licenseButton.Click(
            [this](const auto&, const auto&) {
                Invoke(HomeAboutCommand::OpenLicense);
            });
        thirdPartyToken = thirdPartyButton.Click(
            [this](const auto&, const auto&) {
                Invoke(HomeAboutCommand::OpenThirdPartyNotices);
            });
    }

    [[nodiscard]] bool CanInvoke() const noexcept
    {
        return !closed && active && hasSnapshot;
    }

    void Invoke(HomeAboutCommand command)
    {
        if (CanInvoke() && activePage == SettingsPage::About &&
            actions.invoke)
        {
            actions.invoke(generation, command);
        }
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
            if (backupCount != 0)
                return std::to_wstring(backupCount);
            return L("settings.home.backup.ready", L"Backups available");
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
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        updateCard.value.Text(UpdateSummary());

        const bool backupRunning =
            backupState == SettingsBackupState::Running;
        backupCard.progress.IsActive(backupRunning);
        backupCard.progress.Visibility(backupRunning
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        backupCard.value.Text(BackupSummary());

        installedVersion.Text(applicationVersion.empty()
            ? L("settings.about.version.unknown", L"Version unavailable")
            : applicationVersion);
        updateProgress.IsActive(updateRunning);
        updateProgress.Visibility(updateRunning
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
        updateStatus.Text(UpdateSummary());
        checkUpdateButton.IsEnabled(!updateRunning &&
            updateState != SettingsUpdateState::ManagedByStore);

        const bool showUpdateInfo =
            updateState == SettingsUpdateState::UpdateAvailable ||
            updateState == SettingsUpdateState::Failed;
        updateInfoBar.IsOpen(showUpdateInfo);
        updateInfoBar.Severity(
            updateState == SettingsUpdateState::Failed
                ? muxc::InfoBarSeverity::Error
                : muxc::InfoBarSeverity::Informational);
        updateInfoBar.Message(showUpdateInfo ? UpdateSummary() : L"");

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
        muxa::AutomationProperties::SetName(
            updateStatus, updateStatus.Text());
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
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
        muxa::AutomationProperties::SetHelpText(
            card.root, card.description.Text());
    }

    void SetAboutCardText(
        AboutCard& card,
        std::string_view titleKey,
        std::wstring_view titleFallback,
        std::string_view descriptionKey,
        std::wstring_view descriptionFallback)
    {
        card.title.Text(L(titleKey, titleFallback));
        card.description.Text(L(descriptionKey, descriptionFallback));
        muxa::AutomationProperties::SetName(card.root, card.title.Text());
        muxa::AutomationProperties::SetHelpText(
            card.root, card.description.Text());
    }

    void SetButtonText(
        const muxc::Button& button,
        std::string_view key,
        std::wstring_view fallback)
    {
        const std::wstring text = L(key, fallback);
        button.Content(winrt::box_value(text));
        muxa::AutomationProperties::SetName(button, text);
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

        SetAboutCardText(versionCard,
            "settings.about.version", L"Version and updates",
            "settings.about.version.description",
            L"Check the installed version and update status.");
        SetAboutCardText(projectCard,
            "settings.about.project", L"Project and licenses",
            "settings.about.project.description",
            L"Open project links and license information.");
        SetAboutCardText(thirdPartyCard,
            "settings.about.thirdparty", L"Third-party notices",
            "settings.about.thirdparty.description",
            L"Review third-party software information.");

        SetButtonText(checkUpdateButton,
            "app.settings.check_update", L"Check for Updates");
        SetButtonText(projectButton,
            "app.settings.project_url", L"Project URL");
        SetButtonText(licenseButton,
            "app.settings.copyright", L"Copyright & License");
        SetButtonText(thirdPartyButton,
            "app.settings.third_party_libs", L"Third-Party Libraries");
        muxa::AutomationProperties::SetName(updateInfoBar,
            L("settings.about.version", L"Version and updates"));
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
            if (focusId == "about.version") return checkUpdateButton;
            if (focusId == "about.project") return projectButton;
            if (focusId == "about.license") return licenseButton;
            if (focusId == "about.thirdparty") return thirdPartyButton;
            return checkUpdateButton;
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
            checkUpdateButton.Click(checkUpdateToken);
            projectButton.Click(projectToken);
            licenseButton.Click(licenseToken);
            thirdPartyButton.Click(thirdPartyToken);
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
    impl_->active = page == SettingsPage::Home || page == SettingsPage::About;
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
