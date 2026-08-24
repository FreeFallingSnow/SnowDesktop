#include "pch.h"

#include "SettingsShell.xaml.h"
#if __has_include("SettingsShell.g.cpp")
#include "SettingsShell.g.cpp"
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace winrt::SnowDesktop::implementation
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace wfc = winrt::Windows::Foundation::Collections;

namespace
{
using snowdesktop::SettingsPage;
using snowdesktop::SettingsRoute;

struct LocalizedFallback
{
    std::string_view key;
    std::wstring_view value;
};

constexpr std::array kFallbackStrings{
    LocalizedFallback{"settings.shell.title", L"Settings"},
    LocalizedFallback{"settings.search.placeholder", L"Find a setting"},
    LocalizedFallback{"settings.search.clear", L"Clear search"},
    LocalizedFallback{"settings.progress.cancel", L"Cancel"},
    LocalizedFallback{"settings.nav.home", L"Home"},
    LocalizedFallback{"settings.nav.general", L"General"},
    LocalizedFallback{"settings.nav.personalization", L"Personalization"},
    LocalizedFallback{"settings.nav.desktop", L"Desktop"},
    LocalizedFallback{"settings.nav.dock", L"Dock & taskbar"},
    LocalizedFallback{"settings.nav.widgets", L"Widgets"},
    LocalizedFallback{"settings.nav.backup", L"Backup & data"},
    LocalizedFallback{"settings.nav.about", L"About"},
    LocalizedFallback{"settings.nav.developer", L"Developer tools"},
    LocalizedFallback{"settings.nav.debug", L"Debug"},
    LocalizedFallback{"settings.page.home.description", L"Your most-used SnowDesktop settings and status at a glance."},
    LocalizedFallback{"settings.page.general.description", L"Startup, language, navigation, hotkeys and everyday behavior."},
    LocalizedFallback{"settings.page.personalization.description", L"Theme, materials, colors, menus and widget appearance."},
    LocalizedFallback{"settings.page.desktop.description", L"Desktop icon layout, appearance and category rules."},
    LocalizedFallback{"settings.page.dock.description", L"Configure the Dock and the Windows taskbar."},
    LocalizedFallback{"settings.page.widgets.description", L"Manage installed widgets, sources, permissions and instances."},
    LocalizedFallback{"settings.page.widget.description", L"Declarative settings for this widget instance."},
    LocalizedFallback{"settings.page.backup.description", L"Back up, restore, migrate and open SnowDesktop data."},
    LocalizedFallback{"settings.page.about.description", L"Version, updates, project links, licenses and third-party notices."},
    LocalizedFallback{"settings.page.developer.description", L"Development overrides and widget authoring tools."},
    LocalizedFallback{"settings.page.debug.description", L"Diagnostics available only while Debug is unlocked."},
    LocalizedFallback{"settings.home.theme", L"Current theme"},
    LocalizedFallback{"settings.home.theme.description", L"Review colors, materials and appearance."},
    LocalizedFallback{"settings.home.dock", L"Dock status"},
    LocalizedFallback{"settings.home.dock.description", L"Open Dock and taskbar settings."},
    LocalizedFallback{"settings.home.widgets", L"Installed widgets"},
    LocalizedFallback{"settings.home.widgets.description", L"Manage widgets and their settings."},
    LocalizedFallback{"settings.home.backup", L"Backup status"},
    LocalizedFallback{"settings.home.backup.description", L"Create or restore a backup."},
    LocalizedFallback{"settings.general.startup", L"Startup and desktop"},
    LocalizedFallback{"settings.general.startup.description", L"Control startup and software desktop behavior."},
    LocalizedFallback{"settings.general.language", L"Language"},
    LocalizedFallback{"settings.general.language.description", L"Choose the language used by SnowDesktop."},
    LocalizedFallback{"settings.general.hotkeys", L"Keyboard shortcuts"},
    LocalizedFallback{"settings.general.hotkeys.description", L"Record and check global shortcuts."},
    LocalizedFallback{"settings.general.navigation", L"Navigation and interaction"},
    LocalizedFallback{"settings.general.navigation.description", L"Configure paging, click-through and double-click behavior."},
    LocalizedFallback{"settings.personalization.theme", L"Theme and material"},
    LocalizedFallback{"settings.personalization.theme.description", L"Choose light, dark, glass or acrylic appearance."},
    LocalizedFallback{"settings.personalization.colors", L"Colors and gradients"},
    LocalizedFallback{"settings.personalization.colors.description", L"Customize accents, surfaces and gradients."},
    LocalizedFallback{"settings.personalization.menu", L"Context menu"},
    LocalizedFallback{"settings.personalization.menu.description", L"Configure SnowDesktop context-menu appearance."},
    LocalizedFallback{"settings.personalization.widgets", L"Widget appearance"},
    LocalizedFallback{"settings.personalization.widgets.description", L"Set shared widget appearance defaults."},
    LocalizedFallback{"settings.desktop.layout", L"Icon layout"},
    LocalizedFallback{"settings.desktop.layout.description", L"Adjust icon size, spacing, fonts and shortcut arrows."},
    LocalizedFallback{"settings.desktop.beautify", L"Icon beautification"},
    LocalizedFallback{"settings.desktop.beautify.description", L"Apply complete icon appearance rules."},
    LocalizedFallback{"settings.desktop.categories", L"Categories"},
    LocalizedFallback{"settings.desktop.categories.description", L"Manage category tabs, layout and matching rules."},
    LocalizedFallback{"settings.dock.dock", L"Dock"},
    LocalizedFallback{"settings.dock.dock.description", L"Enable, position and size the Dock."},
    LocalizedFallback{"settings.dock.items", L"Dock items and monitors"},
    LocalizedFallback{"settings.dock.items.description", L"Choose monitors, layout and common items."},
    LocalizedFallback{"settings.dock.taskbar", L"Taskbar"},
    LocalizedFallback{"settings.dock.taskbar.description", L"Set theme, dynamic rules, alignment and auto-hide."},
    LocalizedFallback{"settings.widgets.installed", L"Installed widgets"},
    LocalizedFallback{"settings.widgets.installed.description", L"Search, enable, disable or uninstall widgets."},
    LocalizedFallback{"settings.widgets.sources", L"Sources and Workshop"},
    LocalizedFallback{"settings.widgets.sources.description", L"Install widgets and synchronize sources."},
    LocalizedFallback{"settings.widgets.permissions", L"Permissions"},
    LocalizedFallback{"settings.widgets.permissions.description", L"Review widget capabilities and owner-scoped access."},
    LocalizedFallback{"settings.widget.fields", L"Widget settings"},
    LocalizedFallback{"settings.widget.fields.description", L"Fields supplied by the widget's v2 settings schema appear here."},
    LocalizedFallback{"settings.backup.layout", L"Layout backup"},
    LocalizedFallback{"settings.backup.layout.description", L"Create and restore desktop-layout backups."},
    LocalizedFallback{"settings.backup.full", L"Full backup and migration"},
    LocalizedFallback{"settings.backup.full.description", L"Export, import, restore or migrate all data."},
    LocalizedFallback{"settings.backup.directory", L"Data directory"},
    LocalizedFallback{"settings.backup.directory.description", L"Open the current SnowDesktop data directory."},
    LocalizedFallback{"settings.about.version", L"Version and updates"},
    LocalizedFallback{"settings.about.version.description", L"Check the installed version and update status."},
    LocalizedFallback{"settings.about.project", L"Project and licenses"},
    LocalizedFallback{"settings.about.project.description", L"Open project links and license information."},
    LocalizedFallback{"settings.about.thirdparty", L"Third-party notices"},
    LocalizedFallback{"settings.about.thirdparty.description", L"Review third-party software information."},
    LocalizedFallback{"settings.developer.overrides", L"Development overrides"},
    LocalizedFallback{"settings.developer.overrides.description", L"Manage local development sources and overrides."},
    LocalizedFallback{"settings.developer.tools", L"Widget tools"},
    LocalizedFallback{"settings.developer.tools.description", L"Inspect and reload widget packages."},
    LocalizedFallback{"settings.debug.runtime", L"Runtime diagnostics"},
    LocalizedFallback{"settings.debug.runtime.description", L"Inspect diagnostic state and traces."},
};

std::wstring DefaultLocalizedString(std::string_view key)
{
    const auto it = std::find_if(
        kFallbackStrings.begin(), kFallbackStrings.end(),
        [key](const LocalizedFallback& entry) { return entry.key == key; });
    return it == kFallbackStrings.end()
        ? std::wstring{}
        : std::wstring(it->value);
}

std::wstring SearchDisplayText(
    const snowdesktop::SettingsSearchResult& result)
{
    if (result.context.empty())
        return result.label;
    return result.label + L" — " + result.context;
}

muxc::InfoBarSeverity ToInfoBarSeverity(SettingsShellInfoSeverity severity)
{
    switch (severity)
    {
    case SettingsShellInfoSeverity::Success:
        return muxc::InfoBarSeverity::Success;
    case SettingsShellInfoSeverity::Warning:
        return muxc::InfoBarSeverity::Warning;
    case SettingsShellInfoSeverity::Error:
        return muxc::InfoBarSeverity::Error;
    case SettingsShellInfoSeverity::Informational:
    default:
        return muxc::InfoBarSeverity::Informational;
    }
}
}

