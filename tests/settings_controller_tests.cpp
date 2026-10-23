#include "settings/settings_controller.h"
#include "settings/settings_ipc_services.h"
#include "settings/settings_ipc_values.h"
#include "settings/settings_search_catalog.h"
#include "desktop/desktop_style_presets.h"
#include "winui/category_creation_acknowledgement.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>
#include <future>
#include <thread>

// The controller tests intentionally avoid linking the native persistence
// implementations. Supply only the value factories used by aggregate default
// construction so the test remains focused on controller behavior.
PersonalizationSettings PersonalizationSettings::DarkPreset()
{
    return {};
}

PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset()
{
    return {};
}

CategorySettings CategorySettings::Defaults()
{
    return {};
}

int g_categoryNormalizationCount = 0;

void NormalizeCategorySettings(CategorySettings&)
{
    ++g_categoryNormalizationCount;
}

namespace
{

using namespace snowdesktop;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

class FakeStore final : public SettingsStore
{
public:
    SettingsValues loaded;
    SettingsDomain failingDomains = SettingsDomain::None;
    int loadCount = 0;
    int personalizationSaveCount = 0;
    int dockSaveCount = 0;
    int navigationSaveCount = 0;
    int generalSaveCount = 0;
    int categorySaveCount = 0;
    int failingLoadCount = 0;
    GeneralSettings lastSavedGeneral;
    DockSettings lastSavedDock;

    SettingsActionResult Load(SettingsValues& values) override
    {
        ++loadCount;
        if (failingLoadCount > 0)
        {
            --failingLoadCount;
            return SettingsActionResult::Failure(L"injected load failure");
        }
        values = loaded;
        return SettingsActionResult::Success(SettingsDomain::All);
    }

    SettingsActionResult SavePersonalization(
        const PersonalizationSettings&) override
    {
        ++personalizationSaveCount;
        return SaveResult(SettingsDomain::Personalization);
    }

    SettingsActionResult SaveDock(const DockSettings& settings) override
    {
        ++dockSaveCount;
        lastSavedDock = settings;
        return SaveResult(SettingsDomain::Dock);
    }

    SettingsActionResult SaveNavigation(
        const NavigationSettings&) override
    {
        ++navigationSaveCount;
        return SaveResult(SettingsDomain::Navigation);
    }

    SettingsActionResult SaveGeneral(
        const GeneralSettings& settings) override
    {
        ++generalSaveCount;
        lastSavedGeneral = settings;
        return SaveResult(SettingsDomain::General);
    }

    SettingsActionResult SaveCategory(const CategorySettings&) override
    {
        ++categorySaveCount;
        return SaveResult(SettingsDomain::Category);
    }

private:
    SettingsActionResult SaveResult(SettingsDomain domain) const
    {
        if (HasSettingsDomain(failingDomains, domain))
            return SettingsActionResult::Failure(L"injected failure", domain);
        return SettingsActionResult::Success(domain);
    }
};

class FakeHostActions final : public SettingsHostActions
{
public:
    int previewCount = 0;
    int commitCount = 0;
    int routeCount = 0;
    SettingsDomain previewDomains = SettingsDomain::None;
    SettingsDomain commitDomains = SettingsDomain::None;
    GeneralSettings committedGeneral;
    DockSettings committedDock;
    DesktopDisplaySettings committedDesktop;
    float previewedAlpha = 0.0f;
    float previewedDockEdgeDistanceScale = 1.0f;
    SettingsRoute route;
    SettingsActionStatus nextPreviewStatus =
        SettingsActionStatus::Succeeded;
    SettingsActionStatus nextCommitStatus =
        SettingsActionStatus::Succeeded;
    std::function<void()> duringCommit;
    int invokeCount = 0;
    Request lastRequest;

    SettingsActionResult OnSettingsPreview(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) override
    {
        ++previewCount;
        previewDomains |= domains;
        previewedAlpha = snapshot.values.personalization.widgetAlpha;
        previewedDockEdgeDistanceScale = snapshot.values.dock.edgeDistanceScale;
        if (nextPreviewStatus == SettingsActionStatus::Busy)
            return SettingsActionResult::Busy(L"preview busy");
        if (nextPreviewStatus == SettingsActionStatus::Failed)
            return SettingsActionResult::Failure(
                L"preview failed", domains);
        return SettingsActionResult::Success(domains);
    }

    SettingsActionResult OnSettingsCommitted(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) override
    {
        ++commitCount;
        commitDomains |= domains;
        committedGeneral = snapshot.values.general;
        committedDock = snapshot.values.dock;
        committedDesktop = snapshot.values.desktop;
        if (duringCommit) duringCommit();
        if (nextCommitStatus == SettingsActionStatus::Busy)
            return SettingsActionResult::Busy(L"commit busy");
        if (nextCommitStatus == SettingsActionStatus::Failed)
            return SettingsActionResult::Failure(
                L"commit failed", domains);
        return SettingsActionResult::Success(domains);
    }

    SettingsActionResult OnSettingsRouteChanged(
        const SettingsRoute& newRoute) override
    {
        ++routeCount;
        route = newRoute;
        return SettingsActionResult::Success();
    }

