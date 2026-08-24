#include "settings_controller.h"

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

void NormalizeCategorySettings(CategorySettings&)
{
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

    SettingsActionResult SaveDock(const DockSettings&) override
    {
        ++dockSaveCount;
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

    SettingsActionResult Invoke(const Request&) override
    {
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
    controller.UpdateCategory(category, SettingsUpdateMode::Draft);
    Check(controller.FlushPending().Succeeded() &&
            store->categorySaveCount == 0,
        "category drafts do not save before explicit apply");
    controller.RequestCommit(SettingsDomain::Category);
    Check(controller.FlushPending().Succeeded() &&
            store->categorySaveCount == 1,
        "explicit apply persists a category draft");
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

} // namespace

int main()
{
    TestRoutes();
    TestLoadRouteAndImmutableSnapshots();
    TestDomainRevisionsTrackChangedDomain();
    TestPreviewCoalescingAndCommit();
    TestFailureRetryAndExplicitApply();
    TestReloadAndExternalSynchronization();
    TestLoadFailureAndOpenRetry();
    TestHostFailureRetryAndCloseGuard();
    TestPreviewFailureBlocksPersistence();
    TestDesktopHostPersistenceBoundary();
    TestReentrantCommitKeepsNewerValuePending();

    if (failures == 0)
        std::cout << "All settings controller tests passed.\n";
    return failures == 0 ? 0 : 1;
}
