#pragma once

#include "widget_settings_model.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
/**
 * A declaration keeps the authored default beside the backend-neutral schema.
 * Manifest declarations are merged before script declarations by the service.
 */
struct WidgetSettingSourceField
{
    WidgetSettingFieldSchema schema;
    InteractionValue defaultValue;

    bool operator==(const WidgetSettingSourceField&) const = default;
};

struct WidgetSettingsBackendDescriptor
{
    std::wstring widgetId;
    std::string packageId;
    std::string widgetName;
    std::uint64_t generation = 0;
    bool preview = false;
    bool customStyle = false;
    std::vector<WidgetSettingSourceField> manifestFields;
    std::vector<WidgetSettingSourceField> scriptFields;
    std::vector<WidgetSettingGroupSchema> manifestGroups;
    std::vector<WidgetSettingGroupSchema> scriptGroups;
    std::vector<WidgetSettingPresetSchema> manifestPresets;
    std::vector<WidgetSettingPresetSchema> scriptPresets;
    WidgetHostAppearanceState hostAppearance;
};

enum class WidgetSettingsBackendStatus
{
    Succeeded,
    Unchanged,
    Unavailable,
    WidgetNotFound,
    SettingNotFound,
    StaleSnapshot,
    InvalidValue,
    PersistenceFailed,
    Failed,
};

struct WidgetSettingsBackendResult
{
    WidgetSettingsBackendStatus status = WidgetSettingsBackendStatus::Failed;
    std::string errorCode;
    std::string message;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == WidgetSettingsBackendStatus::Succeeded ||
            status == WidgetSettingsBackendStatus::Unchanged;
    }
};

struct WidgetSettingBackendReadResult
{
    WidgetSettingsBackendResult result;
    bool hasStoredValue = false;
    InteractionValue value;
};

struct WidgetSettingBackendOpaqueResult
{
    WidgetSettingsBackendResult result;
    WidgetSettingOpaqueState state;
};

struct WidgetSettingOrdinaryWrite
{
    std::string key;
    InteractionValue value;
    /**
     * The backend must persist both the encoded value and the typed-storage
     * marker atomically when this is true. It must never route this write
     * through WidgetEngine::RuntimeSetStorageValue.
     */
    bool typedStorage = false;

    /**
     * appSearch keeps the user's query in its declared searchKey. When this
     * value is present, the backend must update that companion key in the
     * same storage transaction as the ordinary setting value.
     */
    std::optional<std::string> searchQuery;

    bool operator==(const WidgetSettingOrdinaryWrite&) const = default;
};

struct WidgetSettingSearchRequest
{
    std::wstring widgetId;
    std::string packageId;
    std::string settingKey;
    std::string query;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    std::size_t maximumResults = 8;
};

struct WidgetSettingSearchCompletion
{
    std::wstring widgetId;
    std::string settingKey;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    WidgetSettingsBackendResult result;
    std::vector<WidgetSettingSearchResult> results;
};

/**
 * Narrow host contract used by the WinUI-independent settings service.
 *
 * Opaque mutations are deliberately split by channel. A backend which cannot
 * perform one safely returns Unavailable; callers must not fall back to an
 * ordinary storage string. Search results cannot carry filesystem paths or
 * application launch targets. The backend privately retains any information
 * needed to commit a result by requestId/resultId.
 */
class IWidgetSettingsBackend
{
public:
    using SearchCompletion =
        std::function<void(WidgetSettingSearchCompletion)>;

    virtual ~IWidgetSettingsBackend() = default;

    virtual WidgetSettingsBackendResult Describe(
        std::wstring_view widgetId,
        WidgetSettingsBackendDescriptor& descriptor) = 0;
    virtual WidgetSettingBackendReadResult ReadOrdinary(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field,
        bool typedStorage) = 0;
    virtual WidgetSettingBackendReadResult ReadSearchQuery(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field) = 0;
    virtual WidgetSettingBackendOpaqueResult ReadOpaque(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingFieldSchema& field) = 0;

    virtual WidgetSettingsBackendResult ApplyOrdinaryTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) = 0;

    /**
     * Applies host-owned appearance keys and optional declarative preset
     * values in one storage transaction.  This preserves the legacy storage
     * format without exposing those host keys as v2 fields.
     */
    virtual WidgetSettingsBackendResult ApplyHostAppearanceTransaction(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetHostAppearancePatch& appearance,
        const std::vector<WidgetSettingOrdinaryWrite>& writes) = 0;

    virtual WidgetSettingsBackendResult SetSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field,
        std::string_view plaintext) = 0;
    virtual WidgetSettingsBackendResult ClearSecret(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) = 0;

    /** The backend owns the picker and grants a handle scoped to this owner. */
    virtual WidgetSettingsBackendResult ChooseFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) = 0;
    virtual WidgetSettingsBackendResult ClearFilesystemHandle(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) = 0;

    /** The backend owns the picker and stores only the logical reference. */
    virtual WidgetSettingsBackendResult OpenEntityReferencePicker(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) = 0;
    virtual WidgetSettingsBackendResult ClearEntityReference(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingFieldSchema& field) = 0;

    virtual WidgetSettingsBackendResult StartSearch(
        WidgetSettingSearchRequest request,
        SearchCompletion completion) = 0;
    virtual WidgetSettingsBackendResult CancelSearch(
        const WidgetSettingSearchRequest& request) = 0;
    virtual WidgetSettingsBackendResult CommitSearchResult(
        const WidgetSettingsBackendDescriptor& widget,
        const WidgetSettingMutationGuard& guard,
        const WidgetSettingSearchRequest& request,
        std::string_view resultId) = 0;
};