    SettingsActionResult Invoke(const Request& request) override
    {
        ++invokeCount;
        lastRequest = request;
        return SettingsActionResult::Success();
    }
};

void TestRoutes()
{
    const SettingsRoute page = SettingsRoute::ForPage(
        SettingsPage::DockAndTaskbar, "dock.thickness");
    Check(page.IsValid(), "ordinary page route is valid");
    Check(page.focusId == "dock.thickness",
        "page route preserves a search focus target");
    Check(SettingsPageKey(page.page) == "dock-and-taskbar",
        "page route exposes a stable diagnostics key");

    const SettingsRoute widget = SettingsRoute::ForWidget(
        L"clock-instance", "appearance.color");
    Check(widget.IsValid() &&
            widget.page == SettingsPage::WidgetSettings &&
            widget.widgetInstanceId == L"clock-instance",
        "widget route requires and preserves a stable instance id");

    Check(!SettingsRoute::ForWidget(L"").IsValid(),
        "empty widget instance route is rejected");
    SettingsRoute invalid = SettingsRoute::ForPage(SettingsPage::General);
    invalid.widgetInstanceId = L"unexpected";
    Check(!invalid.IsValid(),
        "non-widget page cannot carry a widget instance id");
    invalid = SettingsRoute::ForPage(
        static_cast<SettingsPage>(0xff));
    Check(!invalid.IsValid(),
        "unknown enum values are rejected instead of aliasing home");

    Check(static_cast<unsigned>(SettingsPage::DesktopCategories) == 13u &&
            static_cast<unsigned>(SettingsPage::AppearanceTheme) == 14u &&
            static_cast<unsigned>(SettingsPage::AppearanceWidgets) == 15u &&
            static_cast<unsigned>(SettingsPage::AppearanceDesktopIcons) ==
                16u &&
            static_cast<unsigned>(
                SettingsPage::AppearanceIconBeautification) == 17u &&
            static_cast<unsigned>(SettingsPage::DesktopPages) == 18u,
        "new settings leaves append without changing existing route values");

    Check(SettingsPageKey(SettingsPage::ThemeManager) == "theme-manager" &&
        SettingsRoute::ForPage(SettingsPage::ThemeManager).IsValid(), "theme manager has an independent stable route");
    constexpr std::array appearanceLeaves{
        SettingsPage::ThemeManager,
        SettingsPage::AppearanceTheme,
        SettingsPage::AppearanceWidgets,
        SettingsPage::AppearanceDesktopIcons,
        SettingsPage::AppearanceIconBeautification,
        SettingsPage::DesktopPages,
    };
    bool leafKeysAreUnique = true;
    for (std::size_t left = 0; left < appearanceLeaves.size(); ++left)
    {
        leafKeysAreUnique = leafKeysAreUnique &&
            SettingsRoute::ForPage(appearanceLeaves[left]).IsValid();
        for (std::size_t right = left + 1;
             right < appearanceLeaves.size(); ++right)
        {
            leafKeysAreUnique = leafKeysAreUnique &&
                SettingsPageKey(appearanceLeaves[left]) !=
                    SettingsPageKey(appearanceLeaves[right]);
        }
    }
    Check(leafKeysAreUnique,
        "every appended settings leaf is valid and has a unique stable page key");

    const auto route = snowdesktop::CanonicalizeSettingsRoute(
        snowdesktop::SettingsRoute::ForPage(snowdesktop::SettingsPage::Home, "start.explore"));
    Check(route.page == snowdesktop::SettingsPage::General && route.focusId == "start.explore",
        "legacy Home route targets General without changing guide eligibility");

    const SettingsRoute legacyAppearance = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization));
    const SettingsRoute legacyTheme = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.contextMenu"));
    const SettingsRoute legacyWidgetAppearance = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.backgroundColor"));
    const SettingsRoute edgeHighlight = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.edgeHighlight"));
    const SettingsRoute legacyTabHeight = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.tabHeight"));
    const SettingsRoute legacyCounts = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.showCounts"));
    Check(legacyAppearance.page == SettingsPage::AppearanceTheme &&
            legacyTheme.page == SettingsPage::ContextMenu &&
            legacyWidgetAppearance.page == SettingsPage::AppearanceTheme &&
            edgeHighlight.page == SettingsPage::AppearanceTheme &&
            legacyTabHeight.page == SettingsPage::AppearanceDesktopIcons &&
            legacyTabHeight.focusId == "desktop.categoryLayout" &&
            legacyCounts.page == SettingsPage::DesktopCategories &&
            legacyCounts.focusId == "desktop.categoryCounts",
        "legacy Personalization routes resolve to their owned Appearance or Categories leaf");
    const auto groupCounts = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.showGroupTabCounts"));
    Check(groupCounts.page == SettingsPage::WidgetBehavior &&
            groupCounts.focusId == "personalization.showGroupTabCounts",
        "group count links reach Widget behavior without redirecting to desktop categories");
    const auto transformCursors = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.widgetTransformCursors"));
    Check(transformCursors.page == SettingsPage::WidgetBehavior &&
            transformCursors.focusId == "personalization.widgetTransformCursors",
        "component transform cursor links reach the widget preferences page");
    const auto popupHover = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.popupHoverOpen"));
    Check(popupHover.page == SettingsPage::WidgetBehavior &&
            popupHover.focusId == "personalization.popupHoverOpen",
        "hover popup search and deep links reach Widget behavior");
    const auto popupHoverDelay = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.popupHoverDelayMs"));
    Check(popupHoverDelay.page == SettingsPage::WidgetBehavior &&
            popupHoverDelay.focusId == "personalization.popupHoverDelayMs",
        "hover delay search reaches the same Widget behavior section as its switch");
    const auto titleBarPosition = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.scrollableTitleBarOnTop"));
    Check(titleBarPosition.page == SettingsPage::WidgetBehavior &&
            titleBarPosition.focusId == "personalization.scrollableTitleBarOnTop",
        "title bar position search and deep links reach Widget behavior");
    const auto popupLeave = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.popupCloseOnMouseLeave"));
    Check(popupLeave.page == SettingsPage::WidgetBehavior &&
            popupLeave.focusId == "personalization.popupCloseOnMouseLeave",
        "close-on-leave search and deep links reach the Widget behavior switch");

    // Old settings-search publications can still carry AppearanceWidgets.
    // The production controller must relocate them while preserving the locator.
    for (const auto* focus : {"personalization.showGroupTabCounts",
             "personalization.popupHoverOpen", "personalization.popupHoverDelayMs",
             "personalization.scrollableTitleBarOnTop"})
    {
        const auto migrated = CanonicalizeSettingsRoute(
            SettingsRoute::ForPage(SettingsPage::AppearanceWidgets, focus));
        Check(migrated.page == SettingsPage::WidgetBehavior && migrated.focusId == focus,
            "old appearance search results locate the migrated behavior control");
        Check(CanonicalizeSettingsRoute(migrated) == migrated,
            "behavior navigation stays stable across repeated controller publications");
    }

    const SettingsRoute desktopIcons = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::Desktop, "desktop.iconSize"));
    for (const auto focus : {"desktop.fanTitleFontSize", "desktop.titleOverflow"})
    {
        const auto iconText = CanonicalizeSettingsRoute(SettingsRoute::ForPage(SettingsPage::Desktop, focus));
        Check(iconText.page == SettingsPage::AppearanceDesktopIcons && iconText.focusId == focus,
            "legacy desktop title settings remain reachable after moving typography to Fonts and icons");
    }
    const SettingsRoute iconBeautification = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::Desktop, "desktop.iconBeautify.outlineColor"));
    const SettingsRoute desktopTabHeight = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::Desktop, "desktop.tabHeight"));
    const SettingsRoute categoryRules = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::Desktop, "desktop.categoryRules"));
    const SettingsRoute legacyCategoryLayout = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::DesktopCategories, "desktop.categoryLayout"));
    for (const auto sourcePage : {SettingsPage::Desktop, SettingsPage::DesktopPages,
             SettingsPage::AppearanceDesktopIcons, SettingsPage::AppearanceWidgets})
        for (const auto focus : {"desktop.spacing", "desktop.iconSpacing"})
        {
            const auto spacing = CanonicalizeSettingsRoute(SettingsRoute::ForPage(sourcePage, focus));
            Check(spacing.page == SettingsPage::AppearanceDesktopIcons && spacing.focusId == focus,
                "layout spacing links reach Global appearance while retaining their focus target");
        }
    const SettingsRoute desktopBehavior = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Desktop));
    const SettingsRoute pageNavigation = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::General, "general.pageNavigation.next"));
    Check(desktopIcons.page == SettingsPage::AppearanceDesktopIcons &&
            iconBeautification.page ==
                SettingsPage::AppearanceIconBeautification &&
            desktopTabHeight.page == SettingsPage::AppearanceDesktopIcons &&
            desktopTabHeight.focusId == "desktop.categoryLayout" &&
            categoryRules.page == SettingsPage::DesktopCategories &&
            legacyCategoryLayout.page == SettingsPage::AppearanceDesktopIcons &&
            pageNavigation.page == SettingsPage::DesktopPages &&
            desktopBehavior.page == SettingsPage::Desktop,
        "legacy focus aliases route appearance, page, category, and behavior tasks to distinct owners");

    struct RelocatedSetting { const char* focus; SettingsPage owner; };
    const RelocatedSetting relocated[]{
        {"personalization.font", SettingsPage::AppearanceDesktopIcons},
        {"personalization.tooltipFontSize", SettingsPage::AppearanceDesktopIcons},
        {"personalization.cornerRadius", SettingsPage::AppearanceDesktopIcons},
        {"personalization.barHeight", SettingsPage::AppearanceDesktopIcons},
        {"personalization.luaWidgetRowHeight", SettingsPage::AppearanceDesktopIcons},
        {"personalization.widgetTransformCursors", SettingsPage::WidgetBehavior},
        {"desktop.spacing", SettingsPage::AppearanceDesktopIcons},
        {"desktop.iconSpacing", SettingsPage::AppearanceDesktopIcons},
        {"desktop.scrollingIconSpacingY", SettingsPage::AppearanceDesktopIcons},
        {"desktop.listRowSpacing", SettingsPage::AppearanceDesktopIcons},
        {"personalization.quickNavigationTheme", SettingsPage::QuickNavigation},
        {"quickNav.layout.iconSize", SettingsPage::QuickNavigation},
        {"quickNav.color.searchBg", SettingsPage::QuickNavigation},
    };
    for (const auto& setting : relocated)
        for (const auto sourcePage : {SettingsPage::Personalization,
                 SettingsPage::AppearanceWidgets, SettingsPage::AppearanceTheme,
                 SettingsPage::Widgets, setting.owner})
        {
            auto source = SettingsRoute::ForPage(sourcePage, setting.focus);
            source.guideTopic = "appearance";
            const auto target = CanonicalizeSettingsRoute(source);
            const auto expected = sourcePage == SettingsPage::AppearanceTheme && setting.owner == SettingsPage::QuickNavigation
                ? SettingsPage::AppearanceTheme : setting.owner;
            Check(target.page == expected && target.focusId == setting.focus &&
                    target.guideTopic == source.guideTopic &&
                    CanonicalizeSettingsRoute(target) == target,
                "relocated global fields preserve their IDs and are canonical after one pass");
        }
    for (const auto sourcePage : {SettingsPage::Personalization, SettingsPage::Desktop,
             SettingsPage::DesktopCategories, SettingsPage::WidgetBehavior,
             SettingsPage::AppearanceWidgets, SettingsPage::AppearanceDesktopIcons})
        for (const auto focus : {"desktop.categoryLayout", "desktop.tabHeight", "personalization.tabHeight"})
        {
            auto source = SettingsRoute::ForPage(sourcePage, focus);
            source.guideTopic = "appearance";
            const auto target = CanonicalizeSettingsRoute(source);
            Check(target.page == SettingsPage::AppearanceDesktopIcons &&
                    target.focusId == "desktop.categoryLayout" &&
                    target.guideTopic == source.guideTopic &&
                    CanonicalizeSettingsRoute(target) == target,
                "shared height aliases preserve their canonical locator under Global appearance");
        }
    Check(CanonicalizeSettingsRoute(SettingsRoute::ForPage(SettingsPage::AppearanceWidgets)).page ==
            SettingsPage::AppearanceTheme,
        "a removed appearance leaf without a field opens the shared theme");
    auto object = SettingsRoute::ForWidget(L"clock", "personalization.font");
    Check(CanonicalizeSettingsRoute(object) == object,
        "a widget field with a global-looking ID remains scoped to its instance");
    object = SettingsRoute::ForPage(SettingsPage::LargeIcon, "desktop.spacing");
    object.itemKey = L"object-item";
    Check(CanonicalizeSettingsRoute(object) == object,
        "global aliases cannot redirect an object editor out of its object scope");
}

