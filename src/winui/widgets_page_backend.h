#pragma once

#include "widgets_page_presenter.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class WidgetEngine;

namespace snowdesktop::widget
{
enum class PermissionDecisionState;
enum class PermissionRuntimeBlock;
enum class PermissionRiskClass;
}

namespace snowdesktop::winui
{

/**
 * One desktop-owned component instance.
 *
 * DesktopApp can expose its persisted DesktopWidget table through this small
 * value type.  The backend deliberately does not treat WidgetEngine's loaded
 * runtime table as the authoritative desktop-instance table: disabled or
 * permission-blocked instances need not currently have a Lua runtime.
 */
struct WidgetsPageHostInstance
{
    std::wstring instanceId;
    std::wstring packageId;
    std::wstring displayName;
    bool settingsAvailable = false;
};

struct WidgetsPageHostOperationResult
{
    bool succeeded = false;
    bool changed = false;
    std::wstring message;

    [[nodiscard]] static WidgetsPageHostOperationResult Success(
        bool changed = true, std::wstring message = {});
    [[nodiscard]] static WidgetsPageHostOperationResult Failure(
        std::wstring message);
};

/**
 * Host-owned seams that cannot be implemented safely inside the page model.
 *
 * Package state and package-manager mutations come from WidgetEngine. File
 * pickers, ContentDialogs, shell navigation, the DesktopApp instance table,
 * and the existing Steam subscription watcher remain owned by the host.
 */
struct WidgetsPageBackendOptions
{
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;
    using SnapshotChangedCallback = std::function<void(
        std::shared_ptr<const WidgetsPageSnapshot> snapshot)>;
    using DispatchToOwnerCallback =
        std::function<bool(std::function<void()> callback)>;
    using PackagePickerCompletion = std::function<void(
        std::optional<std::filesystem::path> selected)>;
    using ConfirmationCompletion = std::function<void(bool confirmed)>;
    using AsyncCompletion =
        std::function<void(WidgetsPageHostOperationResult result)>;

    LocalizeCallback localize;
    std::function<std::string()> locale;
    std::function<std::vector<WidgetsPageHostInstance>()> instances;
    std::function<bool()> developerOverridesVisible;
    SnapshotChangedCallback snapshotChanged;

    /** Required for source-search and other completions made off-thread. */
    DispatchToOwnerCallback dispatchToOwner;

    /** HWND-owned picker. An empty optional is an ordinary user cancellation. */
    std::function<void(
        std::uint64_t generation,
        PackagePickerCompletion completion)> pickPackage;

    /** Used only for package source/scope expansion after a rejected attempt. */
    std::function<void(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        ConfirmationCompletion completion)> confirmInstall;

    /** Shell work stays outside the backend and remains HWND/process owned. */
    std::function<WidgetsPageHostOperationResult(
        std::string_view sourceId)> openWorkshop;
    std::function<WidgetsPageHostOperationResult(
        std::wstring_view packageId)> addPackageToDesktop;

    /**
     * The Steam watcher/poll bridge is DesktopApp-owned. A source advertises a
     * manual synchronization action only when both callbacks are supplied and
     * canSynchronizeSource returns true for that provider.
     */
    std::function<bool(std::string_view sourceId)>
        canSynchronizeSource;
    std::function<void(
        std::uint64_t generation,
        std::uint64_t taskId,
        std::string sourceId,
        AsyncCompletion completion)> synchronizeSource;

    /**
     * Steam unsubscription can block while the bridge process waits for
     * Steam.  The host therefore performs it away from the settings STA and,
     * after success, asks its authoritative subscription watcher to reconcile
     * the managed package.  The backend deliberately does not uninstall the
     * local package first: doing so would make an unsubscribe/local-delete
     * partial failure impossible to recover consistently.
     */
    std::function<void(
        std::uint64_t generation,
        std::uint64_t taskId,
        std::string externalItemId,
        AsyncCompletion completion)> unsubscribeWorkshop;
    std::function<void(
        std::uint64_t generation,
        std::uint64_t taskId)> cancelAsyncOperation;

    /** Reconcile DesktopApp layout/runtime after a successful mutation. */
    std::function<void()> hostStateChanged;
};

namespace widgets_page_backend_detail
{
/** Stable mappings shared by the production adapter and focused tests. */
[[nodiscard]] WidgetSourceKind SourceKindFor(
    std::string_view providerId) noexcept;
[[nodiscard]] std::string SourceNameKeyFor(
    std::string_view providerId);
[[nodiscard]] WidgetPackagePermissionState PermissionStateFor(
    snowdesktop::widget::PermissionDecisionState state,
    snowdesktop::widget::PermissionRuntimeBlock runtimeBlock) noexcept;
[[nodiscard]] WidgetPermissionRisk PermissionRiskFor(
    snowdesktop::widget::PermissionRiskClass risk) noexcept;
[[nodiscard]] bool InstallFailureNeedsConfirmation(
    std::wstring_view message) noexcept;
[[nodiscard]] bool CompletionIdentityMatches(
    std::uint64_t expectedGeneration,
    std::uint64_t expectedActivation,
    std::uint64_t expectedTaskId,
    std::uint64_t currentGeneration,
    std::uint64_t currentActivation,
    std::uint64_t currentTaskId) noexcept;
}

/**
 * Application-side state owner for WidgetsPagePresenter.
 *
 * Public methods are owner-thread affine. Source discovery/query runs on one
 * private worker; every completion is marshalled through dispatchToOwner and
 * validated against generation, activation epoch, task id, and (for search)
 * searchRevision before it can publish a snapshot.
 */
class WidgetsPageBackend final
{
public:
    WidgetsPageBackend(
        WidgetEngine& engine,
        WidgetsPageBackendOptions options = {});
    ~WidgetsPageBackend();

    WidgetsPageBackend(const WidgetsPageBackend&) = delete;
    WidgetsPageBackend& operator=(const WidgetsPageBackend&) = delete;
    WidgetsPageBackend(WidgetsPageBackend&&) = delete;
    WidgetsPageBackend& operator=(WidgetsPageBackend&&) = delete;

    void SetSnapshotChangedCallback(
        WidgetsPageBackendOptions::SnapshotChangedCallback callback);

    /** Starts a page session and immediately requests the initial source list. */
    [[nodiscard]] bool Activate(std::uint64_t generation);
    void Deactivate() noexcept;

    /** Re-captures packages and the host instance table without source IO. */
    [[nodiscard]] bool Refresh();

    /** Entry point for WidgetsPageActions::invoke. */
    [[nodiscard]] bool Invoke(
        std::uint64_t generation,
        WidgetsPageRequest request);

    [[nodiscard]] std::shared_ptr<const WidgetsPageSnapshot>
        Snapshot() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] bool IsGenerationCurrent(
        std::uint64_t generation) const noexcept;

    void Close() noexcept;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