SettingsShell::SettingsShell()
    : ownerThreadId_(GetCurrentThreadId())
{
    InitializeComponent();
    generalPage_ =
        std::make_unique<snowdesktop::winui::GeneralPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle")).as<mux::Style>());
    personalizationPage_ = std::make_unique<
        snowdesktop::winui::PersonalizationPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle")).as<mux::Style>());
    desktopPage_ =
        std::make_unique<snowdesktop::winui::DesktopPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle")).as<mux::Style>());
    searchItems_ = winrt::single_threaded_observable_vector<
        winrt::Windows::Foundation::IInspectable>();
    SettingsSearchBox().ItemsSource(searchItems_);
    HookEvents();
    RefreshLocalizedText();
    RenderConditionalPages();
    RenderRoute();
}

SettingsShell::~SettingsShell()
{
    Close();
}

void SettingsShell::Close() noexcept
{
    if (closed_)
        return;
    closed_ = true;
    try
    {
        UnhookEvents();
        if (generalPage_)
        {
            generalPage_->Deactivate();
            generalPage_->Close();
        }
        if (personalizationPage_)
        {
            personalizationPage_->Deactivate();
            personalizationPage_->Close();
        }
        if (desktopPage_)
        {
            desktopPage_->Deactivate();
            desktopPage_->Close();
        }
        if (activeDialog_)
            activeDialog_.Hide();
    }
    catch (...)
    {
    }
    activeDialog_ = nullptr;
    routeRequested_ = {};
    searchRequested_ = {};
    cancelOperation_ = {};
    generalPage_.reset();
    personalizationPage_.reset();
    desktopPage_.reset();
    localizer_ = {};
    searchResults_.clear();
    breadcrumbRoutes_.clear();
    focusTargets_.clear();
}

