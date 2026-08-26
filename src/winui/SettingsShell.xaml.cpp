#include "pch.h"

#include "SettingsShell.xaml.h"
#if __has_include("SettingsShell.g.cpp")
#include "SettingsShell.g.cpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace winrt::SnowDesktop::implementation
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxmi = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace wfc = winrt::Windows::Foundation::Collections;
namespace wg = winrt::Windows::Graphics;

namespace
{
using snowdesktop::CanonicalizeSettingsRoute;
using snowdesktop::SettingsPage;
using snowdesktop::SettingsRoute;
using snowdesktop::SettingsSearchEntryKind;

[[nodiscard]] muxm::Brush CreateSystemWindowFallbackBrush()
{
    const COLORREF color = GetSysColor(COLOR_WINDOW);
    return muxm::SolidColorBrush{winrt::Windows::UI::Color{
        0xFF, GetRValue(color), GetGValue(color), GetBValue(color)}};
}

[[nodiscard]] bool IsHighContrastEnabled() noexcept
{
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(state), &state, 0) != FALSE &&
        (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

[[nodiscard]] mux::DependencyObject VisualParent(
    const mux::DependencyObject& element) noexcept
{
    if (!element)
        return nullptr;
    try
    {
        return muxm::VisualTreeHelper::GetParent(element);
    }
    catch (...)
    {
        return nullptr;
    }
}

[[nodiscard]] muxc::NumberBox FindNumberBoxAncestor(
    const winrt::Windows::Foundation::IInspectable& element) noexcept
{
    auto current = element.try_as<mux::DependencyObject>();
    while (current)
    {
        if (const auto number = current.try_as<muxc::NumberBox>())
            return number;
        current = VisualParent(current);
    }
    return nullptr;
}

[[nodiscard]] bool IsWithinNumberBox(
    const winrt::Windows::Foundation::IInspectable& element,
    const muxc::NumberBox& number) noexcept
{
    auto current = element.try_as<mux::DependencyObject>();
    while (current)
    {
        if (const auto ancestor = current.try_as<muxc::NumberBox>();
            ancestor && ancestor == number)
        {
            return true;
        }
        current = VisualParent(current);
    }
    return false;
}

[[nodiscard]] muxc::ImageIcon CreateColorNavigationIcon(
    std::wstring_view uri)
{
    muxmi::SvgImageSource source{};
    source.UriSource(winrt::Windows::Foundation::Uri{winrt::hstring{uri}});
    muxc::ImageIcon icon{};
    icon.Source(source);
    icon.Width(22.0);
    icon.Height(22.0);
    return icon;
}

[[nodiscard]] muxc::FontIcon CreateHighContrastNavigationIcon(
    std::wstring_view glyph)
{
    muxc::FontIcon icon{};
    icon.Glyph(std::wstring(glyph));
    icon.FontSize(18.0);
    return icon;
}

struct LocalizedFallback
{
    std::string_view key;
    std::wstring_view value;
};

constexpr std::array kFallbackStrings{
    LocalizedFallback{"settings.shell.title", L"Settings"},
    LocalizedFallback{"settings.shell.back", L"Back"},
    LocalizedFallback{"settings.shell.toggleNavigation", L"Toggle navigation pane"},
    LocalizedFallback{"settings.search.placeholder", L"Find a setting"},
    LocalizedFallback{"settings.search.clear", L"Clear search"},
    LocalizedFallback{"settings.progress.cancel", L"Cancel"},
    LocalizedFallback{"settings.nav.home", L"Home"},
    LocalizedFallback{"settings.nav.general", L"General"},
    LocalizedFallback{"settings.nav.personalization", L"Personalization"},
    LocalizedFallback{"settings.nav.desktop", L"Desktop"},
    LocalizedFallback{"settings.nav.group.desktopShell", L"Desktop & shell"},
    LocalizedFallback{"settings.nav.group.data", L"Data"},
    LocalizedFallback{"settings.nav.categories", L"Categories & rules"},
    LocalizedFallback{"settings.nav.dock", L"Dock"},
    LocalizedFallback{"settings.nav.taskbar", L"Windows taskbar"},
    LocalizedFallback{"settings.nav.widgets", L"Widgets"},
    LocalizedFallback{"settings.nav.backup", L"Backup & data"},
    LocalizedFallback{"settings.nav.about", L"About"},
    LocalizedFallback{"settings.nav.developer", L"Developer tools"},
    LocalizedFallback{"settings.nav.debug", L"Debug"},
    LocalizedFallback{"settings.page.home.description", L"Your most-used SnowDesktop settings and status at a glance."},
    LocalizedFallback{"settings.page.general.description", L"Startup, language, navigation, hotkeys and everyday behavior."},
    LocalizedFallback{"settings.page.personalization.description", L"Theme, widget surfaces, desktop icons and icon beautification."},
    LocalizedFallback{"settings.page.desktop.description", L"Desktop behavior, display selection and pointer interaction."},
    LocalizedFallback{"settings.page.categories.description", L"Manage category label counts and automatic matching rules."},
    LocalizedFallback{"settings.page.dock.description", L"Configure Dock behavior, placement and items."},
    LocalizedFallback{"settings.page.taskbar.description", L"Configure Windows taskbar behavior, default appearance and scenario overrides."},
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
    LocalizedFallback{"settings.personalization.theme.description", L"Set the global theme plus navigation, popup and context-menu appearance."},
    LocalizedFallback{"settings.personalization.colors", L"Colors and gradients"},
    LocalizedFallback{"settings.personalization.colors.description", L"Customize accents, surfaces and gradients."},
    LocalizedFallback{"settings.personalization.menu", L"Context menu"},
    LocalizedFallback{"settings.personalization.menu.description", L"Configure SnowDesktop context-menu appearance."},
    LocalizedFallback{"settings.personalization.widgets", L"Widget appearance"},
    LocalizedFallback{"settings.personalization.widgets.description", L"Set widget surfaces, dimensions, category labels and search controls."},
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

std::wstring FormatSingleValue(
    std::wstring format, std::wstring_view value)
{
    if (const std::size_t marker = format.find(L"%s");
        marker != std::wstring::npos)
    {
        format.replace(marker, 2, value.data(), value.size());
    }
    return format;
}

bool IsWidgetsBackendPage(SettingsPage page) noexcept
{
    return page == SettingsPage::Widgets ||
        page == SettingsPage::DeveloperTools;
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
    solidFallbackBackground_ = ShellRoot().Background();
    if (!solidFallbackBackground_)
        solidFallbackBackground_ = CreateSystemWindowFallbackBrush();
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
    dockPage_ = std::make_unique<snowdesktop::winui::DockPagePresenter>(
        [this](std::string_view key) { return Localize(key); },
        Resources().Lookup(
            winrt::box_value(L"SettingsShellCardStyle")).as<mux::Style>());
    homeAboutPage_ =
        std::make_unique<snowdesktop::winui::HomeAboutPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle"))
                .as<mux::Style>(),
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardButtonStyle"))
                .as<mux::Style>());
    widgetsPage_ =
        std::make_unique<snowdesktop::winui::WidgetsPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle"))
                .as<mux::Style>(),
            Resources().Lookup(
                winrt::box_value(L"SettingsShellFluentGlyphTemplate"))
                .as<mux::DataTemplate>(),
            Resources().Lookup(
                winrt::box_value(L"SettingsShellFontAwesomeGlyphTemplate"))
                .as<mux::DataTemplate>());
    backupDataPage_ =
        std::make_unique<snowdesktop::winui::BackupDataPagePresenter>(
            [this](std::string_view key) { return Localize(key); },
            Resources().Lookup(
                winrt::box_value(L"SettingsShellCardStyle"))
                .as<mux::Style>());
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
        if (dockPage_)
        {
            dockPage_->Deactivate();
            dockPage_->Close();
        }
        if (homeAboutPage_)
        {
            homeAboutPage_->Deactivate();
            homeAboutPage_->Close();
        }
        if (widgetSettingsPage_)
        {
            widgetSettingsPage_->Deactivate();
            widgetSettingsPage_->Close();
        }
        if (widgetSettingsService_)
            widgetSettingsService_->SetEventCallbacks({}, {});
        if (widgetsPage_)
        {
            widgetsPage_->Deactivate();
            widgetsPage_->Close();
        }
        if (backupDataPage_)
        {
            backupDataPage_->Deactivate();
            backupDataPage_->Close();
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
    actualThemeChanged_ = {};
    integratedTitleBarLayoutChanged_ = {};
    generalPage_.reset();
    personalizationPage_.reset();
    desktopPage_.reset();
    dockPage_.reset();
    homeAboutPage_.reset();
    widgetSettingsPage_.reset();
    widgetSettingsService_ = nullptr;
    widgetsPage_.reset();
    backupDataPage_.reset();
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

    HomeItem().Content(winrt::box_value(Localize("settings.nav.home")));
    GeneralItem().Content(winrt::box_value(Localize("app.settings.general")));
    PersonalizationItem().Content(
        winrt::box_value(Localize("app.settings.appearance")));
    AppearanceThemeItem().Content(
        winrt::box_value(Localize("settings.personalization.theme")));
    AppearanceWidgetsItem().Content(
        winrt::box_value(Localize("settings.personalization.widgets")));
    AppearanceDesktopIconsItem().Content(
        winrt::box_value(Localize("app.settings.desktop_icons")));
    AppearanceIconBeautificationItem().Content(
        winrt::box_value(Localize("app.settings.icon_beautify")));
    DesktopShellHeader().Content(
        winrt::box_value(Localize("settings.nav.group.desktopShell")));
    DataHeader().Content(
        winrt::box_value(Localize("settings.nav.group.data")));
    DesktopItem().Content(winrt::box_value(Localize("settings.nav.desktop")));
    CategoriesItem().Content(
        winrt::box_value(Localize("settings.nav.categories")));
    DockItem().Content(winrt::box_value(Localize("settings.nav.dock")));
    TaskbarItem().Content(winrt::box_value(Localize("settings.nav.taskbar")));
    WidgetsItem().Content(winrt::box_value(Localize("app.settings.widgets")));
    BackupItem().Content(winrt::box_value(Localize("app.settings.backup")));
    AboutItem().Content(winrt::box_value(Localize("app.settings.about")));
    DeveloperItem().Content(
        winrt::box_value(Localize("app.settings.widgets_developer_tools")));
    DebugItem().Content(winrt::box_value(Localize("app.settings.debug")));
    SettingsSearchBox().PlaceholderText(
        Localize("settings.search.placeholder"));
    CancelOperationButton().Content(
        winrt::box_value(Localize("settings.progress.cancel")));

    muxa::AutomationProperties::SetName(
        NavigationRoot(), Localize("settings.shell.title"));
    muxa::AutomationProperties::SetName(
        SettingsSearchBox(), Localize("settings.search.placeholder"));
    muxa::AutomationProperties::SetName(
        TitleBarBackButton(), Localize("settings.shell.back"));
    muxa::AutomationProperties::SetName(
        TitleBarPaneToggleButton(), Localize("settings.shell.toggleNavigation"));
    muxc::ToolTipService::SetToolTip(
        TitleBarBackButton(),
        winrt::box_value(Localize("settings.shell.back")));
    muxc::ToolTipService::SetToolTip(
        TitleBarPaneToggleButton(),
        winrt::box_value(Localize("settings.shell.toggleNavigation")));

    ApplyNavigationIcons();

    if (generalPage_)
        generalPage_->RefreshLocalizedText();
    if (personalizationPage_)
        personalizationPage_->RefreshLocalizedText();
    if (desktopPage_)
        desktopPage_->RefreshLocalizedText();
    if (dockPage_)
        dockPage_->RefreshLocalizedText();
    if (homeAboutPage_)
        homeAboutPage_->RefreshLocalizedText();
    if (widgetSettingsPage_)
        widgetSettingsPage_->RefreshLocalizedText();
    if (widgetsPage_)
        widgetsPage_->RefreshLocalizedText();
    if (backupDataPage_)
        backupDataPage_->RefreshLocalizedText();
    RenderRoute(true, false);
    if (!searchResults_.empty())
    {
        auto results = searchResults_;
        (void)SetSearchResults(
            std::move(results), navigation_.Generation(), searchRequestId_);
    }
}

void SettingsShell::SetSystemBackdropActive(bool active) noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return;

    try
    {
        if (active)
        {
            // The DesktopWindowXamlSource owns the material. Any opaque root
            // brush would cover it completely.
            ShellRoot().Background(muxm::Brush{nullptr});
            return;
        }

        // Re-resolve the theme resource when possible so Windows 10 and high
        // contrast changes get the current solid color rather than a stale
        // brush cached at shell construction.
        muxm::Brush fallback = solidFallbackBackground_;
        if (const auto application = mux::Application::Current())
        {
            if (const auto resource = application.Resources().TryLookup(
                    winrt::box_value(
                        L"ApplicationPageBackgroundThemeBrush")))
            {
                if (const auto brush = resource.try_as<muxm::Brush>())
                    fallback = brush;
            }
        }
        if (!fallback)
            fallback = CreateSystemWindowFallbackBrush();
        ShellRoot().Background(fallback);
    }
    catch (...)
    {
        // Backdrop decoration must never make the settings session unusable.
        try
        {
            ShellRoot().Background(solidFallbackBackground_);
        }
        catch (...)
        {
        }
    }
}

void SettingsShell::SetActualThemeChangedCallback(
    ActualThemeChangedCallback callback)
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return;

