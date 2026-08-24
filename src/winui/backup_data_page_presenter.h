#pragma once

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::winui
{

/** Stable, host-validated identity and display data for one layout backup. */
struct LayoutBackupEntry
{
    std::wstring id;
    std::wstring displayName;
    std::wstring createdAt;
    bool hasStorageCompanion = false;

    friend bool operator==(const LayoutBackupEntry&, const LayoutBackupEntry&)
        = default;
};

/** Stable, host-validated identity and display data for one full backup. */
struct FullDataBackupEntry
{
    std::wstring id;
    std::wstring displayName;
    std::wstring createdAt;
    std::wstring sourceType;
    std::size_t fileCount = 0;
    std::uint64_t totalBytes = 0;
    bool migrationRollback = false;

    friend bool operator==(
        const FullDataBackupEntry&, const FullDataBackupEntry&) = default;
};

enum class BackupDataOperation : std::uint8_t
{
    None,
    CreateLayoutBackup,
    RestoreLayoutBackup,
    DeleteLayoutBackup,
    CreateFullBackup,
    ImportAndRestoreFullBackup,
    ExportFullBackup,
    RestoreFullBackup,
    DeleteFullBackup,
    MigrateData,
};

enum class BackupDataNoticeSeverity : std::uint8_t
{
    Informational,
    Success,
    Warning,
    Error,
};

struct BackupDataOperationState
{
    std::uint64_t requestId = 0;
    BackupDataOperation operation = BackupDataOperation::None;
    bool running = false;
    bool cancellable = false;
    bool indeterminate = true;
    double progress = 0.0;
    std::wstring message;
};

struct BackupDataNotice
{
    BackupDataNoticeSeverity severity =
        BackupDataNoticeSeverity::Informational;
    std::wstring title;
    std::wstring message;
};

/**
 * Immutable backup/data state published by the host on the settings thread.
 *
 * generation changes when a settings session closes or reloads. revision is
 * strictly increasing within one generation.  Long-running work publishes a
 * new snapshot rather than retaining access to the presenter's controls.
 */
struct BackupDataPageSnapshot
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool initialized = false;
    std::vector<LayoutBackupEntry> layoutBackups;
    std::vector<FullDataBackupEntry> fullBackups;
    std::wstring dataDirectory;
    std::wstring fullBackupDirectory;
    BackupDataOperationState operation;
    std::optional<BackupDataNotice> notice;
};

enum class BackupDataCommand : std::uint8_t
{
    CreateLayoutBackup,
    RestoreLayoutBackup,
    DeleteLayoutBackup,
    CreateFullBackup,
    ImportAndRestoreFullBackup,
    ExportFullBackup,
    RestoreFullBackup,
    DeleteFullBackup,
    MigrateData,
    OpenDataDirectory,
    OpenFullBackupDirectory,
    OpenFullBackupItem,
};

/** Host-owned ContentDialog purpose. No dangerous action bypasses this gate. */
enum class BackupDataConfirmationKind : std::uint8_t
{
    RestoreLayoutBackup,
    DeleteLayoutBackup,
    ImportAndRestoreFullBackup,
    RestoreFullBackup,
    DeleteFullBackup,
    MigrateData,
};

/** Host-owned picker purpose; the host must bind the picker to the settings HWND. */
enum class BackupDataPickerKind : std::uint8_t
{
    ImportFullBackupArchive,
    ExportFullBackupArchive,
    MigrationSourceDirectory,
};

/**
 * Required host transition after a successful action.
 *
 * ClearDirtyThenRestartApplication is deliberately distinct from a normal
 * restart: the host must first discard/clear controller dirty state so stale
 * in-memory settings cannot overwrite restored or migrated files.
 */
enum class BackupDataCompletionPolicy : std::uint8_t
{
    RefreshBackupLists,
    ReloadDesktopLayout,
    ShowResultOnly,
    ClearDirtyThenRestartApplication,
    None,
};

struct BackupDataConfirmationRequest
{
    BackupDataConfirmationKind kind =
        BackupDataConfirmationKind::RestoreLayoutBackup;
    std::wstring subjectId;
    std::wstring subjectLabel;
    BackupDataCompletionPolicy completionPolicy =
        BackupDataCompletionPolicy::None;
};

struct BackupDataPickerRequest
{
    BackupDataPickerKind kind =
        BackupDataPickerKind::ImportFullBackupArchive;
    std::wstring subjectId;
    std::wstring suggestedFileName;
};

struct BackupDataActionRequest
{
    BackupDataCommand command = BackupDataCommand::CreateLayoutBackup;
    std::wstring subjectId;
    std::wstring displayName;
    std::filesystem::path selectedPath;
    BackupDataCompletionPolicy completionPolicy =
        BackupDataCompletionPolicy::None;
};

/** Strongly typed host boundary for backup, picker and migration work. */
struct BackupDataPageActions
{
    using ConfirmationCompletion = std::function<void(bool confirmed)>;
    using PickerCompletion = std::function<void(
        std::optional<std::filesystem::path> selectedPath)>;

    /** Execute work outside the presenter, normally on a cancellable worker. */
    std::function<void(
        std::uint64_t generation,
        std::uint64_t revision,
        BackupDataActionRequest request)> invoke;

    /** Show a host-owned ContentDialog on the settings XamlRoot. */
    std::function<void(
        std::uint64_t generation,
        std::uint64_t revision,
        BackupDataConfirmationRequest request,
        ConfirmationCompletion completion)> confirm;

    /** Show a host-owned HWND-attached file or folder picker. */
    std::function<void(
        std::uint64_t generation,
        std::uint64_t revision,
        BackupDataPickerRequest request,
        PickerCompletion completion)> pickPath;

    /** Cooperatively cancel the operation identified by the latest snapshot. */
    std::function<void(
        std::uint64_t generation,
        std::uint64_t revision,
        std::uint64_t requestId)> cancel;
};

/**
 * Cached native WinUI 3 presentation for the BackupAndData route.
 *
 * The presenter performs no file I/O, migration, shell launch, restart or
 * private background work. It only emits typed host requests. Host-published
 * snapshots drive lists, ProgressRing, cancellation state and InfoBar output.
 */
class BackupDataPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    BackupDataPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~BackupDataPagePresenter();

    BackupDataPagePresenter(const BackupDataPagePresenter&) = delete;
    BackupDataPagePresenter& operator=(
        const BackupDataPagePresenter&) = delete;
    BackupDataPagePresenter(BackupDataPagePresenter&&) = delete;
    BackupDataPagePresenter& operator=(BackupDataPagePresenter&&) = delete;

    void SetActions(BackupDataPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;

    /** Returns false when the snapshot is uninitialized, stale or closed. */
    [[nodiscard]] bool ApplySnapshot(
        const BackupDataPageSnapshot& snapshot);

    void RefreshLocalizedText();
    void Activate() noexcept;
    void Deactivate() noexcept;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;

    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;

    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
