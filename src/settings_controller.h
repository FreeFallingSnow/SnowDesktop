#pragma once

#include "category_settings.h"
#include "desktop_display_settings.h"
#include "dock_settings.h"
#include "general_settings.h"
#include "navigation_settings.h"
#include "personalization.h"
#include "settings_route.h"

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>

namespace snowdesktop
{

enum class SettingsDomain : std::uint32_t
{
    None = 0,
    Personalization = 1u << 0,
    Dock = 1u << 1,
    Navigation = 1u << 2,
    General = 1u << 3,
    Category = 1u << 4,
    Desktop = 1u << 5,
    NativeStored =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),
    All =
        (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
        (1u << 5),
};

[[nodiscard]] constexpr SettingsDomain operator|(
    SettingsDomain left,
    SettingsDomain right) noexcept
{
    return static_cast<SettingsDomain>(
        static_cast<std::uint32_t>(left) |
        static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr SettingsDomain operator&(
    SettingsDomain left,
    SettingsDomain right) noexcept
{
    return static_cast<SettingsDomain>(
        static_cast<std::uint32_t>(left) &
        static_cast<std::uint32_t>(right));
}

constexpr SettingsDomain& operator|=(
    SettingsDomain& left,
    SettingsDomain right) noexcept
{
    left = left | right;
    return left;
}

constexpr SettingsDomain& operator&=(
    SettingsDomain& left,
    SettingsDomain right) noexcept
{
    left = left & right;
    return left;
}

[[nodiscard]] constexpr SettingsDomain operator~(
    SettingsDomain value) noexcept
{
    return static_cast<SettingsDomain>(
        ~static_cast<std::uint32_t>(value));
}

[[nodiscard]] constexpr bool HasSettingsDomain(
    SettingsDomain domains,
    SettingsDomain domain) noexcept
{
    return (domains & domain) != SettingsDomain::None;
}

enum class SettingsActionStatus : std::uint8_t
{
    Succeeded,
    Busy,
    Failed,
};

struct SettingsActionResult
{
    SettingsActionStatus status = SettingsActionStatus::Succeeded;
    SettingsDomain completedDomains = SettingsDomain::None;
    SettingsDomain failedDomains = SettingsDomain::None;
    std::wstring message;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == SettingsActionStatus::Succeeded;
    }

    [[nodiscard]] static SettingsActionResult Success(
        SettingsDomain completed = SettingsDomain::None);
    [[nodiscard]] static SettingsActionResult Busy(std::wstring message);
    [[nodiscard]] static SettingsActionResult Failure(
        std::wstring message,
        SettingsDomain failed = SettingsDomain::None);
};

/** Existing persisted settings, independent of ImGui or WinUI. */
struct SettingsValues
{
    PersonalizationSettings personalization;
    DockSettings dock;
    NavigationSettings navigation;
    GeneralSettings general;
    CategorySettings category = CategorySettings::Defaults();
    DesktopDisplaySettings desktop;
};

/** Per-domain revisions used by views to update only changed presenters. */
struct SettingsDomainRevisions
{
    std::uint64_t personalization = 0;
    std::uint64_t dock = 0;
    std::uint64_t navigation = 0;
    std::uint64_t general = 0;
    std::uint64_t category = 0;
    std::uint64_t desktop = 0;
};

/**
 * Immutable value published to a settings ViewModel.
 *
 * revision changes for every observable controller transition. generation
 * changes whenever a view session ends or persisted state is reloaded, and is
 * the token asynchronous work must validate before applying its result.
 */
struct SettingsSnapshot
{
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    bool initialized = false;
    bool sessionActive = false;
    SettingsRoute route;
    SettingsValues values;
    SettingsDomainRevisions domainRevisions;
    SettingsDomain dirtyDomains = SettingsDomain::None;
    SettingsDomain pendingPreviewDomains = SettingsDomain::None;
    SettingsDomain pendingCommitDomains = SettingsDomain::None;
    bool retryRequired = false;
    /**
     * A complete data-tree replacement has been committed outside the
     * controller. The current process must not publish its stale in-memory
     * values again; only restart and exit host actions remain valid.
     */
    bool externalReplacementPending = false;
    std::wstring lastActionMessage;
};

/** Persistence seam used by tests and by the native JSON implementation. */
class SettingsStore
{
public:
    virtual ~SettingsStore() = default;

    virtual SettingsActionResult Load(SettingsValues& values) = 0;
    virtual SettingsActionResult SavePersonalization(
        const PersonalizationSettings& settings) = 0;
    virtual SettingsActionResult SaveDock(
        const DockSettings& settings) = 0;
    virtual SettingsActionResult SaveNavigation(
        const NavigationSettings& settings) = 0;
    virtual SettingsActionResult SaveGeneral(
        const GeneralSettings& settings) = 0;
    virtual SettingsActionResult SaveCategory(
        const CategorySettings& settings) = 0;
};

/** Creates a store backed by the existing SnowDesktop JSON files. */
[[nodiscard]] std::shared_ptr<SettingsStore> CreateNativeSettingsStore();

/**
 * Application-side adapter replacing per-control window callbacks.
 *
 * During migration, an adapter can forward these consolidated events to the
 * existing DesktopApp callbacks.  The WinUI backend consumes the same events.
 */
class SettingsHostActions
{
public:
    enum class HotkeyTarget : std::uint8_t
    {
        None,
        QuickNavigation,
        DesktopPassthrough,
        FloatingDock,
        PagePrevious,
        PageNext,
    };

    enum class Action : std::uint8_t
    {
        ApplyLanguage,
        RegisterHotkeys,
        ApplyDock,
        ApplyTaskbar,
        ApplyDesktopLayout,
        ApplyCategories,
        RefreshDesktop,
        RefreshWidgets,
        AddWidgetToDesktop,
        ReloadWidgetInstance,
        RestartExplorer,
        RestartApplication,
        ExitApplication,
        OpenDataDirectory,
        SetAutoStartEnabled,
        OpenStartupAppsSettings,
        CheckForUpdates,
        CancelUpdateCheck,
        OpenProject,
        OpenLicense,
        OpenThirdPartyNotices,
        SetAnimationDiagnostics,
        TriggerCrashTest,
        ProbeHotkeyAvailability,
    };

    struct Request
    {
        Action action = Action::RefreshDesktop;
        std::wstring widgetInstanceId;
        std::wstring value;
        bool boolValue = false;
        HotkeyTarget hotkeyTarget = HotkeyTarget::None;
        UINT modifiers = 0;
        UINT virtualKey = 0;
    };

    virtual ~SettingsHostActions() = default;

    virtual SettingsActionResult OnSettingsPreview(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) = 0;
    /** Apply host-owned state before the controller writes native stores. */
    virtual SettingsActionResult OnSettingsCommitted(
        const SettingsSnapshot& snapshot,
        SettingsDomain domains) = 0;
    virtual SettingsActionResult OnSettingsRouteChanged(
        const SettingsRoute& route) = 0;
    virtual SettingsActionResult Invoke(const Request& request) = 0;
};

/**
 * How an updated value should flow beyond the immutable snapshot.
 *
 * Draft updates only mark dirty. Preview schedules a coalesced host preview.
 * Commit schedules persistence without preview. PreviewAndCommit does both,
 * which is useful for the final value of a continuous control.
 */
enum class SettingsUpdateMode : std::uint8_t
{
    Draft = 0,
    Preview = 1u << 0,
    Commit = 1u << 1,
    PreviewAndCommit = (1u << 0) | (1u << 1),
};

enum class SettingsReloadPolicy : std::uint8_t
{
    PreservePendingChanges,
    DiscardPendingChanges,
};

/**
 * UI-independent settings state owner.
 *
 * The controller is main-thread affine. Background work may retain a
 * generation token, but it must marshal back to the main thread and call
 * IsGenerationCurrent before updating the controller.
 */
class SettingsController
{
public:
    using SnapshotPtr = std::shared_ptr<const SettingsSnapshot>;
    using SnapshotChangedCallback = std::function<void(SnapshotPtr)>;
    using PendingWorkCallback = std::function<void()>;

    explicit SettingsController(
        std::shared_ptr<SettingsStore> store,
        SettingsHostActions* hostActions = nullptr);

    SettingsController(const SettingsController&) = delete;
    SettingsController& operator=(const SettingsController&) = delete;

    [[nodiscard]] SettingsActionResult Initialize();
    [[nodiscard]] SettingsActionResult Reload(
        SettingsReloadPolicy policy =
            SettingsReloadPolicy::PreservePendingChanges);

    [[nodiscard]] SettingsActionResult Open(SettingsRoute route);
    [[nodiscard]] SettingsActionResult CloseSession();

    void SetHostActions(SettingsHostActions* hostActions) noexcept;
    void SetSnapshotChangedCallback(SnapshotChangedCallback callback);
    void SetPendingWorkCallback(PendingWorkCallback callback);

    [[nodiscard]] SnapshotPtr Snapshot() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] bool IsGenerationCurrent(
        std::uint64_t generation) const noexcept;

    void UpdatePersonalization(
        PersonalizationSettings settings,
        SettingsUpdateMode mode);
    void UpdateDock(DockSettings settings, SettingsUpdateMode mode);
    void UpdateNavigation(
        NavigationSettings settings,
        SettingsUpdateMode mode);
    void UpdateGeneral(GeneralSettings settings, SettingsUpdateMode mode);
    void UpdateCategory(CategorySettings settings, SettingsUpdateMode mode);
    void UpdateDesktop(
        DesktopDisplaySettings settings,
        SettingsUpdateMode mode);

    /** Mark dirty domains for persistence, e.g. an explicitly applied draft. */
    void RequestCommit(SettingsDomain domains);

    /** Dispatch coalesced previews and requested persistence work. */
    [[nodiscard]] SettingsActionResult FlushPending();
    /** Persist every dirty domain; used when a settings view closes. */
    [[nodiscard]] SettingsActionResult FlushAll();
    [[nodiscard]] bool RetryPending();

    /**
     * Abandon every in-memory edit before an already-queued external data
     * replacement requests process restart. This deliberately does not load
     * or save any file: once the replacement marker is published, reading the
     * old data tree or allowing CloseSession() to flush it is unsafe.
     */
    void PrepareForExternalDataReplacement();

    [[nodiscard]] SettingsActionResult InvokeHostAction(
        const SettingsHostActions::Request& request);

    // Reconcile changes made elsewhere in DesktopApp without creating writes.
    // A dirty domain is never overwritten and causes false to be returned.
    [[nodiscard]] bool SynchronizePersonalization(
        PersonalizationSettings settings);
    [[nodiscard]] bool SynchronizeDock(DockSettings settings);
    [[nodiscard]] bool SynchronizeNavigation(NavigationSettings settings);
    [[nodiscard]] bool SynchronizeGeneral(GeneralSettings settings);
    [[nodiscard]] bool SynchronizeCategory(CategorySettings settings);
    [[nodiscard]] bool SynchronizeDesktop(
        DesktopDisplaySettings settings);

private:
    [[nodiscard]] static bool LoadProducedUsableSnapshot(
        const SettingsActionResult& result) noexcept;
    void MarkChanged(SettingsDomain domain, SettingsUpdateMode mode);
    void PublishSnapshot();
    void SchedulePendingWorkIfNeeded(bool previouslyPending);
    [[nodiscard]] bool HasPendingWork() const noexcept;
    [[nodiscard]] SettingsActionResult SaveDomain(
        SettingsDomain domain,
        const SettingsValues& values);
    [[nodiscard]] static std::size_t DomainIndex(
        SettingsDomain domain) noexcept;
    [[nodiscard]] bool SynchronizeDomain(
        SettingsDomain domain,
        const std::function<void()>& assign);

    std::shared_ptr<SettingsStore> store_;
    SettingsHostActions* hostActions_ = nullptr;
    SettingsValues values_;
    SettingsRoute route_;
    SettingsDomain dirtyDomains_ = SettingsDomain::None;
    SettingsDomain pendingPreviewDomains_ = SettingsDomain::None;
    SettingsDomain pendingCommitDomains_ = SettingsDomain::None;
    std::array<std::uint64_t, 6> domainRevisions_{};
    std::uint64_t revision_ = 0;
    std::uint64_t generation_ = 0;
    bool initialized_ = false;
    bool sessionActive_ = false;
    bool flushing_ = false;
    bool retryRequired_ = false;
    bool externalReplacementPending_ = false;
    std::wstring lastActionMessage_;
    SnapshotPtr snapshot_;
    SnapshotChangedCallback snapshotChangedCallback_;
    PendingWorkCallback pendingWorkCallback_;
};

} // namespace snowdesktop