void SettingsShell::SetLocalizer(LocalizeCallback localizer)
{
    localizer_ = std::move(localizer);
    RefreshLocalizedText();
}

void SettingsShell::RefreshLocalizedText()
{
    if (closed_)
        return;

    NavigationRoot().PaneTitle(Localize("settings.shell.title"));
    HomeItem().Content(winrt::box_value(Localize("settings.nav.home")));
    GeneralItem().Content(winrt::box_value(Localize("settings.nav.general")));
    PersonalizationItem().Content(
        winrt::box_value(Localize("settings.nav.personalization")));
    DesktopItem().Content(winrt::box_value(Localize("settings.nav.desktop")));
    DockItem().Content(winrt::box_value(Localize("settings.nav.dock")));
    WidgetsItem().Content(winrt::box_value(Localize("settings.nav.widgets")));
    BackupItem().Content(winrt::box_value(Localize("settings.nav.backup")));
    AboutItem().Content(winrt::box_value(Localize("settings.nav.about")));
    DeveloperItem().Content(
        winrt::box_value(Localize("settings.nav.developer")));
    DebugItem().Content(winrt::box_value(Localize("settings.nav.debug")));
    SettingsSearchBox().PlaceholderText(
        Localize("settings.search.placeholder"));
    CancelOperationButton().Content(
        winrt::box_value(Localize("settings.progress.cancel")));

    muxa::AutomationProperties::SetName(
        NavigationRoot(), Localize("settings.shell.title"));
    muxa::AutomationProperties::SetName(
        SettingsSearchBox(), Localize("settings.search.placeholder"));
    muxa::AutomationProperties::SetName(
        ClearSearchButton(), Localize("settings.search.clear"));
    muxc::ToolTipService::SetToolTip(
        ClearSearchButton(),
        winrt::box_value(Localize("settings.search.clear")));

    if (generalPage_)
        generalPage_->RefreshLocalizedText();
    if (personalizationPage_)
        personalizationPage_->RefreshLocalizedText();
    if (desktopPage_)
        desktopPage_->RefreshLocalizedText();
    RenderRoute(true, false);
    if (!searchResults_.empty())
    {
        auto results = searchResults_;
        (void)SetSearchResults(
            std::move(results), navigation_.Generation(), searchRequestId_);
    }
}

void SettingsShell::SetRouteRequestedCallback(
    RouteRequestedCallback callback)
{
    routeRequested_ = std::move(callback);
}

void SettingsShell::SetSearchRequestedCallback(
    SearchRequestedCallback callback)
{
    searchRequested_ = std::move(callback);
}

void SettingsShell::SetCancelOperationCallback(
    CancelOperationCallback callback)
{
    cancelOperation_ = std::move(callback);
}

void SettingsShell::SetGeneralPageActions(
    snowdesktop::winui::GeneralPageActions actions)
{
    if (generalPage_)
        generalPage_->SetActions(std::move(actions));
}

void SettingsShell::SetPersonalizationPageActions(
    snowdesktop::winui::PersonalizationPageActions actions)
{
    if (personalizationPage_)
        personalizationPage_->SetActions(std::move(actions));
}

void SettingsShell::SetDesktopPageActions(
    snowdesktop::winui::DesktopPageActions actions)
{
    if (desktopPage_)
        desktopPage_->SetActions(std::move(actions));
}

bool SettingsShell::IsHotkeyCaptureActive() const noexcept
{
    return generalPage_ && generalPage_->IsHotkeyCaptureActive();
}

void SettingsShell::CaptureRegisteredHotkey(
    UINT modifiers,
    UINT virtualKey)
{
    if (generalPage_)
        generalPage_->CaptureRegisteredHotkey(modifiers, virtualKey);
}