void TestReadOnlySearchNavigationAndPackageScope()
{
    auto store = std::make_shared<FakeStore>();
    store->loaded.personalization.backgroundPreset = 1;
    store->loaded.desktop.iconBeautify.enabled = false;
    store->loaded.dock.showOnlyWhenSummoned = false;
    FakeHostActions host;
    SettingsController controller(store, &host);
    Check(controller.Initialize().Succeeded(), "read-only navigation test initializes existing values");
    const auto before = controller.Snapshot();
    const auto values = settings_ipc::Pack(before->values);
    const auto domainRevisions = settings_ipc::Pack(before->domainRevisions);
    SettingsSearchIndexInput catalog;
    PopulateSettingsSearchCatalog(catalog, [](std::string_view key) {
        return std::wstring(key.begin(), key.end());
    }, false, false);
    for (const auto focus : {"dock.showOnlyWhenSummoned", "desktop.spacing",
             "desktop.iconBeautify.outlineColor", "personalization.backgroundColor"})
    {
        const auto entry = std::find_if(catalog.staticSettings.begin(), catalog.staticSettings.end(),
            [&](const auto& descriptor) { return descriptor.focusId == focus; });
        Check(entry != catalog.staticSettings.end(), "a user task has a catalog destination");
        if (entry == catalog.staticSettings.end()) continue;
        const auto route = SettingsRoute::ForPage(entry->page, entry->focusId);
        Check(controller.Open(route).Succeeded() && host.route == route,
            "opening a search destination dispatches navigation only");
        const auto current = controller.Snapshot();
        Check(settings_ipc::Pack(current->values) == values &&
                settings_ipc::Pack(current->domainRevisions) == domainRevisions &&
                current->dirtyDomains == SettingsDomain::None &&
                current->pendingPreviewDomains == SettingsDomain::None &&
                current->pendingCommitDomains == SettingsDomain::None,
            "locating a conditional setting does not turn on its parent, select custom, or change stored values");
    }
    auto permissions = SettingsRoute::ForPage(SettingsPage::Widgets, "widgets.permissions");
    permissions.itemKey = L"org.snowdesktop.天气包";
    Check(permissions.IsValid() && controller.Open(permissions).Succeeded() && host.route == permissions &&
            CanonicalizeSettingsRoute(permissions) == permissions,
        "the controller forwards the exact permission package key without changing its scope");
    const auto routeRevision = controller.Snapshot()->revision;
    const auto routeCount = host.routeCount;
    for (const auto page : {SettingsPage::General, SettingsPage::WidgetSettings, SettingsPage::Widgets})
    {
        auto invalid = permissions;
        invalid.page = page;
        if (page == SettingsPage::Widgets) invalid.focusId = "widgets.installed";
        if (page == SettingsPage::WidgetSettings) invalid.widgetInstanceId = L"weather-instance";
        Check(!invalid.IsValid() && !controller.Open(invalid).Succeeded(),
            "package keys are rejected outside the exact permissions destination");
    }
    auto invalid = permissions;
    invalid.widgetInstanceId = L"weather-instance";
    Check(!invalid.IsValid() && !controller.Open(invalid).Succeeded() &&
            controller.Snapshot()->route == permissions && controller.Snapshot()->revision == routeRevision &&
            host.routeCount == routeCount,
        "invalid package routes do not publish a replacement or reach the host");
    Check(controller.CloseSession().Succeeded() && host.previewCount == 0 && host.commitCount == 0 &&
            store->personalizationSaveCount == 0 && store->dockSaveCount == 0 &&
            store->navigationSaveCount == 0 && store->generalSaveCount == 0 && store->categorySaveCount == 0,
        "search navigation and closing an unchanged session never preview, commit, or save configuration");
}

void TestLoadRouteAndImmutableSnapshots()
{
    auto store = std::make_shared<FakeStore>();
    store->loaded.general.demoModeEnabled = true;
    strcpy_s(store->loaded.general.language, "zh-CN");
    FakeHostActions host;
    SettingsController controller(store, &host);

    const auto beforeInitialize = controller.Snapshot();
    Check(!beforeInitialize->initialized &&
            beforeInitialize->revision == 0,
        "controller publishes an immutable pre-initialize snapshot");

    Check(controller.Initialize().Succeeded(),
        "controller initializes through the injected store");
    const auto initialized = controller.Snapshot();
    Check(initialized->initialized &&
            initialized->values.general.demoModeEnabled &&
            std::strcmp(initialized->values.general.language, "zh-CN") == 0 &&
            store->loadCount == 1,
        "initialize publishes loaded settings");

    const auto openResult = controller.Open(SettingsRoute::ForPage(
        SettingsPage::General, "general.language"));
    const std::uint64_t openGeneration = controller.Generation();
    Check(openResult.Succeeded() &&
            controller.Snapshot()->sessionActive &&
            controller.Snapshot()->route.page == SettingsPage::General &&
            host.routeCount == 1 &&
            host.route.focusId == "general.language" &&
            controller.IsGenerationCurrent(openGeneration),
        "open activates a session and forwards its strongly typed route");
    Check(!controller.Open(SettingsRoute::ForWidget(L"")).Succeeded(),
        "controller rejects an invalid route without replacing the current one");

    GeneralSettings changed = controller.Snapshot()->values.general;
    changed.demoModeEnabled = false;
    controller.UpdateGeneral(changed, SettingsUpdateMode::Draft);
    Check(initialized->values.general.demoModeEnabled &&
            initialized->revision < controller.Snapshot()->revision,
        "previous snapshots remain unchanged after an edit");

    Check(controller.CloseSession().Succeeded() &&
            !controller.IsGenerationCurrent(openGeneration) &&
            !controller.Snapshot()->sessionActive,
        "closing flushes changes and invalidates asynchronous generation tokens");
}

void TestDomainRevisionsTrackChangedDomain()
{
    auto store = std::make_shared<FakeStore>();
    SettingsController controller(store);
    (void)controller.Initialize();

    const auto initialized = controller.Snapshot();
    const SettingsDomainRevisions baseline = initialized->domainRevisions;
    Check(baseline.personalization == initialized->revision &&
            baseline.dock == initialized->revision &&
            baseline.systemTaskbar == initialized->revision &&
            baseline.navigation == initialized->revision &&
            baseline.general == initialized->revision &&
            baseline.category == initialized->revision &&
            baseline.desktop == initialized->revision,
        "initial snapshot publishes a common revision for every domain");

    (void)controller.Open(SettingsRoute::ForPage(SettingsPage::General));
    const auto opened = controller.Snapshot();
    Check(opened->revision > initialized->revision &&
            opened->domainRevisions.general == baseline.general &&
            opened->domainRevisions.desktop == baseline.desktop,
        "non-domain controller transitions do not advance domain revisions");

    GeneralSettings general = opened->values.general;
    general.demoModeEnabled = !general.demoModeEnabled;
    controller.UpdateGeneral(general, SettingsUpdateMode::Draft);
    const auto generalChanged = controller.Snapshot();
    Check(generalChanged->domainRevisions.general ==
                generalChanged->revision &&
            generalChanged->domainRevisions.general > baseline.general &&
            generalChanged->domainRevisions.personalization ==
                baseline.personalization &&
            generalChanged->domainRevisions.dock == baseline.dock &&
            generalChanged->domainRevisions.systemTaskbar ==
                baseline.systemTaskbar &&
            generalChanged->domainRevisions.navigation ==
                baseline.navigation &&
            generalChanged->domainRevisions.category == baseline.category &&
            generalChanged->domainRevisions.desktop == baseline.desktop,
        "a general edit advances only the general domain revision");
    Check(opened->domainRevisions.general == baseline.general,
        "previous snapshots retain their immutable domain revisions");

    DesktopDisplaySettings desktop = generalChanged->values.desktop;
    desktop.dockEnabled = !desktop.dockEnabled;
    Check(controller.SynchronizeDesktop(desktop),
        "a clean desktop domain accepts external synchronization");
    const auto desktopChanged = controller.Snapshot();
    Check(desktopChanged->domainRevisions.desktop ==
                desktopChanged->revision &&
            desktopChanged->domainRevisions.desktop > baseline.desktop &&
            desktopChanged->domainRevisions.general ==
                generalChanged->domainRevisions.general,
        "desktop synchronization advances desktop without changing general");
}

void TestDesktopStylePresetScope()
{
    struct Expected
    {
        const wchar_t* key;
        bool dock, bar, suppress, autoHide, backdrop, summoned, reserve, overlap, swipe;
        bool preserveOrientation;
        DockPosition position;
        bool attached;
        DockPosition barPosition;
    };
    const Expected cases[] = {
        {L"native", false, false, false, false, true, false, false, true, false,
            true, DockPosition::Right, false, DockPosition::Bottom},
        {L"taskbar-dock", true, false, false, true, false, false, false, true, true,
            false, DockPosition::Bottom, false, DockPosition::Bottom},
        {L"island", true, true, true, true, false, false, false, false, false,
            false, DockPosition::Bottom, false, DockPosition::Top},
        {L"merged", true, true, true, true, false, false, false, false, false,
            false, DockPosition::Bottom, true, DockPosition::Bottom},
        {L"side", true, true, true, true, false, false, true, false, false,
            false, DockPosition::Left, true, DockPosition::Top},
    };
    for (const bool attached : {false, true})
    {
        GeneralSettings originalGeneral;
        originalGeneral.dockEnabled = originalGeneral.statusBar.enabled = true;
        originalGeneral.statusBar.position = DockPosition::Bottom;
        originalGeneral.statusBar.monitorScope = DockMonitorScope::Last;
        originalGeneral.demoModeEnabled = true;
        originalGeneral.animationMode = 2;
        originalGeneral.popupAnimationEffect = 3;
        DockSettings originalDock;
        originalDock.position = attached ? DockPosition::Top : DockPosition::Right;
        originalDock.edgeAttached = attached;
        originalDock.monitorScope = DockMonitorScope::All;
        originalDock.allowDesktopContentOverlap = originalDock.reserveScreenSpace = true;
        originalDock.suppressSystemTaskbar = true;
        originalDock.floatingEdgeSwipeEnabled = false;
        originalDock.hoverEffect = originalDock.launchEffect = originalDock.windowEffect = 0;
        originalDock.hoverScale = 1.75f;
        for (const auto& expected : cases)
        {
            auto general = originalGeneral, wantedGeneral = originalGeneral;
            auto dock = originalDock, wantedDock = originalDock;
            wantedGeneral.dockEnabled = expected.dock;
            wantedGeneral.statusBar.enabled = expected.bar;
            wantedGeneral.statusBar.position = expected.barPosition;
            if (expected.bar) wantedGeneral.statusBar.monitorScope = DockMonitorScope::All;
            wantedDock.suppressSystemTaskbar = expected.suppress;
            wantedDock.systemTaskbarAutoHide = expected.autoHide;
            wantedDock.systemTaskbarBackdropEnabled = expected.backdrop;
            wantedDock.showOnlyWhenSummoned = expected.summoned;
            wantedDock.reserveScreenSpace = expected.reserve;
            wantedDock.allowDesktopContentOverlap = expected.overlap;
            wantedDock.floatingEdgeSwipeEnabled = expected.swipe;
            if (!expected.preserveOrientation)
            { wantedDock.position = expected.position; wantedDock.edgeAttached = expected.attached; }
            Check(ApplyDesktopStylePreset(expected.key, general, dock) && dock == wantedDock &&
                    settings_ipc::Pack(general) == settings_ipc::Pack(wantedGeneral),
                "each desktop style changes only its advertised layout fields and preserves animation preferences");
        }
        for (const auto* key : {L"island", L"merged", L"side"})
        {
            auto dock = originalDock, wanted = originalDock;
            const bool island = std::wstring_view(key) == L"island";
            wanted.hoverEffect = island ? 2 : 1;
            wanted.launchEffect = island ? 1 : 2;
            Check(ApplyDesktopStyleAnimations(key, dock) && dock == wanted,
                "recommended animation changes only hover and launch, retaining layout, scale and window effects");
            Check(NeedsDesktopStyleAnimations(key, originalDock) && !NeedsDesktopStyleAnimations(key, dock),
                "matching hover and launch effects do not repeat the animation recommendation dialog");
            auto changedHover = dock, changedLaunch = dock;
            changedHover.hoverEffect = 0;
            changedLaunch.launchEffect = 0;
            Check(NeedsDesktopStyleAnimations(key, changedHover) && NeedsDesktopStyleAnimations(key, changedLaunch),
                "changing either recommended effect makes the animation choice meaningful again");
            dock.hoverScale = 1.25f;
            dock.windowEffect = 4;
            Check(!NeedsDesktopStyleAnimations(key, dock),
                "unrelated magnification scale and window effects do not cause an animation prompt");
        }
        for (const auto* key : {L"native", L"taskbar-dock", L"unknown"})
        {
            auto dock = originalDock;
            Check(!ApplyDesktopStyleAnimations(key, dock) && dock == originalDock,
                "styles without an animation recommendation reject it without changing preferences");
            Check(!NeedsDesktopStyleAnimations(key, dock),
                "a style without recommended effects never requests animation confirmation");
        }
        auto general = originalGeneral;
        auto dock = originalDock;
        Check(!ApplyDesktopStylePreset(L"unknown", general, dock) && dock == originalDock &&
                settings_ipc::Pack(general) == settings_ipc::Pack(originalGeneral),
            "unknown layout presets leave both domains unchanged");
    }
}

