#include "settings_controller.h"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <utility>

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
    float previewedAlpha = 0.0f;
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
        if (nextPreviewStatus == SettingsActionStatus::Busy)
            return SettingsActionResult::Busy(L"preview busy");
        if (nextPreviewStatus == SettingsActionStatus::Failed)
            return SettingsActionResult::Failure(
                L"preview failed", domains);
        return SettingsActionResult::Success(domains);
    }

    SettingsActionResult OnSettingsCommitted(
        const SettingsSnapshot&,
        SettingsDomain domains) override
    {
        ++commitCount;
        commitDomains |= domains;
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

    constexpr std::array appearanceLeaves{
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

    const SettingsRoute legacyAppearance = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization));
    const SettingsRoute legacyTheme = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.contextMenu"));
    const SettingsRoute legacyWidgetAppearance = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.backgroundColor"));
    const SettingsRoute legacyTabHeight = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.tabHeight"));
    const SettingsRoute legacyCounts = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Personalization,
            "personalization.showCounts"));
    Check(legacyAppearance.page == SettingsPage::AppearanceTheme &&
            legacyTheme.page == SettingsPage::AppearanceTheme &&
            legacyWidgetAppearance.page == SettingsPage::AppearanceTheme &&
            legacyTabHeight.page == SettingsPage::AppearanceWidgets &&
            legacyTabHeight.focusId == "desktop.categoryLayout" &&
            legacyCounts.page == SettingsPage::DesktopCategories &&
            legacyCounts.focusId == "desktop.categoryCounts",
        "legacy Personalization routes resolve to their owned Appearance or Categories leaf");

    const SettingsRoute desktopIcons = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::Desktop, "desktop.iconSize"));
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
    const SettingsRoute desktopBehavior = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(SettingsPage::Desktop));
    const SettingsRoute pageNavigation = CanonicalizeSettingsRoute(
        SettingsRoute::ForPage(
            SettingsPage::General, "general.pageNavigation.next"));
    Check(desktopIcons.page == SettingsPage::AppearanceDesktopIcons &&
            iconBeautification.page ==
                SettingsPage::AppearanceIconBeautification &&
            desktopTabHeight.page == SettingsPage::AppearanceWidgets &&
            desktopTabHeight.focusId == "desktop.categoryLayout" &&
            categoryRules.page == SettingsPage::DesktopCategories &&
            legacyCategoryLayout.page == SettingsPage::AppearanceWidgets &&
            pageNavigation.page == SettingsPage::DesktopPages &&
            desktopBehavior.page == SettingsPage::Desktop,
        "legacy focus aliases route appearance, page, category, and behavior tasks to distinct owners");
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

} // namespace

int main()
{
    TestRoutes();
    TestLoadRouteAndImmutableSnapshots();
    TestDomainRevisionsTrackChangedDomain();
    TestTypedHotkeyRequestTransport();
    TestPreviewCoalescingAndCommit();
    TestFailureRetryAndExplicitApply();
    TestReloadAndExternalSynchronization();
    TestSummonOnlyLinkedPreferencesRemainBaseValues();
    TestSystemTaskbarFieldSynchronization();
    TestLoadFailureAndOpenRetry();
    TestHostFailureRetryAndCloseGuard();
    TestPreviewFailureBlocksPersistence();
    TestDesktopHostPersistenceBoundary();
    TestReentrantCommitKeepsNewerValuePending();
    TestExternalReplacementDiscardsWithoutStorageIo();
    TestExternalReplacementDuringCommitPreventsLateSave();

    if (failures == 0)
        std::cout << "All settings controller tests passed.\n";
    return failures == 0 ? 0 : 1;
}
