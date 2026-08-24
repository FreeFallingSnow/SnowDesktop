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

    /** Return the current settings top-level HWND at the instant of use. */
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
 * archive validation and migration staging run on one cooperative jthread.
 * Every presenter callback is checked against the exact published generation
 * and revision. A successful full restore/import/migration abandons dirty
 * controller state without reading or writing the old data tree and only then
 * requests an application restart; this class never flushes the pre-restore
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
     * Stop accepting actions and drain a completed replacement safely.
     * The settings host must close this backend before CloseSession so a
     * successfully queued replacement cannot be followed by an old flush.
     */
    void Close() noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace snowdesktop::winui