void TestDesktopStyleQueuedCommit()
{
    auto store = std::make_shared<FakeStore>();
    store->loaded.general.dockEnabled = true;
    store->loaded.dock.position = DockPosition::Right;
    store->loaded.dock.reserveScreenSpace = true;
    store->loaded.dock.showOnlyWhenSummoned = true;
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    int scheduled = 0;
    controller.SetPendingWorkCallback([&] { ++scheduled; });
    SettingsHostActions::Request request;
    request.action = SettingsHostActions::Action::ApplyDesktopStylePreset;
    request.value = L"native";
    const auto original = controller.Snapshot();
    Check(controller.InvokeHostAction(request).status == SettingsActionStatus::Busy &&
            controller.Snapshot()->revision == original->revision &&
            controller.Snapshot()->dirtyDomains == SettingsDomain::None && scheduled == 0 &&
            settings_ipc::Pack(controller.Snapshot()->values.general) == settings_ipc::Pack(original->values.general) &&
            controller.Snapshot()->values.dock == original->values.dock,
        "disabling an enabled Dock requires confirmation before either preset domain changes");

    request.value = L"merged";
    const auto domains = SettingsDomain::General | SettingsDomain::Dock;
    Check(controller.InvokeHostAction(request).Succeeded(), "a valid combined layout preset is accepted for dispatch");
    const auto queued = controller.Snapshot();
    Check(queued->dirtyDomains == domains && queued->pendingCommitDomains == domains &&
            queued->pendingPreviewDomains == SettingsDomain::None && scheduled == 1 &&
            !queued->values.dock.reserveScreenSpace && !queued->values.dock.showOnlyWhenSummoned &&
            host.commitCount == 0 && host.invokeCount == 0 && host.previewCount == 0 &&
            store->generalSaveCount == 0 && store->dockSaveCount == 0,
        "switching from a reserved summon-only Dock queues the visible non-reserving layout in one joint commit");
    Check(queued->revision == original->revision + 1 &&
            queued->domainRevisions.general == queued->revision &&
            queued->domainRevisions.dock == queued->revision &&
            queued->domainRevisions.systemTaskbar == queued->revision &&
            queued->domainRevisions.personalization == original->domainRevisions.personalization &&
            queued->domainRevisions.navigation == original->domainRevisions.navigation &&
            queued->domainRevisions.category == original->domainRevisions.category &&
            queued->domainRevisions.desktop == original->domainRevisions.desktop,
        "combined preset updates both domain revisions and the Dock system-state guard in one publication");
    Check(controller.FlushPending().Succeeded() && host.commitCount == 1 && host.commitDomains == domains &&
            host.committedDock == queued->values.dock &&
            settings_ipc::Pack(host.committedGeneral) == settings_ipc::Pack(queued->values.general) &&
            store->dockSaveCount == 1 && store->generalSaveCount == 1 &&
            controller.Snapshot()->dirtyDomains == SettingsDomain::None,
        "flush delivers both new mirrors to one host commit and persists each domain once");
    Check(controller.FlushPending().Succeeded() && host.commitCount == 1 &&
            store->dockSaveCount == 1 && store->generalSaveCount == 1,
        "a settled joint preset has no duplicate host or store work");

    request.value = L"native";
    request.boolValue = true;
    Check(controller.InvokeHostAction(request).Succeeded() &&
            !controller.Snapshot()->values.general.dockEnabled &&
            !controller.Snapshot()->values.general.statusBar.enabled &&
            controller.Snapshot()->pendingCommitDomains == domains && host.commitCount == 1,
        "confirmed native layout queues the Dock and status-bar disable together");
    Check(controller.FlushPending().Succeeded(), "confirmed native layout can be committed");
    const auto beforeAnimation = controller.Snapshot();
    host.commitDomains = SettingsDomain::None;
    request.action = SettingsHostActions::Action::ApplyDesktopStyleAnimations;
    request.value = L"island";
    Check(controller.InvokeHostAction(request).Succeeded(), "an animation recommendation can be queued separately");
    const auto animation = controller.Snapshot();
    auto wantedDock = beforeAnimation->values.dock;
    wantedDock.hoverEffect = 2; wantedDock.launchEffect = 1;
    Check(animation->pendingCommitDomains == SettingsDomain::Dock && animation->values.dock == wantedDock &&
            settings_ipc::Pack(animation->values.general) == settings_ipc::Pack(beforeAnimation->values.general) &&
            animation->domainRevisions.general == beforeAnimation->domainRevisions.general &&
            animation->domainRevisions.dock == animation->revision &&
            animation->domainRevisions.systemTaskbar == animation->revision,
        "animation-only actions preserve General and layout while advancing the Dock guards");
    Check(controller.FlushPending().Succeeded() && host.commitCount == 3 &&
            host.commitDomains == SettingsDomain::Dock && store->dockSaveCount == 3 && store->generalSaveCount == 2,
        "animation-only flush does not run or persist a General commit");
    auto preview = controller.Snapshot()->values.dock;
    preview.thicknessScale = 1.3f;
    controller.UpdateDock(preview, SettingsUpdateMode::Preview);
    request.action = SettingsHostActions::Action::ApplyDesktopStylePreset;
    request.value = L"side";
    const auto previews = host.previewCount;
    Check(controller.InvokeHostAction(request).Succeeded() && controller.FlushPending().Succeeded() &&
        host.previewCount == previews && host.commitDomains == domains,
        "a layout preset absorbs old slider previews before its joint commit to avoid a mixed layout");
    for (const auto position : {DockPosition::Bottom, DockPosition::Top, DockPosition::Left, DockPosition::Right})
    {
        for (const bool attached : {false, true})
        {
            request.value = L"taskbar-dock";
            request.desktopStyleDockPosition = position;
            request.desktopStyleDockAttached = attached;
            const auto before = controller.Snapshot();
            const int commits = host.commitCount;
            Check(controller.InvokeHostAction(request).Succeeded() &&
                controller.Snapshot()->values.dock.position == position &&
                controller.Snapshot()->values.dock.edgeAttached == attached &&
                controller.Snapshot()->values.dock.hoverEffect == before->values.dock.hoverEffect &&
                !controller.Snapshot()->values.dock.reserveScreenSpace &&
                !controller.Snapshot()->values.dock.showOnlyWhenSummoned &&
                !controller.Snapshot()->values.general.statusBar.enabled &&
                controller.FlushPending().Succeeded() && host.commitCount == commits + 1,
                "companion draft choices reach one layout commit without changing animations or reserving space");
        }
    }
    request.desktopStyleDockPosition = static_cast<DockPosition>(99);
    const auto validRevision = controller.Snapshot()->revision;
    Check(!controller.InvokeHostAction(request).Succeeded() && controller.Snapshot()->revision == validRevision,
        "invalid companion geometry cannot partially apply either preset domain");
}