struct WidgetSettingsLoadResult
{
    WidgetSettingMutationStatus status =
        WidgetSettingMutationStatus::Failed;
    std::optional<WidgetSettingsSnapshot> snapshot;
    std::string errorCode;
    std::string message;

    [[nodiscard]] bool Succeeded() const noexcept
    {
        return status == WidgetSettingMutationStatus::Applied ||
            status == WidgetSettingMutationStatus::Unchanged;
    }
};

/**
 * Immutable value hint published after a widget settings snapshot changes.
 * Consumers must read Snapshot() again after crossing a thread boundary;
 * the hint deliberately does not own or expose mutable session state.
 */
struct WidgetSettingsSnapshotChanged
{
    std::wstring widgetId;
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;

    bool operator==(const WidgetSettingsSnapshotChanged&) const = default;
};

/**
 * Immutable value hint for one accepted asynchronous search completion.
 * Consumers must read SearchSnapshot() after dispatching to their UI thread.
 */
struct WidgetSettingSearchCompleted
{
    std::wstring widgetId;
    std::string settingKey;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;

    bool operator==(const WidgetSettingSearchCompleted&) const = default;
};

/**
 * Owns immutable snapshots and guards all mutations by instance, generation,
 * and monotonically increasing revision. Public methods may be called from the
 * UI thread; search completions may arrive from any thread.
 */
class WidgetSettingsService
{
public:
    struct State;
    using SnapshotChangedCallback =
        std::function<void(WidgetSettingsSnapshotChanged)>;
    using SearchCompletedCallback =
        std::function<void(WidgetSettingSearchCompleted)>;

    explicit WidgetSettingsService(IWidgetSettingsBackend& backend);
    ~WidgetSettingsService();

    WidgetSettingsService(const WidgetSettingsService&) = delete;
    WidgetSettingsService& operator=(const WidgetSettingsService&) = delete;

    /**
     * Atomically replaces both event callbacks. Passing two empty callbacks
     * detaches the observer. Callbacks are hints, may run on different
     * threads, and are always invoked after the service mutex is released.
     */
    void SetEventCallbacks(
        SnapshotChangedCallback snapshotChanged,
        SearchCompletedCallback searchCompleted);

    WidgetSettingsLoadResult Load(std::wstring widgetId);
    WidgetSettingsLoadResult Reload(std::wstring_view widgetId);
    std::optional<WidgetSettingsSnapshot> Snapshot(
        std::wstring_view widgetId) const;
    void Close(std::wstring_view widgetId);
    void CloseAll();

    WidgetSettingMutationResult SetOrdinary(
        const WidgetSettingMutationGuard& guard, std::string_view key,
        const InteractionValue& value);
    WidgetSettingMutationResult SetSearchQuery(
        const WidgetSettingMutationGuard& guard, std::string_view key,
        std::string query);
    WidgetSettingMutationResult SetSecret(
        const WidgetSettingMutationGuard& guard, std::string_view key,
        std::string_view plaintext);
    WidgetSettingMutationResult ChooseFilesystemHandle(
        const WidgetSettingMutationGuard& guard, std::string_view key);
    WidgetSettingMutationResult OpenEntityReferencePicker(
        const WidgetSettingMutationGuard& guard, std::string_view key);
    WidgetSettingMutationResult ClearOpaque(
        const WidgetSettingMutationGuard& guard, std::string_view key);

    WidgetSettingMutationResult ApplyPreset(
        const WidgetSettingMutationGuard& guard,
        std::string_view presetId);
    WidgetSettingMutationResult UpdateHostAppearance(
        const WidgetSettingMutationGuard& guard,
        const WidgetHostAppearancePatch& patch);
    WidgetSettingMutationResult Reset(
        const WidgetSettingMutationGuard& guard);

    WidgetSettingMutationResult StartSearch(
        const WidgetSettingMutationGuard& guard, std::string_view key,
        std::string query, std::size_t maximumResults = 8);
    WidgetSettingMutationResult CancelSearch(
        const WidgetSettingMutationGuard& guard, std::string_view key);
    WidgetSettingMutationResult CommitSearchResult(
        const WidgetSettingMutationGuard& guard, std::string_view key,
        std::uint64_t requestId, std::string_view resultId);
    std::optional<WidgetSettingSearchSnapshot> SearchSnapshot(
        std::wstring_view widgetId, std::string_view key) const;

private:
    std::shared_ptr<State> state_;
};
}