bool SettingsShell::ApplySnapshot(
    const snowdesktop::SettingsSnapshot& snapshot) noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return false;
    try
    {
        const SettingsRoute previousRoute = navigation_.Route();
        const std::uint64_t previousGeneration = navigation_.Generation();
        if (!navigation_.ApplyControllerUpdate(
                snapshot.route, snapshot.revision, snapshot.generation))
        {
            return false;
        }
        if (generalPage_)
            generalPage_->ApplySnapshot(snapshot);
        if (personalizationPage_)
            personalizationPage_->ApplySnapshot(snapshot);
        if (desktopPage_)
            desktopPage_->ApplySnapshot(snapshot);
        const bool routeChanged = previousRoute != navigation_.Route();
        const bool generationChanged =
            previousGeneration != navigation_.Generation();
        if (routeChanged || generationChanged || !renderedPageRoute_)
            RenderRoute(false, true);
        RenderControllerStatus(snapshot);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SettingsShell::Navigate(
    const SettingsRoute& route,
    bool notifyHost) noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return false;
    try
    {
        const bool changed = navigation_.Navigate(route);
        if (changed)
            RenderRoute();
        else if (navigation_.IsRouteAvailable(route) &&
                 navigation_.Route() == route)
            ScheduleFocus();
        else
            return false;

        if (notifyHost && routeRequested_)
            routeRequested_(route);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

SettingsRoute SettingsShell::CurrentRoute() const
{
    return navigation_.Route();
}

std::uint64_t SettingsShell::CurrentGeneration() const noexcept
{
    return navigation_.Generation();
}

std::uint64_t SettingsShell::CurrentRevision() const noexcept
{
    return navigation_.Revision();
}

void SettingsShell::SetConditionalPagesVisible(
    bool developerToolsVisible,
    bool debugVisible)
{
    const auto replacement = navigation_.SetVisibility(
        {developerToolsVisible, debugVisible});
    RenderConditionalPages();
    if (replacement)
    {
        RenderRoute();
        if (routeRequested_)
            routeRequested_(*replacement);
    }
}

bool SettingsShell::SetSearchResults(
    std::vector<snowdesktop::SettingsSearchResult> results,
    std::uint64_t generation,
    std::uint64_t requestId) noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId() ||
        generation != navigation_.Generation() ||
        requestId != searchRequestId_)
    {
        return false;
    }

    try
    {
        updatingSearch_ = true;
        searchResults_ = std::move(results);
        searchItems_.Clear();
        for (const auto& result : searchResults_)
            searchItems_.Append(winrt::box_value(SearchDisplayText(result)));
        SettingsSearchBox().IsSuggestionListOpen(
            !searchResults_.empty() && !SettingsSearchBox().Text().empty());
        updatingSearch_ = false;
        return true;
    }
    catch (...)
    {
        updatingSearch_ = false;
        return false;
    }
}

void SettingsShell::ClearSearch()
{
    if (closed_)
        return;
    updatingSearch_ = true;
    ++searchRequestId_;
    searchResults_.clear();
    if (searchItems_)
        searchItems_.Clear();
    SettingsSearchBox().Text(L"");
    SettingsSearchBox().IsSuggestionListOpen(false);
    ClearSearchButton().Visibility(mux::Visibility::Collapsed);
    updatingSearch_ = false;
}

std::uint64_t SettingsShell::CurrentSearchRequestId() const noexcept
{
    return searchRequestId_;
}

void SettingsShell::RegisterFocusTarget(
    std::string focusId,
    const mux::FrameworkElement& element)
{
    if (focusId.empty() || !element)
        return;
    focusTargets_.insert_or_assign(
        std::move(focusId), winrt::make_weak(element));
}

void SettingsShell::UnregisterFocusTarget(std::string_view focusId)
{
    focusTargets_.erase(std::string(focusId));
}

