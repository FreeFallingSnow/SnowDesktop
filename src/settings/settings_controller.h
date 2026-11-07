#pragma once

#include "settings_controller_interface.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace snowdesktop
{

class SettingsController : public ISettingsController
{
public:
    using SnapshotPtr = std::shared_ptr<const SettingsSnapshot>;
    using SnapshotChangedCallback = std::function<void(SnapshotPtr)>;
    using PendingWorkCallback = std::function<void()>;

    explicit SettingsController(
        std::shared_ptr<SettingsStore> store,
        SettingsHostActions* hostActions = nullptr);
    ~SettingsController() override;

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
    /**
     * Reconcile the Windows-owned auto-hide and alignment fields without
     * replacing unrelated Dock drafts. A field edited in the current settings
     * session remains protected until that Dock commit succeeds.
     */
    [[nodiscard]] bool SynchronizeSystemTaskbarState(
        bool autoHide,
        bool alignmentCentered);
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

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace snowdesktop
