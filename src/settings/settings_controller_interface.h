#pragma once

#include "settings_domain.h"
#include "settings_fwd.h"
#include "settings_host_actions.h"

#include <cstdint>
#include <functional>
#include <memory>

namespace snowdesktop
{

/**
 * UI-independent settings state owner.
 *
 * The controller is main-thread affine. Background work may retain a
 * generation token, but it must marshal back to the main thread and call
 * IsGenerationCurrent before updating the controller.
 */
/** Internal value/service boundary used by the settings UI process. */
class ISettingsController
{
public:
    virtual ~ISettingsController() = default;
    using SnapshotPtr = std::shared_ptr<const SettingsSnapshot>;
    using SnapshotChangedCallback = std::function<void(SnapshotPtr)>;
    using PendingWorkCallback = std::function<void()>;
    virtual SettingsActionResult Initialize() = 0;
    virtual SettingsActionResult Reload(
        SettingsReloadPolicy policy =
            SettingsReloadPolicy::PreservePendingChanges) = 0;
    virtual SettingsActionResult Open(SettingsRoute route) = 0;
    virtual SettingsActionResult CloseSession() = 0;
    virtual void SetSnapshotChangedCallback(SnapshotChangedCallback callback) = 0;
    virtual void SetPendingWorkCallback(PendingWorkCallback callback) = 0;
    virtual SnapshotPtr Snapshot() const noexcept = 0;
    virtual std::uint64_t Generation() const noexcept = 0;
    virtual bool IsGenerationCurrent(
        std::uint64_t generation) const noexcept = 0;
    virtual void UpdatePersonalization(
        PersonalizationSettings settings,
        SettingsUpdateMode mode) = 0;
    virtual void UpdateDock(DockSettings settings, SettingsUpdateMode mode) = 0;
    virtual void UpdateNavigation(
        NavigationSettings settings,
        SettingsUpdateMode mode) = 0;
    virtual void UpdateGeneral(GeneralSettings settings, SettingsUpdateMode mode) = 0;
    virtual void UpdateCategory(CategorySettings settings, SettingsUpdateMode mode) = 0;
    virtual void UpdateDesktop(
        DesktopDisplaySettings settings,
        SettingsUpdateMode mode) = 0;
    virtual void RequestCommit(SettingsDomain domains) = 0;
    virtual SettingsActionResult FlushPending() = 0;
    virtual SettingsActionResult FlushAll() = 0;
    virtual bool RetryPending() = 0;
    virtual void PrepareForExternalDataReplacement() = 0;
    virtual SettingsActionResult InvokeHostAction(
        const SettingsHostActions::Request& request) = 0;
};

} // namespace snowdesktop