void TestTypedHotkeyRequestTransport()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);

    SettingsHostActions::Request request;
    request.action =
        SettingsHostActions::Action::ProbeHotkeyAvailability;
    request.hotkeyTarget =
        SettingsHostActions::HotkeyTarget::DesktopPassthrough;
    request.modifiers = MOD_CONTROL | MOD_ALT;
    request.virtualKey = VK_OEM_3;

    Check(controller.InvokeHostAction(request).Succeeded() &&
            host.invokeCount == 1 &&
            host.lastRequest.action == request.action &&
            host.lastRequest.hotkeyTarget == request.hotkeyTarget &&
            host.lastRequest.modifiers == request.modifiers &&
            host.lastRequest.virtualKey == request.virtualKey,
        "controller transports a strongly typed hotkey probe request");
}

void TestPreviewCoalescingAndCommit()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    (void)controller.Open(
        SettingsRoute::ForPage(SettingsPage::Personalization));

    int scheduledWorkCount = 0;
    controller.SetPendingWorkCallback(
        [&scheduledWorkCount]() { ++scheduledWorkCount; });

    PersonalizationSettings appearance =
        controller.Snapshot()->values.personalization;
    appearance.widgetAlpha = 0.25f;
    controller.UpdatePersonalization(
        appearance, SettingsUpdateMode::Preview);
    appearance.widgetAlpha = 0.50f;
    controller.UpdatePersonalization(
        appearance, SettingsUpdateMode::Preview);

    Check(scheduledWorkCount == 1 && host.previewCount == 0,
        "continuous edits schedule one coalesced dispatcher callback");
    Check(controller.FlushPending().Succeeded() &&
            host.previewCount == 1 &&
            host.previewedAlpha == 0.50f &&
            store->personalizationSaveCount == 0 &&
            HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::Personalization),
        "flush previews only the latest value and keeps it dirty");

    appearance.widgetAlpha = 0.75f;
    controller.UpdatePersonalization(
        appearance, SettingsUpdateMode::PreviewAndCommit);
    Check(scheduledWorkCount == 2,
        "a final continuous value schedules another flush");
    Check(controller.FlushPending().Succeeded() &&
            host.previewCount == 2 &&
            host.commitCount == 1 &&
            HasSettingsDomain(
                host.commitDomains,
                SettingsDomain::Personalization) &&
            store->personalizationSaveCount == 1 &&
            !HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::Personalization),
        "final continuous value previews, persists, and clears dirty state");
}

void TestFailureRetryAndExplicitApply()
{
    auto store = std::make_shared<FakeStore>();
    SettingsController controller(store);
    (void)controller.Initialize();

    GeneralSettings general = controller.Snapshot()->values.general;
    general.doubleClickHideDesktop = true;
    store->failingDomains = SettingsDomain::General;
    controller.UpdateGeneral(general, SettingsUpdateMode::Commit);
    const SettingsActionResult failed = controller.FlushPending();
    Check(!failed.Succeeded() &&
            failed.status == SettingsActionStatus::Failed &&
            HasSettingsDomain(failed.failedDomains, SettingsDomain::General) &&
            HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::General) &&
            HasSettingsDomain(
                controller.Snapshot()->pendingCommitDomains,
                SettingsDomain::General),
        "persistence failure retains dirty and pending state for retry");

    store->failingDomains = SettingsDomain::None;
    Check(controller.FlushPending().Succeeded() &&
            store->generalSaveCount == 2 &&
            !HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::General),
        "pending persistence can be retried without recreating the edit");

    CategorySettings category = controller.Snapshot()->values.category;
    category.tabFontSize = 18.0f;
    const int normalizationsBeforeDraft = g_categoryNormalizationCount;
    controller.UpdateCategory(category, SettingsUpdateMode::Draft);
    Check(controller.FlushPending().Succeeded() &&
            store->categorySaveCount == 0 &&
            g_categoryNormalizationCount == normalizationsBeforeDraft,
        "category drafts stay verbatim and do not save before explicit apply");
    controller.RequestCommit(SettingsDomain::Category);
    Check(controller.FlushPending().Succeeded() &&
            store->categorySaveCount == 1 &&
            g_categoryNormalizationCount == normalizationsBeforeDraft + 1,
        "explicit apply normalizes once and persists a category draft");
}

void TestCategoryCreationAcknowledgesPersistedSnapshot()
{
    auto store = std::make_shared<FakeStore>();
    SettingsController controller(store);
    (void)controller.Initialize();
    (void)controller.Open(SettingsRoute::ForPage(SettingsPage::DesktopCategories));
    const auto generation = controller.Generation();
    const std::wstring createdId = L"custom-pending-persistence";
    Check(!winui::CanAcknowledgeCategoryCreation(*controller.Snapshot(), createdId, generation),
        "a clean snapshot without the newly created category cannot acknowledge it");

    // Observe the real controller's publications, including its optimistic
    // draft and save result. Only the persistence boundary injects failure.
    struct Observation
    {
        SettingsController::SnapshotPtr snapshot;
        bool canAcknowledge = false;
    };
    std::vector<Observation> observations;
    controller.SetSnapshotChangedCallback([&](auto snapshot) {
        const bool canAcknowledge = winui::CanAcknowledgeCategoryCreation(*snapshot, createdId, generation);
        observations.push_back({std::move(snapshot), canAcknowledge});
    });
    CategorySettings category = controller.Snapshot()->values.category;
    category.rules.push_back({createdId, L"待保存的分类 日本語", L"PDF DOCX"});
    store->failingDomains = SettingsDomain::Category;
    controller.UpdateCategory(std::move(category), SettingsUpdateMode::PreviewAndCommit);
    const auto draft = controller.Snapshot();
    Check(!observations.empty() && !observations.back().canAcknowledge &&
            store->categorySaveCount == 0,
        "publishing a new category before persistence cannot acknowledge the input form");
    const auto failed = controller.FlushPending();
    Check(!failed.Succeeded() && store->categorySaveCount == 1 &&
            controller.Snapshot()->retryRequired &&
            std::none_of(observations.begin(), observations.end(),
                [](const auto& observation) { return observation.canAcknowledge; }),
        "a failed category save never acknowledges creation in any intermediate snapshot");

    const auto failedSnapshot = controller.Snapshot();
    store->failingDomains = SettingsDomain::None;
    Check(controller.RetryPending(), "a failed category save can be retried without recreating the rule");
    Check(!observations.back().canAcknowledge,
        "requesting a retry still cannot acknowledge an unsaved category");
    const auto retried = controller.FlushPending();
    const auto saved = controller.Snapshot();
    Check(retried.Succeeded() && store->categorySaveCount == 2 &&
            !observations.empty() && observations.back().canAcknowledge,
        "the successful save publication acknowledges the same pending category");
    Check(saved->revision > failedSnapshot->revision &&
            saved->domainRevisions.category == failedSnapshot->domainRevisions.category &&
            saved->domainRevisions.category == draft->domainRevisions.category &&
            !HasSettingsDomain(saved->dirtyDomains, SettingsDomain::Category) &&
            !HasSettingsDomain(saved->pendingCommitDomains, SettingsDomain::Category),
        "acknowledgement observes save-result flags even when the category domain revision stays unchanged");
    Check(!winui::CanAcknowledgeCategoryCreation(*saved, L"custom-not-created", generation) &&
            !winui::CanAcknowledgeCategoryCreation(*saved, createdId, generation + 1),
        "saving cannot acknowledge a missing identity or a creation from a different session");

    GeneralSettings unrelated = saved->values.general;
    unrelated.demoModeEnabled = !unrelated.demoModeEnabled;
    controller.UpdateGeneral(std::move(unrelated), SettingsUpdateMode::Commit);
    Check(HasSettingsDomain(controller.Snapshot()->dirtyDomains, SettingsDomain::General) &&
            observations.back().canAcknowledge,
        "an unrelated dirty settings domain does not hide the saved category acknowledgement");
    controller.SetSnapshotChangedCallback({});
}

void TestDockEdgeDistancePreviewAndCommit()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    for (const float scale : {0.1f, 1.0f, 5.0f})
    {
        auto settings = controller.Snapshot()->values.dock;
        settings.edgeAttached = false;
        settings.edgeDistanceScale = scale;
        controller.UpdateDock(settings, SettingsUpdateMode::PreviewAndCommit);
        Check(controller.FlushPending().Succeeded() &&
                host.previewedDockEdgeDistanceScale == scale &&
                store->lastSavedDock.edgeDistanceScale == scale,
            "island distance previews and persists the entire 10%-500% range");
        settings.edgeAttached = true;
        controller.UpdateDock(settings, SettingsUpdateMode::Commit);
        Check(controller.FlushPending().Succeeded() &&
                store->lastSavedDock.edgeDistanceScale == scale,
            "switching to an edge-attached Dock retains the island distance");
    }
    auto invalid = controller.Snapshot()->values.dock;
    invalid.edgeDistanceScale = 0.0f;
    controller.UpdateDock(invalid, SettingsUpdateMode::Commit);
    Check(controller.FlushPending().Succeeded() &&
            store->lastSavedDock.edgeDistanceScale == 0.1f,
        "external distances below the minimum are normalized before persistence");
    invalid.edgeDistanceScale = 9.0f;
    controller.UpdateDock(invalid, SettingsUpdateMode::Commit);
    Check(controller.FlushPending().Succeeded() &&
            store->lastSavedDock.edgeDistanceScale == 5.0f,
        "external distances above the maximum are normalized before persistence");
    Check(ScaleDockEdgeDistance(20, 0.1f) == 2 &&
            ScaleDockEdgeDistance(20, 1.0f) == 20 &&
            ScaleDockEdgeDistance(20, 5.0f) == 100 &&
            ScaleDockEdgeDistance(0, 5.0f) == 0,
        "edge gaps scale relative to the original margin without inventing a zero-margin gap");
}