    actualThemeChanged_ = std::move(callback);
    NotifyActualThemeChanged();
}

void SettingsShell::SetIntegratedTitleBarInsets(
    double leftInset, double rightInset)
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return;

    leftInset = std::max(0.0, leftInset);
    rightInset = std::max(0.0, rightInset);
    TitleBarLeftInsetColumn().Width(
        mux::GridLength{leftInset, mux::GridUnitType::Pixel});
    TitleBarRightInsetColumn().Width(
        mux::GridLength{rightInset, mux::GridUnitType::Pixel});
}

std::vector<wg::RectInt32>
SettingsShell::IntegratedTitleBarDragRectangles()
{
    std::vector<wg::RectInt32> rectangles;
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return rectangles;

    try
    {
        const mux::FrameworkElement host = IntegratedTitleBarHost();
        const mux::XamlRoot xamlRoot = host.XamlRoot();
        if (!xamlRoot || host.ActualWidth() <= 0.0 ||
            host.ActualHeight() <= 0.0)
        {
            return rectangles;
        }

        const double scale = xamlRoot.RasterizationScale();
        if (scale <= 0.0)
            return rectangles;

        const mux::FrameworkElement dragRegion = TitleBarDragRegion();
        if (dragRegion.ActualWidth() <= 0.0 ||
            dragRegion.ActualHeight() <= 0.0)
        {
            return rectangles;
        }
        const auto transform = dragRegion.TransformToVisual(host);
        const auto origin = transform.TransformPoint(
            winrt::Windows::Foundation::Point{0.0f, 0.0f});
        const double titleLeft = std::max(0.0,
            static_cast<double>(origin.X));
        const double titleRight = std::min(host.ActualWidth(),
            titleLeft + dragRegion.ActualWidth());
        const int pixelHeight = std::max(1,
            static_cast<int>(std::floor(host.ActualHeight() * scale)));
        const int pixelLeft =
            static_cast<int>(std::ceil(titleLeft * scale));
        const int pixelRight =
            static_cast<int>(std::floor(titleRight * scale));
        if (pixelRight > pixelLeft)
        {
            rectangles.push_back(wg::RectInt32{pixelLeft, 0,
                pixelRight - pixelLeft, pixelHeight});
        }
    }
    catch (...)
    {
        rectangles.clear();
    }
    return rectangles;
}

void SettingsShell::SetIntegratedTitleBarLayoutChangedCallback(
    IntegratedTitleBarLayoutChangedCallback callback)
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return;
    integratedTitleBarLayoutChanged_ = std::move(callback);
    NotifyIntegratedTitleBarLayoutChanged();
}

void SettingsShell::NotifyActualThemeChanged() noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId())
        return;

    const ActualThemeChangedCallback callback = actualThemeChanged_;
    if (!callback)
        return;

    try
    {
        callback(ShellRoot().ActualTheme() == mux::ElementTheme::Dark);
    }
    catch (...)
    {
        // Theme notification is presentation-only and must not unwind through
        // the XAML event dispatcher or window initialization.
    }
}

