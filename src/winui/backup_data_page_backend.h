#pragma once

#include "../settings_controller.h"
#include "backup_data_page_presenter.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

/** Validated documents handed back to DesktopApp for an STA-owned commit. */
struct LayoutRestorePayload
{
    std::string layoutDocument;
    std::optional<std::string> storageDocument;
};

/**
 * Host services used by BackupDataPageBackend.
 *
 * The backend owns storage work and request ordering, while the settings host
 * keeps ownership of XamlRoot dialogs, HWND-initialized pickers and UI-thread
 * dispatch.  postToUi must enqueue the callback instead of running it on a
 * worker thread.
 */
struct BackupDataPageBackendOptions
{
    using UiTask = std::function<void()>;
    using ConfirmationCompletion =
        BackupDataPageActions::ConfirmationCompletion;
    using PickerCompletion = BackupDataPageActions::PickerCompletion;

    /** Optional test/embedding overrides. Empty paths use data_paths. */
    std::filesystem::path stateRoot;
    std::filesystem::path dataDirectory;
    std::string hostVersion;
    std::string sourceType;

    /**
     * Return the current settings top-level HWND at the instant of use.
     * Besides picker ownership, the backend uses this as the lifetime boundary
     * for a detached replacement completion: a hidden window remains valid,
     * while permanent host shutdown must return null before destroying the
     * SettingsController.
     */
    std::function<HWND()> ownerWindow;

    /** Marshal a completion to the settings STA. False means enqueue failed. */
    std::function<bool(UiTask)> postToUi;

    /** Show a host-owned ContentDialog. */
    std::function<void(
        HWND owner,
        BackupDataConfirmationRequest request,
        ConfirmationCompletion completion)> confirm;

    /**
     * Show a host-owned file/folder picker initialized with owner.
     * The callback may arrive on any thread; the backend marshals it to the
     * settings STA before allowing the presenter to continue.
     */
    std::function<void(
        HWND owner,
        BackupDataPickerRequest request,
        PickerCompletion completion)> pickPath;

    /** Optional shell-launch seam. ShellExecuteW is used when omitted. */
    std::function<SettingsActionResult(
        HWND owner,
        const std::filesystem::path& path)> openPath;

    /**
     * Atomically replace the live layout on the application STA, reload the
     * desktop model and synchronize the SettingsController mirror. The worker
     * never writes application-owned live files directly.
     */
    std::function<SettingsActionResult(LayoutRestorePayload payload)>
        commitLayoutRestore;

    std::function<std::wstring(std::string_view key)> localize;
};

/**
 * Application-side adapter for BackupDataPagePresenter.
 *
 * All public methods are settings-STA affine. File enumeration, copying,
 * archive validation and migration staging run on one detached cooperative
 * jthread, so route deactivation and window close only request cancellation and
 * never join storage work on the STA. Every callback and ordinary completion
 * is checked against its exact generation, activation, revision and task.
 *
 * FullDataBackupManager's synchronous archive/queue calls do not expose a
 * cancellation token. Cancellation is observed immediately around those
 * non-interruptible calls, while their detached execution cannot block the
 * settings STA. Detached workers from a closed and reopened settings window
 * are process-serialized, and no later storage task may cross an already
 * queued replacement transaction. If a restore/import/migration atomically
 * queues an external replacement, its dirty-discard/restart transition
 * remains an application-lifecycle obligation after the page closes, but may
 * not publish stale view state. This class never flushes the pre-restore
 * snapshot.
 */
class BackupDataPageBackend final
{
public:
    using SnapshotChangedCallback =
        std::function<void(const BackupDataPageSnapshot&)>;

    BackupDataPageBackend(
        SettingsController& controller,
        BackupDataPageBackendOptions options);
    ~BackupDataPageBackend();

    BackupDataPageBackend(const BackupDataPageBackend&) = delete;
    BackupDataPageBackend& operator=(const BackupDataPageBackend&) = delete;
    BackupDataPageBackend(BackupDataPageBackend&&) = delete;
    BackupDataPageBackend& operator=(BackupDataPageBackend&&) = delete;

    /** Bind directly to BackupDataPagePresenter::SetActions. */
    [[nodiscard]] BackupDataPageActions Actions();

    void SetSnapshotChangedCallback(SnapshotChangedCallback callback);
    [[nodiscard]] BackupDataPageSnapshot CurrentSnapshot() const;

    /** Start or rebind the backend to the current SettingsController session. */
    void Activate(std::uint64_t generation);
    void Deactivate() noexcept;

    /** Re-enumerate layout and complete-data backups on the worker. */
    void Refresh();

    /**
     * Stop accepting actions without waiting for storage work on the STA.
     * A detached task retains only its copied work context plus guarded state
     * needed to finalize an already-queued replacement safely.
     * The settings host must close this backend before CloseSession so a
     * successfully queued replacement cannot be followed by an old flush.
     */
    void Close() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace snowdesktop::winui