void TestReloadAndExternalSynchronization()
{
    auto store = std::make_shared<FakeStore>();
    SettingsController controller(store);
    (void)controller.Initialize();

    DockSettings edited = controller.Snapshot()->values.dock;
    edited.thicknessScale = 0.75f;
    controller.UpdateDock(edited, SettingsUpdateMode::Draft);

    DockSettings external = edited;
    external.thicknessScale = 0.60f;
    Check(!controller.SynchronizeDock(external) &&
            controller.Snapshot()->values.dock.thicknessScale == 0.75f,
        "external synchronization never overwrites a dirty domain");
    Check(controller.Reload().status == SettingsActionStatus::Busy,
        "default reload policy preserves pending edits");

    store->loaded.dock.thicknessScale = 0.55f;
    const std::uint64_t beforeReload = controller.Generation();
    Check(controller.Reload(
            SettingsReloadPolicy::DiscardPendingChanges).Succeeded() &&
            controller.Snapshot()->values.dock.thicknessScale == 0.55f &&
            controller.Snapshot()->dirtyDomains == SettingsDomain::None &&
            controller.Generation() != beforeReload,
        "explicit discard reloads persisted state and advances generation");

    external.thicknessScale = 0.65f;
    Check(controller.SynchronizeDock(external) &&
            controller.Snapshot()->values.dock.thicknessScale == 0.65f,
        "clean domains accept application-side synchronization without writes");
}

void TestSummonOnlyLinkedPreferencesRemainBaseValues()
{
    bool allCombinationsPreserved = true;
    for (int combination = 0; combination < 4; ++combination)
    {
        const bool originalOverlap = (combination & 1) != 0;
        const bool originalEdgeSwipe = (combination & 2) != 0;
        auto store = std::make_shared<FakeStore>();
        store->loaded.dock.allowDesktopContentOverlap = originalOverlap;
        store->loaded.dock.floatingEdgeSwipeEnabled = originalEdgeSwipe;
        SettingsController controller(store);
        const bool initialized = controller.Initialize().Succeeded();
        allCombinationsPreserved = allCombinationsPreserved && initialized;
        if (!initialized)
            continue;

        DockSettings enabled = controller.Snapshot()->values.dock;
        enabled.showOnlyWhenSummoned = true;
        controller.UpdateDock(enabled, SettingsUpdateMode::Commit);
        const bool enabledSaved = controller.FlushPending().Succeeded();
        allCombinationsPreserved = allCombinationsPreserved &&
            enabledSaved &&
            store->dockSaveCount == 1 &&
            store->lastSavedDock.showOnlyWhenSummoned &&
            store->lastSavedDock.allowDesktopContentOverlap ==
                originalOverlap &&
            store->lastSavedDock.floatingEdgeSwipeEnabled ==
                originalEdgeSwipe &&
            snowdesktop::dock_settings_rules::
                IsDesktopContentOverlapEnabled(
                    store->lastSavedDock.showOnlyWhenSummoned,
                    store->lastSavedDock.allowDesktopContentOverlap) &&
            snowdesktop::dock_settings_rules::
                IsFloatingEdgeSwipeEnabled(
                    store->lastSavedDock.showOnlyWhenSummoned,
                    store->lastSavedDock.floatingEdgeSwipeEnabled);

        DockSettings disabled = controller.Snapshot()->values.dock;
        disabled.showOnlyWhenSummoned = false;
        controller.UpdateDock(disabled, SettingsUpdateMode::Commit);
        const bool disabledSaved = controller.FlushPending().Succeeded();
        allCombinationsPreserved = allCombinationsPreserved &&
            disabledSaved &&
            store->dockSaveCount == 2 &&
            !store->lastSavedDock.showOnlyWhenSummoned &&
            store->lastSavedDock.allowDesktopContentOverlap ==
                originalOverlap &&
            store->lastSavedDock.floatingEdgeSwipeEnabled ==
                originalEdgeSwipe;
    }
    Check(allCombinationsPreserved,
        "summon-only display preserves every linked preference combination through snapshots and saves");
}

void TestSystemTaskbarFieldSynchronization()
{
    auto store = std::make_shared<FakeStore>();
    store->loaded.dock.systemTaskbarAutoHide = false;
    store->loaded.dock.systemTaskbarAlignment = 1;
    SettingsController controller(store);
    (void)controller.Initialize();

    DockSettings unrelatedDraft = controller.Snapshot()->values.dock;
    unrelatedDraft.thicknessScale = 0.75f;
    controller.UpdateDock(
        unrelatedDraft, SettingsUpdateMode::PreviewAndCommit);
    const auto beforeExternal = controller.Snapshot();
    Check(controller.SynchronizeSystemTaskbarState(true, false),
        "Windows taskbar state can be reconciled while Dock is dirty");
    const auto reconciled = controller.Snapshot();
    Check(reconciled->values.dock.thicknessScale == 0.75f &&
            reconciled->values.dock.systemTaskbarAutoHide &&
            reconciled->values.dock.systemTaskbarAlignment == 0 &&
            HasSettingsDomain(
                reconciled->dirtyDomains, SettingsDomain::Dock) &&
            HasSettingsDomain(
                reconciled->pendingPreviewDomains, SettingsDomain::Dock) &&
            HasSettingsDomain(
                reconciled->pendingCommitDomains, SettingsDomain::Dock),
        "field synchronization preserves unrelated Dock values and pending work");
    Check(reconciled->domainRevisions.dock ==
                beforeExternal->domainRevisions.dock &&
            reconciled->domainRevisions.systemTaskbar >
                beforeExternal->domainRevisions.systemTaskbar,
        "Windows taskbar reconciliation publishes only its focused revision");

    Check(controller.FlushPending().Succeeded() &&
            controller.Snapshot()->dirtyDomains == SettingsDomain::None,
        "a successful Dock save completes the unrelated draft");

    DockSettings taskbarDraft = controller.Snapshot()->values.dock;
    taskbarDraft.systemTaskbarAlignment = 1;
    controller.UpdateDock(taskbarDraft, SettingsUpdateMode::Commit);
    Check(controller.SynchronizeSystemTaskbarState(false, false) &&
            !controller.Snapshot()->values.dock.systemTaskbarAutoHide &&
            controller.Snapshot()->values.dock.systemTaskbarAlignment == 1,
        "an alignment edit protects only alignment while auto-hide still reconciles");

    store->failingDomains = SettingsDomain::Dock;
    Check(!controller.FlushPending().Succeeded(),
        "the injected Dock persistence failure is observable");
    Check(controller.SynchronizeSystemTaskbarState(true, false) &&
            controller.Snapshot()->values.dock.systemTaskbarAlignment == 1,
        "a failed save keeps the edited taskbar field protected");

    store->failingDomains = SettingsDomain::None;
    Check(controller.RetryPending() &&
            controller.FlushPending().Succeeded() &&
            controller.SynchronizeSystemTaskbarState(false, false) &&
            controller.Snapshot()->values.dock.systemTaskbarAlignment == 0,
        "a successful retry releases taskbar field protection");

    controller.PrepareForExternalDataReplacement();
    const auto terminal = controller.Snapshot();
    Check(!controller.SynchronizeSystemTaskbarState(true, true) &&
            controller.Snapshot()->revision == terminal->revision &&
            controller.Snapshot()->values.dock.systemTaskbarAlignment ==
                terminal->values.dock.systemTaskbarAlignment,
        "terminal data replacement rejects taskbar reconciliation");
}

void TestLoadFailureAndOpenRetry()
{
    auto store = std::make_shared<FakeStore>();
    store->failingLoadCount = 1;
    SettingsController controller(store);

    const SettingsActionResult firstOpen = controller.Open(
        SettingsRoute::ForPage(SettingsPage::General));
    Check(!firstOpen.Succeeded() &&
            !controller.Snapshot()->initialized &&
            !controller.Snapshot()->sessionActive &&
            store->loadCount == 1,
        "a failed first load does not create a false active session");

    Check(controller.Open(
            SettingsRoute::ForPage(SettingsPage::General)).Succeeded() &&
            controller.Snapshot()->initialized &&
            controller.Snapshot()->sessionActive &&
            store->loadCount == 2,
        "opening retries initialization after a transient load failure");

    GeneralSettings edited = controller.Snapshot()->values.general;
    edited.demoModeEnabled = true;
    controller.UpdateGeneral(edited, SettingsUpdateMode::Draft);
    store->failingLoadCount = 1;
    const std::uint64_t generation = controller.Generation();
    const SettingsActionResult reload = controller.Reload(
        SettingsReloadPolicy::DiscardPendingChanges);
    Check(!reload.Succeeded() &&
            controller.Snapshot()->values.general.demoModeEnabled &&
            HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::General) &&
            controller.Generation() == generation,
        "a failed discard reload preserves live edits and generation");
}

void TestHostFailureRetryAndCloseGuard()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    (void)controller.Open(SettingsRoute::ForPage(SettingsPage::General));

    GeneralSettings changed = controller.Snapshot()->values.general;
    changed.demoModeEnabled = true;
    host.nextCommitStatus = SettingsActionStatus::Failed;
    controller.UpdateGeneral(changed, SettingsUpdateMode::Commit);
    const SettingsActionResult failed = controller.FlushPending();
    Check(!failed.Succeeded() &&
            store->generalSaveCount == 0 &&
            controller.Snapshot()->sessionActive &&
            controller.Snapshot()->retryRequired &&
            HasSettingsDomain(
                controller.Snapshot()->pendingCommitDomains,
                SettingsDomain::General),
        "host commit failure keeps the value dirty without persisting it");
    Check(!controller.CloseSession().Succeeded() &&
            controller.Snapshot()->sessionActive,
        "a failed close flush leaves the session active for error handling");

    host.nextCommitStatus = SettingsActionStatus::Succeeded;
    Check(controller.RetryPending() &&
            controller.FlushPending().Succeeded() &&
            store->generalSaveCount == 1 &&
            controller.CloseSession().Succeeded() &&
            !controller.Snapshot()->sessionActive,
        "explicit retry commits host and store state before closing");
}