void SettingsShell::NotifyIntegratedTitleBarLayoutChanged() noexcept
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId() ||
        !integratedTitleBarLayoutChanged_)
    {
        return;
    }
    try
    {
        integratedTitleBarLayoutChanged_();
    }
    catch (...)
    {
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

void SettingsShell::SetDockPageActions(
    snowdesktop::winui::DockPageActions actions)
{
    if (dockPage_)
        dockPage_->SetActions(std::move(actions));
}

void SettingsShell::SetHomeAboutPageActions(
    snowdesktop::winui::HomeAboutPageActions actions)
{
    if (homeAboutPage_)
        homeAboutPage_->SetActions(std::move(actions));
}

bool SettingsShell::ApplyHomeAboutStatusPatch(
    const snowdesktop::winui::HomeAboutStatusPatch& patch)
{
    return homeAboutPage_ && homeAboutPage_->ApplyStatusPatch(patch);
}

void SettingsShell::SetWidgetSettingsService(
    snowdesktop::widget_runtime::WidgetSettingsService* service) noexcept
{
    if (closed_ ||
        (widgetSettingsService_ == service && widgetSettingsPage_))
        return;
    try
    {
        if (widgetSettingsService_)
            widgetSettingsService_->SetEventCallbacks({}, {});
        if (widgetSettingsPage_)
        {
            widgetSettingsPage_->Deactivate();
            widgetSettingsPage_->Close();
            widgetSettingsPage_.reset();
        }

        widgetSettingsService_ = service;
        if (!widgetSettingsService_)
            return;

        widgetSettingsPage_ = std::make_unique<
            snowdesktop::winui::WidgetSettingsPresenter>(
                *widgetSettingsService_,
                [this](std::string_view key) { return Localize(key); },
                Resources().Lookup(
                    winrt::box_value(L"SettingsShellCardStyle"))
                    .as<mux::Style>());

        snowdesktop::winui::WidgetSettingsPresenterCallbacks callbacks;
        callbacks.mutationCompleted =
            [this](std::string,
                   snowdesktop::widget_runtime::WidgetSettingMutationResult
                       result) {
                if (closed_ || result.Succeeded() ||
                    result.status == snowdesktop::widget_runtime::
                        WidgetSettingMutationStatus::InvalidValue)
                    return;
                std::wstring message;
                if (!result.message.empty())
                    message = winrt::to_hstring(result.message).c_str();
                if (message.empty())
                    message = Localize("settings.widget.saveFailed");
                (void)ShowInfoForGeneration(
                    navigation_.Generation(),
                    SettingsShellInfoSeverity::Error,
                    Localize("settings.status.error"),
                    std::move(message));
            };
        callbacks.diagnostic =
            [](std::wstring widgetId, std::string settingKey,
               std::string diagnosticCode) {
                std::wstring message = L"SnowDesktop widget settings: ";
                message += widgetId;
                message += L" / ";
                message += winrt::to_hstring(settingKey).c_str();
                message += L" / ";
                message += winrt::to_hstring(diagnosticCode).c_str();
                message += L"\n";
                OutputDebugStringW(message.c_str());
            };
        widgetSettingsPage_->SetCallbacks(std::move(callbacks));
        auto dispatchers = widgetSettingsPage_->EventDispatchers();
        widgetSettingsService_->SetEventCallbacks(
            std::move(dispatchers.snapshotChanged),
            std::move(dispatchers.searchCompleted));

        const auto& route = navigation_.Route();
        if (route.page == SettingsPage::WidgetSettings)
        {
            if (const auto snapshot = widgetSettingsService_->Snapshot(
                    route.widgetInstanceId))
            {
                (void)ApplyWidgetSettingsSnapshot(*snapshot);
            }
        }
    }
    catch (...)
    {
        try
        {
            if (widgetSettingsService_)
                widgetSettingsService_->SetEventCallbacks({}, {});
        }
        catch (...)
        {
        }
        widgetSettingsPage_.reset();
        widgetSettingsService_ = nullptr;
    }
}

bool SettingsShell::ApplyWidgetSettingsSnapshot(
    const snowdesktop::widget_runtime::WidgetSettingsSnapshot& snapshot)
{
    if (closed_ || ownerThreadId_ != GetCurrentThreadId() ||
        !widgetSettingsPage_ ||
        navigation_.Route().page != SettingsPage::WidgetSettings ||
        navigation_.Route().widgetInstanceId != snapshot.widgetId ||
        !widgetSettingsPage_->ApplySnapshot(snapshot))
    {
        return false;
    }

    renderedPageRoute_.reset();
    RenderPageCards(true);
    ScheduleFocus();
    return true;
}

void SettingsShell::SetWidgetsPageActions(
    snowdesktop::winui::WidgetsPageActions actions)
{
    if (widgetsPage_)
        widgetsPage_->SetActions(std::move(actions));
}

bool SettingsShell::ApplyWidgetsPageSnapshot(
    const snowdesktop::winui::WidgetsPageSnapshot& snapshot)
{
    return !closed_ && ownerThreadId_ == GetCurrentThreadId() &&
        widgetsPage_ &&
        IsWidgetsBackendPage(navigation_.Route().page) &&
        navigation_.Generation() == snapshot.generation &&
        widgetsPage_->ApplySnapshot(snapshot);
}

void SettingsShell::SetBackupDataPageActions(
    snowdesktop::winui::BackupDataPageActions actions)
{
    if (backupDataPage_)
        backupDataPage_->SetActions(std::move(actions));
}

bool SettingsShell::ApplyBackupDataPageSnapshot(
    const snowdesktop::winui::BackupDataPageSnapshot& snapshot)
{
    return !closed_ && ownerThreadId_ == GetCurrentThreadId() &&
        backupDataPage_ &&
        navigation_.Route().page == SettingsPage::BackupAndData &&
        navigation_.Generation() == snapshot.generation &&
        backupDataPage_->ApplySnapshot(snapshot);
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

snowdesktop::widget_runtime::WidgetSettingMutationResult
SettingsShell::FlushPendingWidgetSettings()
{
    using snowdesktop::widget_runtime::WidgetSettingMutationResult;
    using snowdesktop::widget_runtime::WidgetSettingMutationStatus;
    if (closed_)
    {
        return {WidgetSettingMutationStatus::Unavailable,
            navigation_.Generation(), 0, "settingsShellClosed", {}};
    }
    if (!widgetSettingsPage_ ||
        navigation_.Route().page != SettingsPage::WidgetSettings)
    {
        return {WidgetSettingMutationStatus::Unchanged,
            navigation_.Generation(), 0, {}, {}};
    }
    return widgetSettingsPage_->FlushPendingEdits();
}

void SettingsShell::SuspendInteraction() noexcept
{
    if (closed_)
        return;
    try
    {
        if (generalPage_)
            generalPage_->Deactivate();
        if (personalizationPage_)
            personalizationPage_->Deactivate();
        if (desktopPage_)
            desktopPage_->Deactivate();
        if (dockPage_)
            dockPage_->Deactivate();
        if (homeAboutPage_)
            homeAboutPage_->Deactivate();
        if (widgetSettingsPage_)
            widgetSettingsPage_->Deactivate();
        if (widgetsPage_)
            widgetsPage_->Deactivate();
        if (backupDataPage_)
            backupDataPage_->Deactivate();
    }
    catch (...)
    {
    }
}

void SettingsShell::ResumeInteraction() noexcept
{
    if (closed_)
        return;
    try
    {
        ApplyNavigationIcons();
        renderedPageRoute_.reset();
        RenderPageCards(true);
        ScheduleFocus();
    }
    catch (...)
    {
    }
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
        if (dockPage_)
            dockPage_->ApplySnapshot(snapshot);
        if (homeAboutPage_)
            homeAboutPage_->ApplySnapshot(snapshot);
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
        const SettingsRoute canonicalRoute =
            CanonicalizeSettingsRoute(route);
        if (notifyHost && routeRequested_)
        {
            // The host synchronously validates and commits the route before
            // the shell renders it. This is essential for component routes:
            // their instance and declarative settings snapshot must exist
            // before the presenter becomes active.
            routeRequested_(canonicalRoute);
            if (navigation_.Route() == canonicalRoute)
                return true;
            RenderRoute();
            return false;
        }

        const bool changed = navigation_.Navigate(canonicalRoute);
        if (changed)
            RenderRoute();
        else if (navigation_.IsRouteAvailable(canonicalRoute) &&
                 navigation_.Route() == canonicalRoute)
            ScheduleFocus();
        else
            return false;

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
    if (!searchResults_.empty())
    {
        auto visibleResults = searchResults_;
        (void)SetSearchResults(std::move(visibleResults),
            navigation_.Generation(), searchRequestId_);
    }
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
        std::erase_if(results, [this](const auto& result) {
            return !navigation_.IsRouteAvailable(result.route);
        });
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

void SettingsShell::ShowWidgetInstallConfirmation(
    std::uint64_t generation,
    snowdesktop::winui::WidgetInstallConfirmationRequest request,
    DialogCompletedCallback completed)
{
    if (closed_ || generation != navigation_.Generation() ||
        !IsWidgetsBackendPage(navigation_.Route().page))
    {
        if (completed)
            completed(false);
        return;
    }
    ShowWidgetInstallConfirmationAsync(generation, navigation_.Route(),
        std::move(request), std::move(completed));
}

void SettingsShell::ShowWidgetPermissionEditor(
    std::uint64_t generation,
    snowdesktop::winui::WidgetPermissionEditorRequest request,
    snowdesktop::winui::WidgetsPageActions::PermissionEditorCompletion
        completed)
{
    if (closed_ || generation != navigation_.Generation() ||
        navigation_.Route().page != SettingsPage::Widgets)
    {
        if (completed)
            completed({});
        return;
    }
    ShowWidgetPermissionEditorAsync(generation, navigation_.Route(),
        std::move(request), std::move(completed));
}

void SettingsShell::HookEvents()
{
    shellPointerPressedHandler_ = winrt::box_value(muxi::PointerEventHandler{
        [this](const winrt::Windows::Foundation::IInspectable&,
               const muxi::PointerRoutedEventArgs& args) {
            if (closed_)
                return;
            const auto focused = muxi::FocusManager::GetFocusedElement(
                ShellRoot().XamlRoot());
            const auto focusedNumber = FindNumberBoxAncestor(focused);
            if (!focusedNumber ||
                IsWithinNumberBox(args.OriginalSource(), focusedNumber))
            {
                return;
            }

            auto current = args.OriginalSource().try_as<mux::DependencyObject>();
            while (current)
            {
                if (const auto control = current.try_as<muxc::Control>();
                    control && control.Focus(mux::FocusState::Pointer))
                {
                    return;
                }
                current = VisualParent(current);
            }
            (void)PageScrollViewer().Focus(mux::FocusState::Pointer);
        }});
    ShellRoot().AddHandler(
        mux::UIElement::PointerPressedEvent(),
        shellPointerPressedHandler_,
        true);

    actualThemeChangedToken_ = ShellRoot().ActualThemeChanged(
        [this](const mux::FrameworkElement&,
               const winrt::Windows::Foundation::IInspectable&) {
            if (!closed_)
            {
                ApplyNavigationIcons();
                RenderPageHeaderIcon();
                NotifyActualThemeChanged();
            }
        });

    integratedTitleBarLoadedToken_ = IntegratedTitleBarHost().Loaded(
        [this](const winrt::Windows::Foundation::IInspectable&,
               const mux::RoutedEventArgs&) {
            NotifyIntegratedTitleBarLayoutChanged();
        });
    integratedTitleBarSizeChangedToken_ =
        IntegratedTitleBarHost().SizeChanged(
            [this](const winrt::Windows::Foundation::IInspectable&,
                   const mux::SizeChangedEventArgs&) {
                NotifyIntegratedTitleBarLayoutChanged();
            });
    titleBarBackToken_ = TitleBarBackButton().Click(
        [this](const winrt::Windows::Foundation::IInspectable&,
               const mux::RoutedEventArgs&) {
            NavigateBack();
        });
    titleBarPaneToggleToken_ = TitleBarPaneToggleButton().Click(
        [this](const winrt::Windows::Foundation::IInspectable&,
               const mux::RoutedEventArgs&) {
            if (!closed_)
                NavigationRoot().IsPaneOpen(!NavigationRoot().IsPaneOpen());
        });
    backKeyboardAcceleratorToken_ = BackKeyboardAccelerator().Invoked(
        [this](const muxi::KeyboardAccelerator&,
               const muxi::KeyboardAcceleratorInvokedEventArgs& args) {
            NavigateBack();
            args.Handled(true);
        });
    searchKeyboardAcceleratorToken_ = SearchKeyboardAccelerator().Invoked(
        [this](const muxi::KeyboardAccelerator&,
               const muxi::KeyboardAcceleratorInvokedEventArgs& args) {
            if (!closed_)
            {
                NavigationRoot().IsPaneOpen(true);
                SettingsSearchBox().Focus(mux::FocusState::Keyboard);
            }
            args.Handled(true);
        });
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
                     SettingsPage::AppearanceTheme,
                     SettingsPage::AppearanceWidgets,
                     SettingsPage::AppearanceDesktopIcons,
                     SettingsPage::AppearanceIconBeautification,
                     SettingsPage::Desktop, SettingsPage::DesktopCategories,
                     SettingsPage::Dock, SettingsPage::Taskbar,
                     SettingsPage::Widgets,
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
        if (shellPointerPressedHandler_)
        {
            ShellRoot().RemoveHandler(
                mux::UIElement::PointerPressedEvent(),
                shellPointerPressedHandler_);
        }
        if (actualThemeChangedToken_.value)
            ShellRoot().ActualThemeChanged(actualThemeChangedToken_);
        if (integratedTitleBarLoadedToken_.value)
            IntegratedTitleBarHost().Loaded(integratedTitleBarLoadedToken_);
        if (integratedTitleBarSizeChangedToken_.value)
        {
            IntegratedTitleBarHost().SizeChanged(
                integratedTitleBarSizeChangedToken_);
        }
        if (titleBarBackToken_.value)
            TitleBarBackButton().Click(titleBarBackToken_);
        if (titleBarPaneToggleToken_.value)
            TitleBarPaneToggleButton().Click(titleBarPaneToggleToken_);
        if (backKeyboardAcceleratorToken_.value)
            BackKeyboardAccelerator().Invoked(backKeyboardAcceleratorToken_);
        if (searchKeyboardAcceleratorToken_.value)
            SearchKeyboardAccelerator().Invoked(searchKeyboardAcceleratorToken_);
        if (selectionChangedToken_.value)
            NavigationRoot().SelectionChanged(selectionChangedToken_);
        if (breadcrumbClickedToken_.value)
            PageBreadcrumb().ItemClicked(breadcrumbClickedToken_);
        if (searchTextChangedToken_.value)
            SettingsSearchBox().TextChanged(searchTextChangedToken_);
        if (searchQuerySubmittedToken_.value)
            SettingsSearchBox().QuerySubmitted(searchQuerySubmittedToken_);
        if (searchSuggestionChosenToken_.value)
            SettingsSearchBox().SuggestionChosen(searchSuggestionChosenToken_);
        if (cancelOperationToken_.value)
            CancelOperationButton().Click(cancelOperationToken_);
    }
    catch (...)
    {
    }
    actualThemeChangedToken_ = {};
    integratedTitleBarLoadedToken_ = {};
    integratedTitleBarSizeChangedToken_ = {};
    titleBarBackToken_ = {};
    titleBarPaneToggleToken_ = {};
    backKeyboardAcceleratorToken_ = {};
    searchKeyboardAcceleratorToken_ = {};
    selectionChangedToken_ = {};
    breadcrumbClickedToken_ = {};
    searchTextChangedToken_ = {};
    searchQuerySubmittedToken_ = {};
    searchSuggestionChosenToken_ = {};
    cancelOperationToken_ = {};
    shellPointerPressedHandler_ = nullptr;
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
    RenderPageHeaderIcon();
    muxa::AutomationProperties::SetName(PageTitle(), title);
    RenderPageCards(forcePageCards);
    if (scheduleFocus)
        ScheduleFocus();
}

void SettingsShell::RenderNavigationSelection()
{
    updatingNavigation_ = true;
    SettingsPage selectedPage = navigation_.Route().page;
    if (selectedPage == SettingsPage::WidgetSettings)
        selectedPage = SettingsPage::Widgets;
    if (selectedPage == SettingsPage::Personalization)
        selectedPage = SettingsPage::AppearanceTheme;
    if (selectedPage == SettingsPage::AppearanceTheme ||
        selectedPage == SettingsPage::AppearanceWidgets ||
        selectedPage == SettingsPage::AppearanceDesktopIcons ||
        selectedPage == SettingsPage::AppearanceIconBeautification)
    {
        PersonalizationItem().IsExpanded(true);
    }
    NavigationRoot().SelectedItem(NavigationItemForPage(selectedPage));
    const bool canGoBack = navigation_.CanGoBack();
    TitleBarBackButton().IsEnabled(canGoBack);
    BackKeyboardAccelerator().IsEnabled(canGoBack);
    updatingNavigation_ = false;
}

void SettingsShell::NavigateBack()
{
    if (closed_)
        return;
    const auto route = navigation_.PeekBack();
    if (!route)
        return;
    if (routeRequested_)
        routeRequested_(*route);
    else if (navigation_.GoBack())
        RenderRoute();
}

void SettingsShell::ApplyNavigationIcons()
{
    struct IconDescriptor
    {
        muxc::NavigationViewItem item;
        std::wstring_view asset;
        std::wstring_view fallbackGlyph;
    };

    const std::array descriptors{
        IconDescriptor{GeneralItem(),
            L"ms-appx:///Assets/Settings/Icons/general.svg", L"\xE713"},
        IconDescriptor{PersonalizationItem(),
            L"ms-appx:///Assets/Settings/Icons/appearance.svg", L"\xE771"},
        IconDescriptor{AppearanceThemeItem(),
            L"ms-appx:///Assets/Settings/Icons/appearance-theme.svg",
            L"\xE790"},
        IconDescriptor{AppearanceWidgetsItem(),
            L"ms-appx:///Assets/Settings/Icons/appearance-widgets.svg",
            L"\xECA5"},
        IconDescriptor{AppearanceDesktopIconsItem(),
            L"ms-appx:///Assets/Settings/Icons/appearance-desktop-icons.svg",
            L"\xE7F4"},
        IconDescriptor{AppearanceIconBeautificationItem(),
            L"ms-appx:///Assets/Settings/Icons/appearance-icon-beautification.svg",
            L"\xE793"},
        IconDescriptor{DesktopItem(),
            L"ms-appx:///Assets/Settings/Icons/desktop.svg", L"\xE7F4"},
        IconDescriptor{CategoriesItem(),
            L"ms-appx:///Assets/Settings/Icons/categories.svg", L"\xE8B7"},
        IconDescriptor{DockItem(),
            L"ms-appx:///Assets/Settings/Icons/dock.svg", L"\xEBC8"},
        IconDescriptor{TaskbarItem(),
            L"ms-appx:///Assets/Settings/Icons/taskbar.svg", L"\xEBC8"},
        IconDescriptor{WidgetsItem(),
            L"ms-appx:///Assets/Settings/Icons/widgets.svg", L"\xECA5"},
        IconDescriptor{BackupItem(),
            L"ms-appx:///Assets/Settings/Icons/backup.svg", L"\xE74E"},
        IconDescriptor{AboutItem(),
            L"ms-appx:///Assets/Settings/Icons/about.svg", L"\xE946"},
        IconDescriptor{DeveloperItem(),
            L"ms-appx:///Assets/Settings/Icons/developer.svg", L"\xEC7A"},
        IconDescriptor{DebugItem(),
            L"ms-appx:///Assets/Settings/Icons/debug.svg", L"\xE7BA"},
    };

    const bool highContrast = IsHighContrastEnabled();
    for (const auto& descriptor : descriptors)
    {
        if (highContrast)
        {
            descriptor.item.Icon(CreateHighContrastNavigationIcon(
                descriptor.fallbackGlyph));
        }
        else
        {
            descriptor.item.Icon(CreateColorNavigationIcon(
                descriptor.asset));
        }
    }
}

void SettingsShell::RenderPageHeaderIcon()
{
    PageHeaderIconHost().Content(nullptr);
    PageHeaderIconHost().Visibility(mux::Visibility::Collapsed);
    const auto item = NavigationItemForPage(navigation_.Route().page);
    if (!item)
        return;

    if (const auto sourceIcon = item.Icon().try_as<muxc::ImageIcon>())
    {
        muxc::ImageIcon headerIcon{};
        headerIcon.Source(sourceIcon.Source());
        headerIcon.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
        headerIcon.VerticalAlignment(mux::VerticalAlignment::Stretch);
        PageHeaderIconHost().Content(headerIcon);
        PageHeaderIconHost().Visibility(mux::Visibility::Visible);
        return;
    }
    if (const auto sourceIcon = item.Icon().try_as<muxc::FontIcon>())
    {
        muxc::FontIcon headerIcon{};
        headerIcon.Glyph(sourceIcon.Glyph());
        headerIcon.FontSize(34.0);
        PageHeaderIconHost().Content(headerIcon);
        PageHeaderIconHost().Visibility(mux::Visibility::Visible);
    }
}

void SettingsShell::RenderBreadcrumb()
{
    breadcrumbRoutes_.clear();
    auto items = winrt::single_threaded_observable_vector<
        winrt::Windows::Foundation::IInspectable>();
    const auto route = navigation_.Route();
    if (route.page == SettingsPage::WidgetSettings)
    {
        breadcrumbRoutes_.push_back(
            SettingsRoute::ForPage(SettingsPage::Widgets));
        items.Append(winrt::box_value(Localize("settings.nav.widgets")));
        breadcrumbRoutes_.push_back(route);
        items.Append(winrt::box_value(PageTitleText(SettingsPage::WidgetSettings)));
    }
    PageBreadcrumb().Visibility(route.page == SettingsPage::WidgetSettings
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
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

    const auto usesGeneralPresenter = [](SettingsPage page) {
        return page == SettingsPage::General ||
            page == SettingsPage::Desktop ||
            page == SettingsPage::Dock;
    };
    const auto usesPersonalizationPresenter = [](SettingsPage page) {
        return page == SettingsPage::Personalization ||
            page == SettingsPage::AppearanceTheme ||
            page == SettingsPage::AppearanceWidgets;
    };
    const auto usesDesktopPresenter = [](SettingsPage page) {
        return page == SettingsPage::DesktopCategories ||
            page == SettingsPage::AppearanceDesktopIcons ||
            page == SettingsPage::AppearanceIconBeautification;
    };
    const auto usesDockPresenter = [](SettingsPage page) {
        return page == SettingsPage::Dock ||
            page == SettingsPage::Taskbar;
    };
    const bool leavingGeneral = renderedPageRoute_ &&
        usesGeneralPresenter(renderedPageRoute_->page) &&
        !usesGeneralPresenter(pageRoute.page);
    if (leavingGeneral && generalPage_)
        generalPage_->Deactivate();
    const bool leavingPersonalization = renderedPageRoute_ &&
        usesPersonalizationPresenter(renderedPageRoute_->page) &&
        !usesPersonalizationPresenter(pageRoute.page);
    if (leavingPersonalization && personalizationPage_)
        personalizationPage_->Deactivate();
    const bool leavingDesktop = renderedPageRoute_ &&
        usesDesktopPresenter(renderedPageRoute_->page) &&
        !usesDesktopPresenter(pageRoute.page);
    if (leavingDesktop && desktopPage_)
        desktopPage_->Deactivate();
    const bool leavingDock = renderedPageRoute_ &&
        usesDockPresenter(renderedPageRoute_->page) &&
        !usesDockPresenter(pageRoute.page);
    if (leavingDock && dockPage_)
        dockPage_->Deactivate();
    const bool leavingHomeAbout = renderedPageRoute_ &&
        (renderedPageRoute_->page == SettingsPage::Home ||
            renderedPageRoute_->page == SettingsPage::About ||
            renderedPageRoute_->page == SettingsPage::Debug) &&
        pageRoute.page != SettingsPage::Home &&
        pageRoute.page != SettingsPage::About &&
        pageRoute.page != SettingsPage::Debug;
    if (leavingHomeAbout && homeAboutPage_)
        homeAboutPage_->Deactivate();
    const bool leavingWidgetSettings = renderedPageRoute_ &&
        renderedPageRoute_->page == SettingsPage::WidgetSettings &&
        pageRoute.page != SettingsPage::WidgetSettings;
    if (leavingWidgetSettings && widgetSettingsPage_)
        widgetSettingsPage_->Deactivate();
    const bool leavingWidgets = renderedPageRoute_ &&
        IsWidgetsBackendPage(renderedPageRoute_->page) &&
        !IsWidgetsBackendPage(pageRoute.page);
    if (leavingWidgets && widgetsPage_)
        widgetsPage_->Deactivate();
    const bool leavingBackup = renderedPageRoute_ &&
        renderedPageRoute_->page == SettingsPage::BackupAndData &&
        pageRoute.page != SettingsPage::BackupAndData;
    if (leavingBackup && backupDataPage_)
        backupDataPage_->Deactivate();

    PageCards().Children().Clear();
    focusTargets_.clear();

    const auto addPlaceholder = [this](
                                    std::string focusId,
                                    std::string_view title,
                                    std::string_view description) {
        PageCards().Children().Append(CreatePlaceholderCard(
            std::move(focusId), title, description));
    };
    const auto registerDockFocus = [this](
                                       std::initializer_list<
                                           std::string_view> ids) {
        if (!dockPage_) return;
        for (const std::string_view id : ids)
        {
            RegisterFocusTarget(
                std::string(id), dockPage_->FocusTarget(id));
        }
    };
    const auto registerDesktopFocus = [this](
                                          std::initializer_list<
                                              std::string_view> ids) {
        if (!desktopPage_) return;
        for (const std::string_view id : ids)
        {
            RegisterFocusTarget(
                std::string(id), desktopPage_->FocusTarget(id));
        }
    };
    const auto registerPersonalizationFocus = [this](
        std::initializer_list<std::string_view> ids) {
        if (!personalizationPage_) return;
        for (const std::string_view id : ids)
        {
            RegisterFocusTarget(
                std::string(id), personalizationPage_->FocusTarget(id));
        }
    };
    switch (navigation_.Route().page)
    {
    case SettingsPage::Home:
        if (homeAboutPage_)
        {
            PageCards().Children().Append(homeAboutPage_->HomeContent());
            for (const std::string_view focusId : {
                     "home.theme", "home.dock", "home.widgets",
                     "home.update", "home.backup"})
            {
                RegisterFocusTarget(std::string(focusId),
                    homeAboutPage_->FocusTarget(
                        SettingsPage::Home, focusId));
            }
            homeAboutPage_->Activate(SettingsPage::Home);
        }
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
    case SettingsPage::AppearanceTheme:
        if (personalizationPage_)
        {
            PageCards().Children().Append(
                personalizationPage_->ThemeContent());
            registerPersonalizationFocus({
                "personalization.theme",
                "personalization.globalTheme",
                "personalization.quickNavigationTheme",
                "personalization.collectionPopupTheme",
                "personalization.contextMenu"});
            personalizationPage_->Activate();
        }
        break;
    case SettingsPage::AppearanceWidgets:
        if (personalizationPage_)
        {
            PageCards().Children().Append(
                personalizationPage_->WidgetAppearanceContent());
            registerPersonalizationFocus({
                "personalization.backgroundColor",
                "personalization.borderColor",
                "personalization.widgetAlpha",
                "personalization.borderAlpha",
                "personalization.enableGradient",
                "personalization.gradientEndAlpha",
                "personalization.glass",
                "personalization.blurRadius",
                "personalization.acrylic",
                "personalization.contentTheme",
                "personalization.cornerRadius",
                "personalization.barHeight",
                "desktop.categoryLayout",
                "desktop.tabHeight",
                "personalization.tabHeight"});
            personalizationPage_->Activate();
        }
        break;
    case SettingsPage::Desktop:
        if (generalPage_)
        {
            PageCards().Children().Append(
                generalPage_->DesktopBehaviorContent());
            generalPage_->RegisterFocusTargets(
                [this](std::string focusId,
                       const mux::FrameworkElement& element) {
                    RegisterFocusTarget(std::move(focusId), element);
                });
            generalPage_->Activate();
        }
        break;
    case SettingsPage::AppearanceDesktopIcons:
        if (desktopPage_)
        {
            PageCards().Children().Append(
                desktopPage_->DesktopIconsContent());
            registerDesktopFocus({
                "desktop.spacing", "desktop.iconSpacing", "desktop.iconSize",
                "desktop.itemFontSize", "desktop.listFontSize",
                "desktop.fontWeight", "desktop.shortcutArrow"});
            desktopPage_->Activate();
        }
        break;
    case SettingsPage::AppearanceIconBeautification:
        if (desktopPage_)
        {
            PageCards().Children().Append(
                desktopPage_->IconBeautificationContent());
            registerDesktopFocus({
                "desktop.iconBeautify",
                "desktop.iconBeautify.mode",
                "desktop.iconBeautify.backgroundColor",
                "desktop.iconBeautify.backgroundOpacity",
                "desktop.iconBeautify.gradient",
                "desktop.iconBeautify.gradientEndColor",
                "desktop.iconBeautify.gradientDirection",
                "desktop.iconBeautify.shape",
                "desktop.iconBeautify.contentScale",
                "desktop.iconBeautify.highlightStrength",
                "desktop.iconBeautify.highlightSize",
                "desktop.iconBeautify.highlightAngle",
                "desktop.iconBeautify.shadeStrength",
                "desktop.iconBeautify.edgeHighlight",
                "desktop.iconBeautify.filter",
                "desktop.iconBeautify.filterColor",
                "desktop.iconBeautify.filterStrength",
                "desktop.iconBeautify.shadowStrength",
                "desktop.iconBeautify.outline",
                "desktop.iconBeautify.outlineWidth",
                "desktop.iconBeautify.outlineOpacity",
                "desktop.iconBeautify.outlineColor"});
            desktopPage_->Activate();
        }
        break;
    case SettingsPage::DesktopCategories:
        if (desktopPage_)
        {
            PageCards().Children().Append(desktopPage_->CategoryContent());
            for (const std::string_view focusId : {
                      "desktop.categoryCounts",
                      "desktop.categories",
                      "desktop.categoryRules",
                      "desktop.category.add"})
            {
                RegisterFocusTarget(
                    std::string(focusId),
                    desktopPage_->FocusTarget(focusId));
            }
            desktopPage_->Activate();
        }
        break;
    case SettingsPage::Dock:
        if (dockPage_ && generalPage_)
        {
            PageCards().Children().Append(dockPage_->DockEnableContent());
            PageCards().Children().Append(
                generalPage_->DockShortcutContent());
            PageCards().Children().Append(dockPage_->DockContent());
            generalPage_->RegisterFocusTargets(
                [this](std::string focusId,
                       const mux::FrameworkElement& element) {
                    RegisterFocusTarget(std::move(focusId), element);
                });
            registerDockFocus({
                "dock.enable", "dock.position", "dock.layout",
                "dock.monitor", "dock.thickness",
                "dock.floatingShortcutMode", "dock.floatingEdgeSwipe",
                "dock.showWindowsButton", "dock.showFrequentItems",
                "dock.frequentItemCount", "dock.keepWhenDesktopHidden"});
            generalPage_->Activate();
            dockPage_->Activate();
        }
        break;
    case SettingsPage::Taskbar:
        if (dockPage_)
        {
            dockPage_->ActivateTaskbar();
            PageCards().Children().Append(dockPage_->TaskbarContent());
            registerDockFocus({
                "taskbar.autoHide", "taskbar.alignment",
                "taskbar.systemTheme", "taskbar.theme",
                "taskbar.contentTheme", "taskbar.backgroundColor",
                "taskbar.borderColor", "taskbar.backgroundOpacity",
                "taskbar.borderOpacity", "taskbar.glass",
                "taskbar.blurRadius", "taskbar.acrylic",
                "taskbar.restartExplorer",
                "taskbar.dynamic.visibleWindow",
                "taskbar.dynamic.maximizedWindow",
                "taskbar.dynamic.shellUi"});
        }
        break;
    case SettingsPage::DockAndTaskbar:
        // Compatibility routes are canonicalized before they enter history.
        break;
    case SettingsPage::Widgets:
        if (widgetsPage_)
        {
            PageCards().Children().Append(widgetsPage_->Content());
            widgetsPage_->Activate(navigation_.Generation());
            for (const std::string_view focusId : {
                     "widgets.search", "widgets.install",
                     "widgets.workshop", "widgets.developer",
                     "widgets.installed", "widgets.included",
                     "widgets.sources", "widgets.permissions"})
            {
                RegisterFocusTarget(std::string(focusId),
                    widgetsPage_->FocusTarget(focusId));
            }
        }
        break;
    case SettingsPage::WidgetSettings:
        if (widgetSettingsPage_ &&
            widgetSettingsPage_->WidgetId() ==
                navigation_.Route().widgetInstanceId)
        {
            PageCards().Children().Append(widgetSettingsPage_->Content());
            if (widgetSettingsService_)
            {
                if (const auto snapshot = widgetSettingsService_->Snapshot(
                        navigation_.Route().widgetInstanceId))
                {
                    for (const auto& field : snapshot->fields)
                    {
                        RegisterFocusTarget(field.schema.key,
                            widgetSettingsPage_->FocusTarget(
                                field.schema.key));
                    }
                }
            }
            widgetSettingsPage_->Activate();
        }
        else
        {
            addPlaceholder("widget.fields", "settings.widget.fields",
                "settings.widget.fields.description");
        }
        break;
    case SettingsPage::BackupAndData:
        if (backupDataPage_)
        {
            PageCards().Children().Append(backupDataPage_->Content());
            for (const std::string_view focusId : {
                     "backup.layout", "backup.full", "backup.directory",
                     "backup.migration"})
            {
                RegisterFocusTarget(std::string(focusId),
                    backupDataPage_->FocusTarget(focusId));
            }
            backupDataPage_->Activate();
        }
        break;
    case SettingsPage::About:
        if (homeAboutPage_)
        {
            PageCards().Children().Append(homeAboutPage_->AboutContent());
            for (const std::string_view focusId : {
                     "about.version", "about.profile", "about.project",
                     "about.community", "about.thirdparty"})
            {
                RegisterFocusTarget(std::string(focusId),
                    homeAboutPage_->FocusTarget(
                        SettingsPage::About, focusId));
            }
            homeAboutPage_->Activate(SettingsPage::About);
        }
        break;
    case SettingsPage::DeveloperTools:
        if (widgetsPage_)
        {
            PageCards().Children().Append(
                widgetsPage_->DeveloperToolsContent());
            widgetsPage_->Activate(navigation_.Generation());
            for (const std::string_view focusId : {
                     "developer.overrides", "developer.agentSkill",
                     "developer.workspace", "developer.cli",
                     "developer.publish", "developer.reference",
                     "developer.runtime", "developer.tools"})
            {
                RegisterFocusTarget(std::string(focusId),
                    widgetsPage_->DeveloperToolsFocusTarget(focusId));
            }
        }
        break;
    case SettingsPage::Debug:
        if (homeAboutPage_)
        {
            PageCards().Children().Append(homeAboutPage_->DebugContent());
            for (const std::string_view focusId : {
                     "debug.demo_mode", "debug.animation",
                     "debug.crash"})
            {
                RegisterFocusTarget(std::string(focusId),
                    homeAboutPage_->FocusTarget(
                        SettingsPage::Debug, focusId));
            }
            homeAboutPage_->Activate(SettingsPage::Debug);
        }
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
    if (!focusId.empty() &&
        navigation_.Route().page == SettingsPage::WidgetSettings &&
        widgetSettingsPage_)
    {
        if (auto target = widgetSettingsPage_->FocusTarget(focusId))
        {
            target.StartBringIntoView();
            (void)target.Focus(mux::FocusState::Keyboard);
            return;
        }
    }
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
    case SettingsPage::General: return Localize("app.settings.general");
    case SettingsPage::Personalization:
        return Localize("app.settings.appearance");
    case SettingsPage::AppearanceTheme:
        return Localize("settings.personalization.theme");
    case SettingsPage::AppearanceWidgets:
        return Localize("settings.personalization.widgets");
    case SettingsPage::AppearanceDesktopIcons:
        return Localize("app.settings.desktop_icons");
    case SettingsPage::AppearanceIconBeautification:
        return Localize("app.settings.icon_beautify");
    case SettingsPage::Desktop: return Localize("settings.nav.desktop");
    case SettingsPage::DesktopCategories:
        return Localize("settings.nav.categories");
    case SettingsPage::Dock: return Localize("settings.nav.dock");
    case SettingsPage::Taskbar: return Localize("settings.nav.taskbar");
    case SettingsPage::DockAndTaskbar:
        return Localize("settings.nav.dock");
    case SettingsPage::Widgets: return Localize("app.settings.widgets");
    case SettingsPage::WidgetSettings: return Localize("app.settings.widgets");
    case SettingsPage::BackupAndData: return Localize("app.settings.backup");
    case SettingsPage::About: return Localize("app.settings.about");
    case SettingsPage::DeveloperTools:
        return Localize("app.settings.widgets_developer_tools");
    case SettingsPage::Debug: return Localize("app.settings.debug");
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
    case SettingsPage::AppearanceTheme:
        return Localize("settings.personalization.theme.description");
    case SettingsPage::AppearanceWidgets:
        return Localize("settings.personalization.widgets.description");
    case SettingsPage::AppearanceDesktopIcons:
        return Localize("settings.desktop.layout.description");
    case SettingsPage::AppearanceIconBeautification:
        return Localize("settings.desktop.beautify.description");
    case SettingsPage::Desktop:
        return Localize("settings.page.desktop.description");
    case SettingsPage::DesktopCategories:
        return Localize("settings.page.categories.description");
    case SettingsPage::Dock:
    case SettingsPage::DockAndTaskbar:
        return Localize("settings.page.dock.description");
    case SettingsPage::Taskbar:
        return Localize("settings.page.taskbar.description");
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
    case SettingsPage::Personalization:
    case SettingsPage::AppearanceTheme: return AppearanceThemeItem();
    case SettingsPage::AppearanceWidgets: return AppearanceWidgetsItem();
    case SettingsPage::AppearanceDesktopIcons:
        return AppearanceDesktopIconsItem();
    case SettingsPage::AppearanceIconBeautification:
        return AppearanceIconBeautificationItem();
    case SettingsPage::Desktop: return DesktopItem();
    case SettingsPage::DesktopCategories: return CategoriesItem();
    case SettingsPage::Dock:
    case SettingsPage::DockAndTaskbar: return DockItem();
    case SettingsPage::Taskbar: return TaskbarItem();
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
    const std::size_t count = (std::min)(
        searchResults_.size(), static_cast<std::size_t>(searchItems_.Size()));
    for (std::size_t index = 0; index < count; ++index)
    {
        // AutoSuggestBox returns the exact boxed ItemsSource entry. Match that
        // identity instead of its display text so two widgets/settings with
        // the same localized label can never navigate to the first match.
        if (searchItems_.GetAt(static_cast<std::uint32_t>(index)) ==
            selectedItem)
        {
            RequestRoute(searchResults_[index].route);
            return true;
        }
    }
    return false;
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

winrt::fire_and_forget SettingsShell::ShowWidgetInstallConfirmationAsync(
    std::uint64_t generation,
    SettingsRoute route,
    snowdesktop::winui::WidgetInstallConfirmationRequest request,
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
        dialog.Title(winrt::box_value(Localize(
            "app.settings.widgets_confirm_install")));
        dialog.PrimaryButtonText(Localize(
            "app.settings.widgets_confirm_install"));
        dialog.CloseButtonText(Localize("app.settings.cancel"));
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);

        muxc::StackPanel content;
        content.Spacing(8.0);
        content.MaxWidth(620.0);
        const auto appendText = [&](std::wstring text,
                                    bool emphasized = false) {
            muxc::TextBlock block;
            block.Text(std::move(text));
            block.TextWrapping(mux::TextWrapping::Wrap);
            if (emphasized)
            {
                block.FontWeight(
                    winrt::Windows::UI::Text::FontWeights::SemiBold());
            }
            content.Children().Append(block);
        };

        appendText(Localize(request.reasons.empty()
                ? "settings.widgets.install.reviewPrompt"
                : "app.settings.widgets_install_confirm"));
        if (!request.packageName.empty())
            appendText(request.packageName, true);
        if (!request.version.empty())
        {
            appendText(Localize("app.settings.version") + L": " +
                request.version);
        }
        if (!request.packageId.empty())
        {
            appendText(Localize("app.settings.widgets_package_id") +
                L": " + request.packageId);
        }
        if (!request.sha256.empty())
        {
            appendText(Localize("settings.widgets.install.fingerprint") +
                L":\n" + request.sha256);
        }

        for (const auto& reason : request.reasons)
        {
            std::wstring description;
            switch (reason.kind)
            {
            case snowdesktop::winui::
                    WidgetInstallConfirmationReasonKind::NewPermission:
            {
                std::wstring value = reason.value;
                if (!reason.valueLabelKey.empty())
                    value = Localize(reason.valueLabelKey);
                description = FormatSingleValue(Localize(
                    "app.settings.widgets_new_permission"), value);
                break;
            }
            case snowdesktop::winui::
                    WidgetInstallConfirmationReasonKind::NewWebsite:
                description = FormatSingleValue(Localize(
                    "app.settings.widgets_new_website"), reason.value);
                break;
            case snowdesktop::winui::
                    WidgetInstallConfirmationReasonKind::SourceChange:
                description = Localize(
                    "app.settings.widgets_source_change");
                break;
            case snowdesktop::winui::
                    WidgetInstallConfirmationReasonKind::Other:
            default:
                break;
            }
            if (!description.empty())
                appendText(L"• " + description);
        }

        if (!request.technicalDetails.empty())
        {
            muxc::Expander technicalDetails;
            technicalDetails.HorizontalAlignment(
                mux::HorizontalAlignment::Stretch);
            technicalDetails.HorizontalContentAlignment(
                mux::HorizontalAlignment::Stretch);
            technicalDetails.Header(winrt::box_value(Localize(
                "app.settings.widgets_technical_details")));
            muxc::TextBlock details;
            details.Text(request.technicalDetails);
            details.TextWrapping(mux::TextWrapping::Wrap);
            details.IsTextSelectionEnabled(true);
            technicalDetails.Content(details);
            muxa::AutomationProperties::SetName(technicalDetails,
                Localize("app.settings.widgets_technical_details"));
            content.Children().Append(technicalDetails);
        }

        muxc::ScrollViewer scroller;
        scroller.MaxHeight(560.0);
        scroller.VerticalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        scroller.Content(content);
        dialog.Content(scroller);

        const auto result = co_await dialog.ShowAsync();
        if (activeDialog_ == dialog)
            activeDialog_ = nullptr;
        if (!closed_ && generation == navigation_.Generation() &&
            navigation_.Route() == route && completed)
        {
            completed(result == muxc::ContentDialogResult::Primary);
        }
    }
    catch (...)
    {
        activeDialog_ = nullptr;
        if (!closed_ && generation == navigation_.Generation() &&
            navigation_.Route() == route && completed)
        {
            completed(false);
        }
    }
}

winrt::fire_and_forget SettingsShell::ShowWidgetPermissionEditorAsync(
    std::uint64_t generation,
    SettingsRoute route,
    snowdesktop::winui::WidgetPermissionEditorRequest request,
    snowdesktop::winui::WidgetsPageActions::PermissionEditorCompletion
        completed)
{
    auto lifetime = get_strong();
    try
    {
        if (activeDialog_)
            activeDialog_.Hide();

        muxc::ContentDialog dialog;
        activeDialog_ = dialog;
        dialog.XamlRoot(XamlRoot());
        std::wstring title = Localize(
            "app.settings.widgets_manage_permissions");
        if (!request.packageName.empty())
            title += L" — " + request.packageName;
        dialog.Title(winrt::box_value(title));
        dialog.PrimaryButtonText(Localize("app.settings.apply"));
        if (request.canRevoke)
        {
            dialog.SecondaryButtonText(Localize(
                "app.settings.widgets_revoke_permissions"));
        }
        dialog.CloseButtonText(Localize("app.settings.cancel"));
        dialog.DefaultButton(muxc::ContentDialogButton::Primary);

        muxc::StackPanel content;
        content.Spacing(8.0);
        content.MaxWidth(620.0);
        std::vector<std::pair<std::wstring, muxc::CheckBox>> editors;
        editors.reserve(request.permissions.size());
        const auto appendHeading = [&](std::string_view key) {
            muxc::TextBlock heading;
            heading.Text(Localize(key));
            heading.FontWeight(
                winrt::Windows::UI::Text::FontWeights::SemiBold());
            heading.TextWrapping(mux::TextWrapping::Wrap);
            content.Children().Append(heading);
        };
        const auto appendPermission = [&](const auto& permission) {
            std::wstring label = permission.label;
            if (!permission.labelKey.empty())
                label = Localize(permission.labelKey);
            if (label.empty()) label = permission.id;
            muxc::CheckBox check;
            check.Content(winrt::box_value(label));
            check.IsChecked(permission.granted);
            check.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            check.UseSystemFocusVisuals(true);
            muxa::AutomationProperties::SetName(check, label);
            content.Children().Append(check);
            editors.emplace_back(permission.id, check);
        };

        const bool hasRequired = std::any_of(request.permissions.begin(),
            request.permissions.end(), [](const auto& permission) {
                return permission.required;
            });
        if (hasRequired)
        {
            appendHeading(
                "app.widget_permission.permissions_required_heading");
            muxc::TextBlock notice;
            notice.Text(Localize(
                "app.settings.widgets_permission_required_notice"));
            notice.Opacity(0.72);
            notice.TextWrapping(mux::TextWrapping::Wrap);
            content.Children().Append(notice);
            for (const auto& permission : request.permissions)
                if (permission.required) appendPermission(permission);
        }
        const bool hasOptional = std::any_of(request.permissions.begin(),
            request.permissions.end(), [](const auto& permission) {
                return !permission.required;
            });
        if (hasOptional)
        {
            appendHeading(
                "app.widget_permission.permissions_optional_heading");
            for (const auto& permission : request.permissions)
                if (!permission.required) appendPermission(permission);
        }
        if (!request.declaredNetworkDomains.empty())
        {
            appendHeading("app.widget_permission.domains_heading");
            for (const std::wstring& domain :
                 request.declaredNetworkDomains)
            {
                muxc::TextBlock item;
                item.Text(L"• " + domain);
                item.TextWrapping(mux::TextWrapping::Wrap);
                content.Children().Append(item);
            }
        }
        muxc::ScrollViewer scroller;
        scroller.MaxHeight(560.0);
        scroller.VerticalScrollBarVisibility(
            muxc::ScrollBarVisibility::Auto);
        scroller.Content(content);
        dialog.Content(scroller);

        const auto result = co_await dialog.ShowAsync();
        if (activeDialog_ == dialog)
            activeDialog_ = nullptr;
        if (closed_ || generation != navigation_.Generation() ||
            navigation_.Route() != route || !completed)
            co_return;

        snowdesktop::winui::WidgetPermissionEditorResult answer;
        if (result == muxc::ContentDialogResult::Primary)
        {
            answer.action =
                snowdesktop::winui::WidgetPermissionEditorAction::Apply;
            for (const auto& [permissionId, check] : editors)
            {
                const auto checked = check.IsChecked();
                if (checked && checked.Value())
                    answer.grantedPermissions.push_back(permissionId);
            }
            const bool networkGranted = std::any_of(
                answer.grantedPermissions.begin(),
                answer.grantedPermissions.end(),
                [](const std::wstring& permission) {
                    return permission == L"network.http" ||
                        permission == L"network.internet";
                });
            if (networkGranted)
                answer.grantedNetworkDomains =
                    request.declaredNetworkDomains;
        }
        else if (result == muxc::ContentDialogResult::Secondary)
        {
            answer.action =
                snowdesktop::winui::WidgetPermissionEditorAction::Revoke;
        }
        completed(std::move(answer));
    }
    catch (...)
    {
        activeDialog_ = nullptr;
        if (!closed_ && generation == navigation_.Generation() &&
            navigation_.Route() == route && completed)
            completed({});
    }
}

} // namespace winrt::SnowDesktop::implementation