bool SettingsShell::ShowInfoForGeneration(
    std::uint64_t generation,
    SettingsShellInfoSeverity severity,
    std::wstring title,
    std::wstring message,
    bool closable) noexcept
{
    if (closed_ || generation != navigation_.Generation())
        return false;
    try
    {
        StatusInfoBar().Severity(ToInfoBarSeverity(severity));
        StatusInfoBar().Title(std::move(title));
        StatusInfoBar().Message(std::move(message));
        StatusInfoBar().IsClosable(closable);
        StatusInfoBar().IsOpen(true);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void SettingsShell::ClearInfo() noexcept
{
    try
    {
        StatusInfoBar().IsOpen(false);
    }
    catch (...)
    {
    }
}

bool SettingsShell::ShowProgress(
    const SettingsShellProgress& progress) noexcept
{
    if (closed_ || progress.generation != navigation_.Generation())
        return false;
    try
    {
        progressGeneration_ = progress.generation;
        OperationProgressTitle().Text(progress.title);
        OperationProgressMessage().Text(progress.message);
        OperationProgressRing().IsIndeterminate(progress.indeterminate);
        OperationProgressRing().Value(
            std::clamp(progress.value, 0.0, 100.0));
        OperationProgressRing().IsActive(true);
        CancelOperationButton().Visibility(
            progress.cancellable
                ? mux::Visibility::Visible
                : mux::Visibility::Collapsed);
        CancelOperationButton().IsEnabled(progress.cancellable);
        ProgressHost().Visibility(mux::Visibility::Visible);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void SettingsShell::HideProgress(std::uint64_t generation) noexcept
{
    if (generation != progressGeneration_)
        return;
    try
    {
        OperationProgressRing().IsActive(false);
        ProgressHost().Visibility(mux::Visibility::Collapsed);
    }
    catch (...)
    {
    }
}

void SettingsShell::ShowConfirmation(
    SettingsShellDialogRequest request,
    DialogCompletedCallback completed)
{
    if (closed_ || request.generation != navigation_.Generation())
    {
        if (completed)
            completed(false);
        return;
    }
    ShowConfirmationAsync(std::move(request), std::move(completed));
}

void SettingsShell::HookEvents()
{
    selectionChangedToken_ = NavigationRoot().SelectionChanged(
        [this](const muxc::NavigationView&,
               const muxc::NavigationViewSelectionChangedEventArgs& args) {
            if (updatingNavigation_ || closed_)
                return;
            const auto item =
                args.SelectedItem().try_as<muxc::NavigationViewItem>();
            if (!item)
                return;
            for (const SettingsPage page : {
                     SettingsPage::Home, SettingsPage::General,
                     SettingsPage::Personalization, SettingsPage::Desktop,
                     SettingsPage::DockAndTaskbar, SettingsPage::Widgets,
                     SettingsPage::BackupAndData, SettingsPage::About,
                     SettingsPage::DeveloperTools, SettingsPage::Debug})
            {
                if (NavigationItemForPage(page) == item)
                {
                    RequestRoute(SettingsRoute::ForPage(page));
                    return;
                }
            }
        });

    backRequestedToken_ = NavigationRoot().BackRequested(
        [this](const muxc::NavigationView&,
               const muxc::NavigationViewBackRequestedEventArgs&) {
            if (const auto route = navigation_.GoBack())
            {
                RenderRoute();
                if (routeRequested_)
                    routeRequested_(*route);
            }
        });

    breadcrumbClickedToken_ = PageBreadcrumb().ItemClicked(
        [this](const muxc::BreadcrumbBar&,
               const muxc::BreadcrumbBarItemClickedEventArgs& args) {
            const auto index = static_cast<std::size_t>(args.Index());
            if (index < breadcrumbRoutes_.size() &&
                index + 1 < breadcrumbRoutes_.size())
            {
                RequestRoute(breadcrumbRoutes_[index]);
            }
        });

    searchTextChangedToken_ = SettingsSearchBox().TextChanged(
        [this](const muxc::AutoSuggestBox& sender,
               const muxc::AutoSuggestBoxTextChangedEventArgs& args) {
            if (closed_ || updatingSearch_ ||
                args.Reason() != muxc::AutoSuggestionBoxTextChangeReason::UserInput)
            {
                return;
            }
            ++searchRequestId_;
            searchResults_.clear();
            searchItems_.Clear();
            const std::wstring query = sender.Text().c_str();
            ClearSearchButton().Visibility(
                query.empty() ? mux::Visibility::Collapsed
                              : mux::Visibility::Visible);
            if (searchRequested_)
            {
                searchRequested_(
                    query, navigation_.Generation(), searchRequestId_);
            }
        });

    searchQuerySubmittedToken_ = SettingsSearchBox().QuerySubmitted(
        [this](const muxc::AutoSuggestBox&,
               const muxc::AutoSuggestBoxQuerySubmittedEventArgs& args) {
            if (TrySelectSearchResult(args.ChosenSuggestion()))
                return;
            if (!searchResults_.empty())
                RequestRoute(searchResults_.front().route);
        });

    searchSuggestionChosenToken_ = SettingsSearchBox().SuggestionChosen(
        [this](const muxc::AutoSuggestBox&,
               const muxc::AutoSuggestBoxSuggestionChosenEventArgs& args) {
            (void)TrySelectSearchResult(args.SelectedItem());
        });

    clearSearchToken_ = ClearSearchButton().Click(
        [this](const winrt::Windows::Foundation::IInspectable&,
               const mux::RoutedEventArgs&) { ClearSearch(); });
    cancelOperationToken_ = CancelOperationButton().Click(
        [this](const winrt::Windows::Foundation::IInspectable&,
               const mux::RoutedEventArgs&) {
            CancelOperationButton().IsEnabled(false);
            if (cancelOperation_)
                cancelOperation_(progressGeneration_);
        });
}

void SettingsShell::UnhookEvents() noexcept
{
    try
    {
        if (selectionChangedToken_.value)
            NavigationRoot().SelectionChanged(selectionChangedToken_);
        if (backRequestedToken_.value)
            NavigationRoot().BackRequested(backRequestedToken_);
        if (breadcrumbClickedToken_.value)
            PageBreadcrumb().ItemClicked(breadcrumbClickedToken_);
        if (searchTextChangedToken_.value)
            SettingsSearchBox().TextChanged(searchTextChangedToken_);
        if (searchQuerySubmittedToken_.value)
            SettingsSearchBox().QuerySubmitted(searchQuerySubmittedToken_);
        if (searchSuggestionChosenToken_.value)
            SettingsSearchBox().SuggestionChosen(searchSuggestionChosenToken_);
        if (clearSearchToken_.value)
            ClearSearchButton().Click(clearSearchToken_);
        if (cancelOperationToken_.value)
            CancelOperationButton().Click(cancelOperationToken_);
    }
    catch (...)
    {
    }
    selectionChangedToken_ = {};
    backRequestedToken_ = {};
    breadcrumbClickedToken_ = {};
    searchTextChangedToken_ = {};
    searchQuerySubmittedToken_ = {};
    searchSuggestionChosenToken_ = {};
    clearSearchToken_ = {};
    cancelOperationToken_ = {};
}

void SettingsShell::RenderRoute(
    bool forcePageCards,
    bool scheduleFocus)
{
    RenderNavigationSelection();
    RenderBreadcrumb();
    const auto route = navigation_.Route();
    std::wstring title = PageTitleText(route.page);
    if (route.page == SettingsPage::WidgetSettings &&
        !route.widgetInstanceId.empty())
    {
        title += L" · ";
        title += route.widgetInstanceId;
    }
    PageTitle().Text(title);
    PageSubtitle().Text(PageDescriptionText(route.page));
    muxa::AutomationProperties::SetName(PageTitle(), title);
    RenderPageCards(forcePageCards);
    NavigationRoot().IsBackEnabled(navigation_.CanGoBack());
    if (scheduleFocus)
        ScheduleFocus();
}

void SettingsShell::RenderNavigationSelection()
{
    updatingNavigation_ = true;
    SettingsPage selectedPage = navigation_.Route().page;
    if (selectedPage == SettingsPage::WidgetSettings)
        selectedPage = SettingsPage::Widgets;
    NavigationRoot().SelectedItem(NavigationItemForPage(selectedPage));
    updatingNavigation_ = false;
}

void SettingsShell::RenderBreadcrumb()
{
    breadcrumbRoutes_.clear();
    auto items = winrt::single_threaded_observable_vector<
        winrt::Windows::Foundation::IInspectable>();
    breadcrumbRoutes_.push_back(SettingsRoute::ForPage(SettingsPage::Home));
    items.Append(winrt::box_value(Localize("settings.nav.home")));

    const auto route = navigation_.Route();
    if (route.page == SettingsPage::WidgetSettings)
    {
        breadcrumbRoutes_.push_back(
            SettingsRoute::ForPage(SettingsPage::Widgets));
        items.Append(winrt::box_value(Localize("settings.nav.widgets")));
        breadcrumbRoutes_.push_back(route);
        items.Append(winrt::box_value(PageTitleText(SettingsPage::WidgetSettings)));
    }
    else if (route.page != SettingsPage::Home)
    {
        breadcrumbRoutes_.push_back(route);
        items.Append(winrt::box_value(PageTitleText(route.page)));
    }
    PageBreadcrumb().ItemsSource(items);
}

void SettingsShell::RenderPageCards(bool forcePageCards)
{
    SettingsRoute pageRoute = navigation_.Route();
    pageRoute.focusId.clear();
    if (!forcePageCards && renderedPageRoute_ &&
        *renderedPageRoute_ == pageRoute)
    {
        return;
    }

    const bool leavingGeneral = renderedPageRoute_ &&
        renderedPageRoute_->page == SettingsPage::General &&
        pageRoute.page != SettingsPage::General;
    if (leavingGeneral && generalPage_)
        generalPage_->Deactivate();
    const bool leavingPersonalization = renderedPageRoute_ &&
        renderedPageRoute_->page == SettingsPage::Personalization &&
        pageRoute.page != SettingsPage::Personalization;
    if (leavingPersonalization && personalizationPage_)
        personalizationPage_->Deactivate();
    const bool leavingDesktop = renderedPageRoute_ &&
        renderedPageRoute_->page == SettingsPage::Desktop &&
        pageRoute.page != SettingsPage::Desktop;
    if (leavingDesktop && desktopPage_)
        desktopPage_->Deactivate();

    PageCards().Children().Clear();
    focusTargets_.clear();

    const auto addPlaceholder = [this](
                                    std::string focusId,
                                    std::string_view title,
                                    std::string_view description) {
        PageCards().Children().Append(CreatePlaceholderCard(
            std::move(focusId), title, description));
    };
    const auto addNavigation = [this](
                                   std::string focusId,
                                   std::string_view title,
                                   std::string_view description,
                                   SettingsPage page) {
        PageCards().Children().Append(CreateNavigationCard(
            std::move(focusId), title, description,
            SettingsRoute::ForPage(page)));
    };

    switch (navigation_.Route().page)
    {
    case SettingsPage::Home:
        addNavigation("home.theme", "settings.home.theme",
            "settings.home.theme.description", SettingsPage::Personalization);
        addNavigation("home.dock", "settings.home.dock",
            "settings.home.dock.description", SettingsPage::DockAndTaskbar);
        addNavigation("home.widgets", "settings.home.widgets",
            "settings.home.widgets.description", SettingsPage::Widgets);
        addNavigation("home.backup", "settings.home.backup",
            "settings.home.backup.description", SettingsPage::BackupAndData);
        break;
    case SettingsPage::General:
        if (generalPage_)
        {
            PageCards().Children().Append(generalPage_->Root());
            generalPage_->RegisterFocusTargets(
                [this](std::string focusId,
                       const mux::FrameworkElement& element) {
                    RegisterFocusTarget(std::move(focusId), element);
                });
            generalPage_->Activate();
        }
        break;
    case SettingsPage::Personalization:
        if (personalizationPage_)
        {
            PageCards().Children().Append(personalizationPage_->Content());
            for (const std::string_view focusId : {
                     "personalization.theme",
                     "personalization.globalTheme",
                     "personalization.backgroundColor",
                     "personalization.borderColor",
                     "personalization.widgetAlpha",
                     "personalization.borderAlpha",
                     "personalization.gradientEndAlpha",
                     "personalization.glass",
                     "personalization.blurRadius",
                     "personalization.acrylic",
                     "personalization.contentTheme",
                     "personalization.contextMenu",
                     "personalization.cornerRadius",
                     "personalization.barHeight",
                     "personalization.tabHeight",
                     "personalization.showCategoryTabCounts"})
            {
                RegisterFocusTarget(
                    std::string(focusId),
                    personalizationPage_->FocusTarget(focusId));
            }
            personalizationPage_->Activate();
        }
        break;
    case SettingsPage::Desktop:
        if (desktopPage_)
        {
            PageCards().Children().Append(desktopPage_->Content());
            for (const std::string_view focusId : {
                     "desktop.spacing",
                     "desktop.iconSize",
                     "desktop.itemFontSize",
                     "desktop.listFontSize",
                     "desktop.fontWeight",
                     "desktop.shortcutArrow",
                     "desktop.categoryLayout",
                     "desktop.tabFontSize",
                     "desktop.categoryCounts",
                     "desktop.iconBeautify",
                     "desktop.categories",
                     "desktop.category.add"})
            {
                RegisterFocusTarget(
                    std::string(focusId),
                    desktopPage_->FocusTarget(focusId));
            }
            desktopPage_->Activate();
        }
        break;
    case SettingsPage::DockAndTaskbar:
        addPlaceholder("dock.dock", "settings.dock.dock",
            "settings.dock.dock.description");
        addPlaceholder("dock.items", "settings.dock.items",
            "settings.dock.items.description");
        addPlaceholder("dock.taskbar", "settings.dock.taskbar",
            "settings.dock.taskbar.description");
        break;
    case SettingsPage::Widgets:
        addPlaceholder("widgets.installed", "settings.widgets.installed",
            "settings.widgets.installed.description");
        addPlaceholder("widgets.sources", "settings.widgets.sources",
            "settings.widgets.sources.description");
        addPlaceholder("widgets.permissions", "settings.widgets.permissions",
            "settings.widgets.permissions.description");
        break;
    case SettingsPage::WidgetSettings:
        addPlaceholder("widget.fields", "settings.widget.fields",
            "settings.widget.fields.description");
        break;
    case SettingsPage::BackupAndData:
        addPlaceholder("backup.layout", "settings.backup.layout",
            "settings.backup.layout.description");
        addPlaceholder("backup.full", "settings.backup.full",
            "settings.backup.full.description");
        addPlaceholder("backup.directory", "settings.backup.directory",
            "settings.backup.directory.description");
        break;
    case SettingsPage::About:
        addPlaceholder("about.version", "settings.about.version",
            "settings.about.version.description");
        addPlaceholder("about.project", "settings.about.project",
            "settings.about.project.description");
        addPlaceholder("about.thirdparty", "settings.about.thirdparty",
            "settings.about.thirdparty.description");
        break;
    case SettingsPage::DeveloperTools:
        addPlaceholder("developer.overrides", "settings.developer.overrides",
            "settings.developer.overrides.description");
        addPlaceholder("developer.tools", "settings.developer.tools",
            "settings.developer.tools.description");
        break;
    case SettingsPage::Debug:
        addPlaceholder("debug.runtime", "settings.debug.runtime",
            "settings.debug.runtime.description");
        break;
    default:
        break;
    }
    renderedPageRoute_ = std::move(pageRoute);
}

void SettingsShell::RenderConditionalPages()
{
    DeveloperItem().Visibility(
        navigation_.Visibility().developerTools
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
    DebugItem().Visibility(
        navigation_.Visibility().debug
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
}

void SettingsShell::RenderControllerStatus(
    const snowdesktop::SettingsSnapshot& snapshot)
{
    if (snapshot.lastActionMessage.empty())
        return;
    StatusInfoBar().Severity(
        snapshot.retryRequired ? muxc::InfoBarSeverity::Warning
                               : muxc::InfoBarSeverity::Informational);
    StatusInfoBar().Title(L"");
    StatusInfoBar().Message(snapshot.lastActionMessage);
    StatusInfoBar().IsClosable(true);
    StatusInfoBar().IsOpen(true);
}

void SettingsShell::ScheduleFocus()
{
    if (closed_)
        return;
    const std::uint64_t generation = navigation_.Generation();
    const SettingsRoute route = navigation_.Route();
    auto lifetime = get_strong();
    DispatcherQueue().TryEnqueue([lifetime, generation, route]() {
        if (!lifetime->closed_ &&
            lifetime->navigation_.Generation() == generation &&
            lifetime->navigation_.Route() == route)
        {
            lifetime->FocusPendingTarget();
        }
    });
}

void SettingsShell::FocusPendingTarget()
{
    const auto& focusId = navigation_.Route().focusId;
    if (!focusId.empty())
    {
        const auto it = focusTargets_.find(focusId);
        if (it != focusTargets_.end())
        {
            if (auto target = it->second.get())
            {
                target.StartBringIntoView();
                (void)target.Focus(mux::FocusState::Keyboard);
                return;
            }
        }
    }
    (void)PageScrollViewer().Focus(mux::FocusState::Programmatic);
}

void SettingsShell::RequestRoute(const SettingsRoute& route)
{
    (void)Navigate(route, true);
}

std::wstring SettingsShell::Localize(std::string_view key) const
{
    if (localizer_)
    {
        std::wstring localized = localizer_(key);
        if (!localized.empty())
            return localized;
    }
    return DefaultLocalizedString(key);
}

std::wstring SettingsShell::PageTitleText(SettingsPage page) const
{
    switch (page)
    {
    case SettingsPage::Home: return Localize("settings.nav.home");
    case SettingsPage::General: return Localize("settings.nav.general");
    case SettingsPage::Personalization:
        return Localize("settings.nav.personalization");
    case SettingsPage::Desktop: return Localize("settings.nav.desktop");
    case SettingsPage::DockAndTaskbar: return Localize("settings.nav.dock");
    case SettingsPage::Widgets: return Localize("settings.nav.widgets");
    case SettingsPage::WidgetSettings: return Localize("settings.nav.widgets");
    case SettingsPage::BackupAndData: return Localize("settings.nav.backup");
    case SettingsPage::About: return Localize("settings.nav.about");
    case SettingsPage::DeveloperTools:
        return Localize("settings.nav.developer");
    case SettingsPage::Debug: return Localize("settings.nav.debug");
    default: return {};
    }
}

std::wstring SettingsShell::PageDescriptionText(SettingsPage page) const
{
    switch (page)
    {
    case SettingsPage::Home: return Localize("settings.page.home.description");
    case SettingsPage::General:
        return Localize("settings.page.general.description");
    case SettingsPage::Personalization:
        return Localize("settings.page.personalization.description");
    case SettingsPage::Desktop:
        return Localize("settings.page.desktop.description");
    case SettingsPage::DockAndTaskbar:
        return Localize("settings.page.dock.description");
    case SettingsPage::Widgets:
        return Localize("settings.page.widgets.description");
    case SettingsPage::WidgetSettings:
        return Localize("settings.page.widget.description");
    case SettingsPage::BackupAndData:
        return Localize("settings.page.backup.description");
    case SettingsPage::About:
        return Localize("settings.page.about.description");
    case SettingsPage::DeveloperTools:
        return Localize("settings.page.developer.description");
    case SettingsPage::Debug:
        return Localize("settings.page.debug.description");
    default: return {};
    }
}

muxc::NavigationViewItem SettingsShell::NavigationItemForPage(
    SettingsPage page)
{
    switch (page)
    {
    case SettingsPage::Home: return HomeItem();
    case SettingsPage::General: return GeneralItem();
    case SettingsPage::Personalization: return PersonalizationItem();
    case SettingsPage::Desktop: return DesktopItem();
    case SettingsPage::DockAndTaskbar: return DockItem();
    case SettingsPage::Widgets:
    case SettingsPage::WidgetSettings: return WidgetsItem();
    case SettingsPage::BackupAndData: return BackupItem();
    case SettingsPage::About: return AboutItem();
    case SettingsPage::DeveloperTools: return DeveloperItem();
    case SettingsPage::Debug: return DebugItem();
    default: return nullptr;
    }
}

bool SettingsShell::TrySelectSearchResult(
    const winrt::Windows::Foundation::IInspectable& selectedItem)
{
    if (!selectedItem)
        return false;
    const auto selectedText = winrt::unbox_value_or<hstring>(
        selectedItem, hstring{});
    if (selectedText.empty())
        return false;
    for (const auto& result : searchResults_)
    {
        if (SearchDisplayText(result) == selectedText.c_str())
        {
            RequestRoute(result.route);
            return true;
        }
    }
    return false;
}

muxc::Button SettingsShell::CreateNavigationCard(
    std::string focusId,
    std::string_view titleKey,
    std::string_view descriptionKey,
    const SettingsRoute& route)
{
    muxc::Button card;
    card.Style(Resources().Lookup(
        winrt::box_value(L"SettingsShellCardButtonStyle")).as<mux::Style>());
    card.Tag(winrt::box_value(winrt::to_hstring(focusId)));

    muxc::StackPanel content;
    content.Spacing(3);
    muxc::TextBlock title;
    title.Text(Localize(titleKey));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.TextWrapping(mux::TextWrapping::Wrap);
    muxc::TextBlock description;
    description.Text(Localize(descriptionKey));
    description.Opacity(0.72);
    description.TextWrapping(mux::TextWrapping::Wrap);
    content.Children().Append(title);
    content.Children().Append(description);
    card.Content(content);
    muxa::AutomationProperties::SetName(card, title.Text());
    card.Click([this, route](const winrt::Windows::Foundation::IInspectable&,
                            const mux::RoutedEventArgs&) {
        RequestRoute(route);
    });
    RegisterFocusTarget(std::move(focusId), card);
    return card;
}

muxc::Border SettingsShell::CreatePlaceholderCard(
    std::string focusId,
    std::string_view titleKey,
    std::string_view descriptionKey)
{
    muxc::Border card;
    card.Style(Resources().Lookup(
        winrt::box_value(L"SettingsShellCardStyle")).as<mux::Style>());
    card.Tag(winrt::box_value(winrt::to_hstring(focusId)));
    card.IsTabStop(true);
    card.UseSystemFocusVisuals(true);

    muxc::StackPanel content;
    content.Spacing(3);
    muxc::TextBlock title;
    title.Text(Localize(titleKey));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.TextWrapping(mux::TextWrapping::Wrap);
    muxc::TextBlock description;
    description.Text(Localize(descriptionKey));
    description.Opacity(0.72);
    description.TextWrapping(mux::TextWrapping::Wrap);
    content.Children().Append(title);
    content.Children().Append(description);
    card.Child(content);
    muxa::AutomationProperties::SetName(card, title.Text());
    RegisterFocusTarget(std::move(focusId), card);
    return card;
}

winrt::fire_and_forget SettingsShell::ShowConfirmationAsync(
    SettingsShellDialogRequest request,
    DialogCompletedCallback completed)
{
    auto lifetime = get_strong();
    try
    {
        if (activeDialog_)
            activeDialog_.Hide();
        muxc::ContentDialog dialog;
        activeDialog_ = dialog;
        dialog.XamlRoot(XamlRoot());
        dialog.Title(winrt::box_value(request.title));
        dialog.Content(winrt::box_value(request.message));
        dialog.PrimaryButtonText(request.primaryButtonText);
        dialog.CloseButtonText(request.closeButtonText);
        dialog.DefaultButton(
            request.destructive ? muxc::ContentDialogButton::Close
                                : muxc::ContentDialogButton::Primary);
        const auto result = co_await dialog.ShowAsync();
        if (activeDialog_ == dialog)
            activeDialog_ = nullptr;
        if (!closed_ && request.generation == navigation_.Generation() &&
            completed)
        {
            completed(result == muxc::ContentDialogResult::Primary);
        }
    }
    catch (...)
    {
        activeDialog_ = nullptr;
        if (!closed_ && request.generation == navigation_.Generation() &&
            completed)
        {
            completed(false);
        }
    }
}

} // namespace winrt::SnowDesktop::implementation