void TestPreviewFailureBlocksPersistence()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();

    PersonalizationSettings changed =
        controller.Snapshot()->values.personalization;
    changed.widgetAlpha = 0.2f;
    host.nextPreviewStatus = SettingsActionStatus::Failed;
    controller.UpdatePersonalization(
        changed, SettingsUpdateMode::PreviewAndCommit);
    const SettingsActionResult failed = controller.FlushPending();
    Check(!failed.Succeeded() &&
            store->personalizationSaveCount == 0 &&
            HasSettingsDomain(
                controller.Snapshot()->pendingPreviewDomains,
                SettingsDomain::Personalization) &&
            HasSettingsDomain(
                controller.Snapshot()->pendingCommitDomains,
                SettingsDomain::Personalization),
        "a failed live preview blocks persistence of the unapplied value");

    host.nextPreviewStatus = SettingsActionStatus::Succeeded;
    Check(controller.RetryPending() &&
            controller.FlushPending().Succeeded() &&
            store->personalizationSaveCount == 1,
        "retry previews and persists the same pending value");
}

void TestDesktopHostPersistenceBoundary()
{
    auto store = std::make_shared<FakeStore>();
    SettingsController withoutHost(store);
    (void)withoutHost.Initialize();
    DesktopDisplaySettings desktop =
        withoutHost.Snapshot()->values.desktop;
    desktop.dockEnabled = true;
    desktop.scrollingIconSpacingYScale = 5.0f;
    desktop.listRowSpacingScale = 4.5f;
    withoutHost.UpdateDesktop(desktop, SettingsUpdateMode::Commit);
    Check(!withoutHost.FlushPending().Succeeded() &&
            HasSettingsDomain(
                withoutHost.Snapshot()->dirtyDomains,
                SettingsDomain::Desktop),
        "layout-backed desktop values cannot report saved without a host");

    FakeHostActions host;
    SettingsController withHost(store, &host);
    (void)withHost.Initialize();
    withHost.UpdateDesktop(desktop, SettingsUpdateMode::Commit);
    Check(withHost.FlushPending().Succeeded() &&
            HasSettingsDomain(
                host.commitDomains,
                SettingsDomain::Desktop) &&
            !HasSettingsDomain(
                withHost.Snapshot()->dirtyDomains,
                SettingsDomain::Desktop),
        "layout-backed desktop values clear only after host persistence");
    Check(host.committedDesktop.scrollingIconSpacingYScale == 5.0f &&
            host.committedDesktop.listRowSpacingScale == 4.5f &&
            host.committedDesktop.iconSpacingScale == desktop.iconSpacingScale,
        "committing vertical content spacing above 200 percent preserves both values and the separate global spacing");
}

void TestReentrantCommitKeepsNewerValuePending()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();

    int scheduled = 0;
    controller.SetPendingWorkCallback([&scheduled]() { ++scheduled; });
    GeneralSettings first = controller.Snapshot()->values.general;
    first.demoModeEnabled = true;
    GeneralSettings second = first;
    second.doubleClickHideDesktop = true;
    host.duringCommit = [&controller, second]() mutable {
        controller.UpdateGeneral(second, SettingsUpdateMode::Commit);
    };

    controller.UpdateGeneral(first, SettingsUpdateMode::Commit);
    Check(controller.FlushPending().Succeeded() &&
            store->generalSaveCount == 1 &&
            !store->lastSavedGeneral.doubleClickHideDesktop &&
            HasSettingsDomain(
                controller.Snapshot()->pendingCommitDomains,
                SettingsDomain::General) &&
            scheduled == 2,
        "a same-domain edit during flush keeps the newer revision pending");

    host.duringCommit = {};
    Check(controller.FlushPending().Succeeded() &&
            store->generalSaveCount == 2 &&
            store->lastSavedGeneral.doubleClickHideDesktop &&
            !HasSettingsDomain(
                controller.Snapshot()->dirtyDomains,
                SettingsDomain::General),
        "the dispatcher retry persists the newer reentrant value");
}

