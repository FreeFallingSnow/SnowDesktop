#pragma once

#include "../settings_route.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::winui
{

/** Stable source categories used for grouping the Widgets page. */
enum class WidgetSourceKind : std::uint8_t
{
    BuiltIn,
    Local,
    Catalog,
    SteamWorkshop,
    Development,
    Other,
};

enum class WidgetPackagePermissionState : std::uint8_t
{
    LegacyImplicit,
    Pending,
    Granted,
    Denied,
    MissingRequired,
};

enum class WidgetPermissionRisk : std::uint8_t
{
    Basic,
    SystemStatus,
    PersonalData,
    ExternalCommunication,
    ElevatedRead,
    Modification,
    UserScoped,
    Sensor,
    Unknown,
};

struct WidgetPermissionSnapshot
{
    std::wstring id;
    std::string labelKey;
    std::wstring label;
    std::wstring description;
    WidgetPermissionRisk risk = WidgetPermissionRisk::Unknown;
    bool required = false;
    bool granted = false;
};

struct WidgetInstanceSnapshot
{
    std::wstring instanceId;
    std::wstring displayName;
    bool settingsAvailable = false;
};

/** One installed package, including its active source and desktop instances. */
struct InstalledWidgetPackageSnapshot
{
    std::wstring packageId;
    std::wstring name;
    std::wstring description;
    std::wstring version;
    std::wstring author;
    std::wstring sourceId;
    std::wstring sourceName;
    bool builtIn = false;
    bool development = false;
    bool enabled = true;
    bool active = true;
    bool canEnable = true;
    bool canUninstall = true;
    bool canAddToDesktop = true;
    bool canUseDevelopmentOverride = false;
    bool developmentOverrideActive = false;
    WidgetPackagePermissionState permissionState =
        WidgetPackagePermissionState::LegacyImplicit;
    std::vector<WidgetPermissionSnapshot> permissions;
    std::vector<std::wstring> declaredNetworkDomains;
    std::vector<std::wstring> grantedNetworkDomains;
    std::vector<WidgetInstanceSnapshot> instances;
};

/** A result supplied by a catalog or Workshop source worker owned by the host. */
struct WidgetCatalogItemSnapshot
{
    std::wstring sourceId;
    std::wstring externalItemId;
    std::wstring packageId;
    std::wstring name;
    std::wstring description;
    std::wstring version;
    std::wstring author;
    bool installed = false;
    bool updateAvailable = false;
    bool installAllowed = true;
};

struct WidgetSourceGroupSnapshot
{
    std::wstring sourceId;
    WidgetSourceKind kind = WidgetSourceKind::Other;
    std::string nameKey;
    std::wstring name;
    std::wstring status;
    bool available = true;
    bool supportsSearch = false;
    bool supportsSynchronization = false;
    bool supportsInstall = false;
    bool workshop = false;
    std::vector<WidgetCatalogItemSnapshot> results;
};

enum class WidgetsPageTaskKind : std::uint8_t
{
    None,
    Searching,
    Installing,
    Uninstalling,
    SynchronizingWorkshop,
    ApplyingPermissions,
    ApplyingDevelopmentOverride,
    AddingToDesktop,
};

struct WidgetsPageTaskSnapshot
{
    std::uint64_t taskId = 0;
    WidgetsPageTaskKind kind = WidgetsPageTaskKind::None;
    std::wstring packageId;
    std::wstring sourceId;
    std::wstring status;
    bool cancellable = false;
    /** Empty means indeterminate; otherwise the host supplies a value in [0, 1]. */
    std::optional<double> progress;
};

enum class WidgetsPageFeedbackSeverity : std::uint8_t
{
    None,
    Informational,
    Success,
    Warning,
    Error,
};

struct WidgetsPageFeedbackSnapshot
{
    WidgetsPageFeedbackSeverity severity =
        WidgetsPageFeedbackSeverity::None;
    std::string titleKey;
    std::wstring title;
    std::wstring message;
};

/**
 * Complete immutable data publication for the Widgets route.
 *
 * Activate() establishes the only generation accepted by ApplySnapshot().
 * Host workers must increment revision for every publication, including task
 * progress, so completions from an earlier search or settings session cannot
 * repopulate controls after the page has closed or reloaded.
 */
struct WidgetsPageSnapshot
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    /** Token echoed from SearchSources; older search result sets are ignored. */
    std::uint64_t searchRevision = 0;
    std::wstring searchQuery;
    std::vector<InstalledWidgetPackageSnapshot> installed;
    std::vector<WidgetSourceGroupSnapshot> sources;
    WidgetsPageTaskSnapshot task;
    WidgetsPageFeedbackSnapshot feedback;
    bool developerOverridesVisible = false;
};

enum class WidgetsPageCommand : std::uint8_t
{
    BrowseInstallPackage,
    SearchSources,
    CancelTask,
    InstallCatalogItem,
    SetPackageEnabled,
    UninstallPackage,
    SetPermissionDecision,
    SetDevelopmentOverride,
    OpenWorkshop,
    SynchronizeSource,
    AddPackageToDesktop,
};

/** Strongly typed payload emitted for all package/source operations. */
struct WidgetsPageRequest
{
    WidgetsPageCommand command = WidgetsPageCommand::SearchSources;
    std::uint64_t taskId = 0;
    std::uint64_t searchRevision = 0;
    std::wstring query;
    std::wstring packageId;
    std::wstring sourceId;
    std::wstring externalItemId;
    std::wstring version;
    bool enabled = false;
    WidgetPackagePermissionState permissionState =
        WidgetPackagePermissionState::Granted;
    std::vector<std::wstring> grantedPermissions;
    std::vector<std::wstring> grantedNetworkDomains;
};

struct WidgetsPageActions
{
    using ConfirmationCompletion = std::function<void(bool confirmed)>;

    /** Performs package, Workshop, permission and desktop actions in the host. */
    std::function<void(
        std::uint64_t generation,
        WidgetsPageRequest request)> invoke;

    /** Navigates to an instance's declarative v2 settings route. */
    std::function<void(
        std::uint64_t generation,
        SettingsRoute route)> navigate;

    /** Shows an HWND-owned ContentDialog; the presenter never owns dialogs. */
    std::function<void(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        ConfirmationCompletion completion)> confirm;
};

/**
 * Cached programmatic WinUI controls for the Widgets route.
 *
 * This class performs no package-manager IO, file picking, shell launch,
 * Workshop access or background work. It renders immutable host snapshots and
 * emits typed actions carrying the current settings-session generation.
 */
class WidgetsPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    WidgetsPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~WidgetsPagePresenter();

    WidgetsPagePresenter(const WidgetsPagePresenter&) = delete;
    WidgetsPagePresenter& operator=(const WidgetsPagePresenter&) = delete;
    WidgetsPagePresenter(WidgetsPagePresenter&&) = delete;
    WidgetsPagePresenter& operator=(WidgetsPagePresenter&&) = delete;

    void SetActions(WidgetsPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;

    /** Establishes the generation accepted by subsequent snapshots/actions. */
    void Activate(std::uint64_t generation) noexcept;
    void Deactivate() noexcept;

    /** Returns false when the publication is stale or belongs to another session. */
    [[nodiscard]] bool ApplySnapshot(const WidgetsPageSnapshot& snapshot);
    void RefreshLocalizedText();

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