void TestExternalReplacementDiscardsWithoutStorageIo()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    (void)controller.Open(SettingsRoute::ForPage(
        SettingsPage::BackupAndData));

    GeneralSettings changed = controller.Snapshot()->values.general;
    changed.demoModeEnabled = true;
    controller.UpdateGeneral(changed, SettingsUpdateMode::PreviewAndCommit);
    const int loadsBefore = store->loadCount;
    const std::uint64_t generationBefore = controller.Generation();

    controller.PrepareForExternalDataReplacement();
    const auto snapshot = controller.Snapshot();
    Check(store->loadCount == loadsBefore &&
            store->generalSaveCount == 0 &&
            snapshot->externalReplacementPending &&
            snapshot->dirtyDomains == SettingsDomain::None &&
            snapshot->pendingPreviewDomains == SettingsDomain::None &&
            snapshot->pendingCommitDomains == SettingsDomain::None &&
            controller.Generation() != generationBefore &&
            !controller.IsGenerationCurrent(controller.Generation()),
        "an external replacement discards dirty state without old-tree IO");

    const std::uint64_t terminalRevision = snapshot->revision;
    const std::uint64_t terminalGeneration = snapshot->generation;
    controller.PrepareForExternalDataReplacement();
    Check(controller.Snapshot()->revision == terminalRevision &&
            controller.Generation() == terminalGeneration,
        "publishing the external replacement terminal state is idempotent");

    int scheduled = 0;
    controller.SetPendingWorkCallback([&scheduled]() { ++scheduled; });
    controller.UpdatePersonalization(
        snapshot->values.personalization, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateDock(
        snapshot->values.dock, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateNavigation(
        snapshot->values.navigation, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateGeneral(
        snapshot->values.general, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateCategory(
        snapshot->values.category, SettingsUpdateMode::PreviewAndCommit);
    controller.UpdateDesktop(
        snapshot->values.desktop, SettingsUpdateMode::PreviewAndCommit);
    controller.RequestCommit(SettingsDomain::All);
    Check(controller.Snapshot()->revision == terminalRevision &&
            controller.Snapshot()->dirtyDomains == SettingsDomain::None &&
            scheduled == 0 &&
            !controller.RetryPending(),
        "ordinary edits and persistence scheduling stop in terminal state");

    Check(!controller.SynchronizeGeneral(snapshot->values.general) &&
            controller.Snapshot()->revision == terminalRevision,
        "external application synchronization cannot revive stale values");
    Check(controller.Reload(
                SettingsReloadPolicy::DiscardPendingChanges).status ==
                SettingsActionStatus::Busy &&
            store->loadCount == loadsBefore,
        "reload cannot read a partly replaced data tree");
    const int routesBefore = host.routeCount;
    Check(controller.Open(SettingsRoute::ForPage(SettingsPage::About)).status ==
                SettingsActionStatus::Busy &&
            host.routeCount == routesBefore &&
            controller.Snapshot()->route.page == SettingsPage::BackupAndData,
        "open cannot leave the terminal replacement route");
    Check(controller.Initialize().status == SettingsActionStatus::Busy &&
            controller.Snapshot()->externalReplacementPending,
        "reinitializing the same controller cannot clear the terminal marker");

    Check(controller.FlushPending().Succeeded() &&
            controller.FlushAll().Succeeded() &&
            host.previewCount == 0 && host.commitCount == 0 &&
            store->generalSaveCount == 0,
        "flush operations become storage-free no-ops in terminal state");

    SettingsHostActions::Request request;
    request.action = SettingsHostActions::Action::RefreshDesktop;
    Check(controller.InvokeHostAction(request).status ==
                SettingsActionStatus::Busy &&
            host.invokeCount == 0,
        "ordinary host actions are rejected after data replacement");
    request.action = SettingsHostActions::Action::RestartApplication;
    Check(controller.InvokeHostAction(request).Succeeded() &&
            host.invokeCount == 1 &&
            host.lastRequest.action ==
                SettingsHostActions::Action::RestartApplication,
        "application restart remains retryable in terminal state");
    request.action = SettingsHostActions::Action::ExitApplication;
    Check(controller.InvokeHostAction(request).Succeeded() &&
            host.invokeCount == 2 &&
            host.lastRequest.action ==
                SettingsHostActions::Action::ExitApplication,
        "application exit remains available in terminal state");

    Check(controller.CloseSession().Succeeded() &&
            store->generalSaveCount == 0 &&
            !controller.Snapshot()->sessionActive &&
            controller.Snapshot()->externalReplacementPending &&
            controller.Open(SettingsRoute::ForPage(SettingsPage::Home)).status ==
                SettingsActionStatus::Busy,
        "closing after a queued replacement cannot flush the abandoned value");

    SettingsController freshController(std::make_shared<FakeStore>());
    Check(freshController.Initialize().Succeeded() &&
            !freshController.Snapshot()->externalReplacementPending,
        "a newly initialized process starts without the terminal marker");
}

void TestExternalReplacementDuringCommitPreventsLateSave()
{
    auto store = std::make_shared<FakeStore>();
    FakeHostActions host;
    SettingsController controller(store, &host);
    (void)controller.Initialize();
    (void)controller.Open(SettingsRoute::ForPage(SettingsPage::BackupAndData));

    GeneralSettings changed = controller.Snapshot()->values.general;
    changed.demoModeEnabled = true;
    host.duringCommit = [&controller]() {
        controller.PrepareForExternalDataReplacement();
    };
    controller.UpdateGeneral(changed, SettingsUpdateMode::Commit);

    Check(controller.FlushPending().Succeeded() &&
            controller.Snapshot()->externalReplacementPending &&
            store->generalSaveCount == 0,
        "a replacement published by a host commit prevents its captured value "
        "from being saved afterward");
}

void TestRemoteSaveFailureKeepsSessionOpen()
{
    using namespace snowdesktop::settings_ipc;
    auto store = std::make_shared<FakeStore>();
    SettingsController controller(store);
    HANDLE hostRead = nullptr, uiWrite = nullptr, uiRead = nullptr, hostWrite = nullptr;
    Check(CreatePipe(&hostRead, &uiWrite, nullptr, 0) &&
        CreatePipe(&uiRead, &hostWrite, nullptr, 0), "create remote controller test pipes");
    const auto processHandle = [] {
        HANDLE handle = nullptr;
        (void)DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
            &handle, SYNCHRONIZE, FALSE, 0);
        return handle;
    };
    Channel host;
    host.Open(hostRead, hostWrite, processHandle());
    BindController(host, controller);
    std::promise<DWORD> started;
    std::atomic<int> phaseReached = 0;
    std::thread peer([&] {
        Channel ui;
        ui.Open(uiRead, uiWrite, processHandle());
        std::unique_ptr<ISettingsController> proxy;
        ui.Bind<bool, int>("test.edit", [&](int phase) {
            phaseReached = 1;
            if (!proxy) proxy = CreateControllerProxy(ui);
            phaseReached = 2;
            if (phase == 0)
            {
                if (!proxy->Initialize().Succeeded() ||
                    !proxy->Open(SettingsRoute::ForPage(SettingsPage::General)).Succeeded()) return false;
                auto general = proxy->Snapshot()->values.general;
                phaseReached = 3;
                general.demoModeEnabled = true;
                proxy->UpdateGeneral(general, SettingsUpdateMode::Commit);
                phaseReached = 4;
                return !proxy->FlushAll().Succeeded() && !proxy->CloseSession().Succeeded() &&
                    proxy->Snapshot()->sessionActive &&
                    HasSettingsDomain(proxy->Snapshot()->dirtyDomains, SettingsDomain::General);
            }
            phaseReached = 5;
            return proxy->RetryPending() && proxy->CloseSession().Succeeded() &&
                !proxy->Snapshot()->sessionActive;
        });
        ui.Bind<bool>("test.stale", [&] {
            if (!proxy || !proxy->Open(SettingsRoute::ForPage(SettingsPage::General)).Succeeded())
                return false;
            const auto original = proxy->Snapshot();
            auto changed = original->values.general;
            changed.demoModeEnabled = !changed.demoModeEnabled;
            using Reply = std::pair<SettingsActionResult, ISettingsController::SnapshotPtr>;
            const auto badRevision = ui.Call<Reply>("controller.UpdateGeneral", original->generation,
                original->domainRevisions.general + 1, original->domainRevisions.systemTaskbar,
                changed, SettingsUpdateMode::Commit);
            const auto badGeneration = ui.Call<Reply>("controller.UpdateGeneral", original->generation + 1,
                original->domainRevisions.general, original->domainRevisions.systemTaskbar,
                changed, SettingsUpdateMode::Commit);
            const auto badTaskbarRevision = ui.Call<Reply>("controller.UpdateDock", original->generation,
                original->domainRevisions.dock, original->domainRevisions.systemTaskbar + 1,
                original->values.dock, SettingsUpdateMode::Commit);
            const bool unchanged = badRevision.second && badGeneration.second && badTaskbarRevision.second &&
                badRevision.second->values.general.demoModeEnabled == original->values.general.demoModeEnabled &&
                badGeneration.second->values.general.demoModeEnabled == original->values.general.demoModeEnabled &&
                badTaskbarRevision.second->domainRevisions.dock == original->domainRevisions.dock;
            return !badRevision.first.Succeeded() && !badGeneration.first.Succeeded() &&
                !badTaskbarRevision.first.Succeeded() && unchanged && proxy->CloseSession().Succeeded();
        });
        ui.Bind<bool>("test.readOnly", [&] {
            if (!proxy || !proxy->Open(SettingsRoute::ForPage(SettingsPage::General)).Succeeded())
                return false;
            int publications = 0;
            int depth = 0;
            int maximumDepth = 0;
            proxy->SetSnapshotChangedCallback([&](auto) {
                ++publications;
                maximumDepth = (std::max)(maximumDepth, ++depth);
                // Entering General probes shortcuts. A duplicate reply must
                // not publish and re-enter that same probe/render callback.
                if (depth < 3)
                {
                    SettingsHostActions::Request probe;
                    probe.action = SettingsHostActions::Action::ProbeHotkeyAvailability;
                    (void)proxy->InvokeHostAction(probe);
                }
                --depth;
            });
            const auto revision = proxy->Snapshot()->revision;
            SettingsHostActions::Request probe;
            probe.action = SettingsHostActions::Action::ProbeHotkeyAvailability;
            (void)proxy->InvokeHostAction(probe);
            const bool readOnly = publications == 0 && maximumDepth == 0 &&
                proxy->Snapshot()->revision == revision;
            (void)proxy->Open(SettingsRoute::ForPage(SettingsPage::Dock));
            const bool changed = publications == 1 && maximumDepth == 1 &&
                proxy->Snapshot()->route.page == SettingsPage::Dock;
            proxy->SetSnapshotChangedCallback({});
            return readOnly && changed && proxy->CloseSession().Succeeded();
        });
        ui.Bind<void>("test.quit", [] { PostQuitMessage(0); });
        started.set_value(GetCurrentThreadId());
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    });
    const DWORD thread = started.get_future().get();
    store->failingDomains = SettingsDomain::General;
    try
    {
        Check(host.Call<bool>("test.edit", 0),
            "remote save failure preserves dirty state and refuses close acknowledgement");
        store->failingDomains = SettingsDomain::None;
        Check(host.Call<bool>("test.edit", 1) && store->lastSavedGeneral.demoModeEnabled,
            "remote retry closes only after authoritative host persistence succeeds");
        Check(host.Call<bool>("test.stale"),
            "remote stale domain, generation and system taskbar edits cannot overwrite authoritative state");
        Check(host.Call<bool>("test.readOnly"),
            "read-only General probes do not republish or recursively render an unchanged IPC snapshot; route changes still publish once");
        host.Notify("test.quit");
    }
    catch (const std::exception& error)
    {
        std::cerr << "Remote save test phase " << phaseReached.load() << ": ";
        Check(false, error.what());
        PostThreadMessageW(thread, WM_QUIT, 0, 0);
    }
    peer.join();
    controller.SetSnapshotChangedCallback({});
    controller.SetPendingWorkCallback({});
}

} // namespace

int RunSettingsIpcTests();
int RunSettingsIpcChildIfRequested();

int main()
{
    const int childResult = RunSettingsIpcChildIfRequested();
    if (childResult >= 0) return childResult;
    failures += RunSettingsIpcTests();
    TestRoutes();
    TestReadOnlySearchNavigationAndPackageScope();
    {
        auto store = std::make_shared<FakeStore>();
        SettingsController controller(store);
        Check(controller.Open(SettingsRoute::ForPage(SettingsPage::RuntimeModules)).Succeeded() &&
            controller.Snapshot()->route == SettingsRoute::ForPage(SettingsPage::About, "about.showAdvancedSettings"),
            "direct hidden module route falls back to the About visibility control");
        auto settings = controller.Snapshot()->values.general;
        settings.showAdvancedSettings = true;
        controller.UpdateGeneral(settings, SettingsUpdateMode::PreviewAndCommit);
        Check(controller.Open(SettingsRoute::ForPage(SettingsPage::RuntimeModules)).Succeeded() && controller.Snapshot()->route.page == SettingsPage::RuntimeModules,
            "host and settings shell agree on visible module navigation");
        settings.showAdvancedSettings = false; settings.runtimeModules.allowed[0] = false;
        controller.UpdateGeneral(settings, SettingsUpdateMode::PreviewAndCommit);
        Check(controller.Snapshot()->route.page == SettingsPage::About && !controller.Snapshot()->values.general.runtimeModules.allowed[0],
            "closing advanced navigation preserves explicit module disables");
    }
    TestLoadRouteAndImmutableSnapshots();
    TestDomainRevisionsTrackChangedDomain();
    TestDesktopStylePresetScope();
    TestDesktopStyleQueuedCommit();
    TestTypedHotkeyRequestTransport();
    TestPreviewCoalescingAndCommit();
    TestFailureRetryAndExplicitApply();
    TestCategoryCreationAcknowledgesPersistedSnapshot();
    TestReloadAndExternalSynchronization();
    TestDockEdgeDistancePreviewAndCommit();
    TestSummonOnlyLinkedPreferencesRemainBaseValues();
    TestSystemTaskbarFieldSynchronization();
    TestLoadFailureAndOpenRetry();
    TestHostFailureRetryAndCloseGuard();
    TestPreviewFailureBlocksPersistence();
    TestDesktopHostPersistenceBoundary();
    TestReentrantCommitKeepsNewerValuePending();
    TestExternalReplacementDiscardsWithoutStorageIo();
    TestExternalReplacementDuringCommitPreventsLateSave();
    TestRemoteSaveFailureKeepsSessionOpen();

    if (failures == 0)
        std::cout << "All settings controller tests passed.\n";
    return failures == 0 ? 0 : 1;
}
