#pragma once

#include "../settings_route.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
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
    bool requiresConsent = false;
    bool granted = false;
};

struct WidgetPackageValidationIssueSnapshot
{
    std::wstring code;
    std::wstring message;
};

/** One invalid source/version retained by the package manager. */
struct InvalidWidgetPackageSourceSnapshot
{
    std::wstring sourceId;
    std::wstring sourceName;
    std::wstring version;
    std::wstring rootName;
    bool builtIn = false;
    bool development = false;
    bool selected = false;
    std::vector<WidgetPackageValidationIssueSnapshot> issues;
};

/** One cached Steam subscription which failed package installation. */
struct WidgetWorkshopInstallFailureSnapshot
{
    std::wstring sourceId;
    std::wstring externalItemId;
    std::wstring version;
    std::wstring error;
};

enum class WidgetInstallConfirmationReasonKind : std::uint8_t
{
    NewPermission,
    NewWebsite,
    SourceChange,
    Other,
};

/** One independently rendered reason for an installation confirmation. */
struct WidgetInstallConfirmationReasonSnapshot
{
    WidgetInstallConfirmationReasonKind kind =
        WidgetInstallConfirmationReasonKind::Other;
    /** Permission ID or network domain; empty for source changes. */
    std::wstring value;
    /** Present for known permission IDs so the Shell localizes at render time. */
    std::string valueLabelKey;
};

/** Structured package identity and access changes reviewed by the Shell. */
struct WidgetInstallConfirmationRequest
{
    std::wstring packageId;
    std::wstring packageName;
    std::wstring version;
    std::wstring sourceId;
    std::wstring externalItemId;
    std::wstring sha256;
    std::vector<WidgetInstallConfirmationReasonSnapshot> reasons;
    std::wstring technicalDetails;
};

struct WidgetInstanceSnapshot
{
    std::wstring instanceId;
    std::wstring displayName;
    bool settingsAvailable = false;
};

/** One managed package version kept by the package manager for rollback. */
struct WidgetRestorableVersionSnapshot
{
    std::wstring version;
};

struct WidgetRuntimeLogSnapshot
{
    std::wstring level;
    std::wstring message;
};

struct WidgetRuntimeErrorSnapshot
{
    std::wstring key;
    std::wstring message;
};

struct WidgetRuntimeViewNodeSnapshot
{
    std::wstring type;
    std::wstring key;
    std::wstring debugName;
    std::wstring testId;
    std::size_t depth = 0;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/** Runtime-only diagnostics exposed exclusively by gated native pages. */
struct WidgetRuntimeDiagnosticSnapshot
{
    std::wstring instanceId;
    std::wstring displayName;
    std::wstring packageId;
    std::wstring scriptPath;
    bool valid = false;
    bool hasManifest = false;
    std::wstring lastError;
    std::size_t memoryBytes = 0;
    std::size_t memoryLimit = 0;
    double lastCallbackMs = 0.0;
    bool executionQuotaExceeded = false;
    bool memoryQuotaExceeded = false;
    bool circuitOpen = false;
    std::vector<std::wstring> permissions;
    std::vector<WidgetRuntimeLogSnapshot> recentLogs;
    std::wstring auxiliarySurface;
    std::vector<WidgetRuntimeViewNodeSnapshot> desktopViewNodes;
    std::vector<WidgetRuntimeViewNodeSnapshot> auxiliaryViewNodes;
};

enum class WidgetAgentSkillInstallState : std::uint8_t
{
    Unavailable,
    NotInstalled,
    UpdateAvailable,
    Current,
};

enum class WidgetAgentSkillTargetKind : std::uint8_t
{
    Shared,
    Codex,
    ClaudeCode,
    Cursor,
    GitHubCopilot,
    GeminiCli,
};

struct WidgetAgentSkillTargetSnapshot
{
    WidgetAgentSkillTargetKind kind = WidgetAgentSkillTargetKind::Shared;
    std::string id;
    std::wstring targetPath;
    WidgetAgentSkillInstallState state =
        WidgetAgentSkillInstallState::Unavailable;
    bool selected = false;
    bool installed = false;
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
    std::wstring sourceExternalItemId;
    /** Provider item identity used to open the package's Workshop page. */
    std::wstring workshopExternalItemId;
    /** Exact identity reviewed by the permission editor. */
    std::wstring permissionScopeFingerprint;
    bool builtIn = false;
    bool development = false;
    bool valid = true;
    bool enabled = true;
    bool active = true;
    bool canEnable = true;
    bool canUninstall = true;
    bool canAddToDesktop = true;
    bool canUseDevelopmentOverride = false;
    bool developmentOverrideActive = false;
    /** Optional legacy actions are hidden until the host publishes support. */
    bool canCreateDevelopmentProject = false;
    bool canInstallDevelopmentSnapshot = false;
    bool canPublishDevelopmentPackage = false;
    std::vector<WidgetRestorableVersionSnapshot> restorableVersions;
    WidgetPackagePermissionState permissionState =
        WidgetPackagePermissionState::LegacyImplicit;
    bool canRevokePermissions = false;
    std::vector<WidgetPermissionSnapshot> permissions;
    std::vector<std::wstring> declaredNetworkDomains;
    std::vector<std::wstring> grantedNetworkDomains;
    std::vector<InvalidWidgetPackageSourceSnapshot> invalidSources;
    std::vector<WidgetWorkshopInstallFailureSnapshot>
        workshopInstallFailures;
    std::vector<WidgetInstanceSnapshot> instances;
};

enum class WidgetPermissionEditorAction : std::uint8_t
{
    Cancel,
    Apply,
    Revoke,
};

/** Immutable package identity and draft content shown by the Shell dialog. */
struct WidgetPermissionEditorRequest
{
    std::wstring packageId;
    std::wstring packageName;
    std::wstring version;
    std::wstring sourceId;
    std::wstring sourceExternalItemId;
    std::wstring scopeFingerprint;
    WidgetPackagePermissionState permissionState =
        WidgetPackagePermissionState::LegacyImplicit;
    bool canRevoke = false;
    std::vector<WidgetPermissionSnapshot> permissions;
    std::vector<std::wstring> declaredNetworkDomains;
};

struct WidgetPermissionEditorResult
{
    WidgetPermissionEditorAction action =
        WidgetPermissionEditorAction::Cancel;
    std::vector<std::wstring> grantedPermissions;
    std::vector<std::wstring> grantedNetworkDomains;
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
    std::vector<WidgetAgentSkillTargetSnapshot> agentSkills;
    int agentSkillTargetMask = 0;
    std::wstring agentSkillStatusError;
    std::wstring developerActionStatus;
    std::wstring developmentWorkspace;
    std::wstring componentCliPath;
    bool developerPublisherAvailable = false;
    std::vector<WidgetRuntimeErrorSnapshot> errors;
    std::vector<WidgetRuntimeDiagnosticSnapshot> diagnostics;
    std::vector<WidgetSourceGroupSnapshot> sources;
    WidgetsPageTaskSnapshot task;
    WidgetsPageFeedbackSnapshot feedback;
    bool developerOverridesVisible = false;
};

enum class WidgetsPageCommand : std::uint8_t
{
    Refresh,
    BrowseInstallPackage,
    SearchSources,
    CancelTask,
    InstallCatalogItem,
    RetryWorkshopInstall,
    SetPackageEnabled,
    UninstallPackage,
    SetPermissionDecision,
    SetDevelopmentOverride,
    CreateDevelopmentProject,
    InstallDevelopmentSnapshot,
    RollbackPackage,
    PublishDevelopmentPackage,
    OpenWorkshop,
    OpenWorkshopItem,
    SynchronizeSource,
    AddPackageToDesktop,
    RefreshAgentSkills,
    ApplyAgentSkillSelection,
    SetAgentSkillTargetSelection,
    OpenDevelopmentFolder,
    PublishDevelopmentWorkspace,
    ClearWidgetErrors,
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
    std::wstring scopeFingerprint;
    bool enabled = false;
    WidgetPackagePermissionState permissionState =
        WidgetPackagePermissionState::Granted;
    std::vector<std::wstring> grantedPermissions;
    std::vector<std::wstring> grantedNetworkDomains;
    int agentSkillTargetMask = 0;
};

struct WidgetsPageActions
{
    using ConfirmationCompletion = std::function<void(bool confirmed)>;
    using PermissionEditorCompletion =
        std::function<void(WidgetPermissionEditorResult result)>;

    /** Performs package, Workshop, permission and desktop actions in the host. */
    std::function<void(
        std::uint64_t generation,
        WidgetsPageRequest request)> invoke;

    /** Navigates to an instance's declarative v2 settings route. */
    std::function<void(
        std::uint64_t generation,
        SettingsRoute route)> navigate;

    /** Toggles the legacy developer-tools setting; true permits navigation. */
    std::function<bool(
        std::uint64_t generation,
        bool enabled)> setDeveloperToolsEnabled;

    /** Reuses SettingsHostActions::ReloadWidgetInstance. */
    std::function<void(
        std::uint64_t generation,
        std::wstring instanceId)> reloadWidgetInstance;

    /** Shows an HWND-owned ContentDialog; the presenter never owns dialogs. */
    std::function<void(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        ConfirmationCompletion completion)> confirm;

    /** Shows the Shell-owned batch permission ContentDialog. */
    std::function<void(
        std::uint64_t generation,
        WidgetPermissionEditorRequest request,
        PermissionEditorCompletion completion)> editPermissions;
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
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DeveloperToolsContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DebugContent() const noexcept;

    /** Establishes the generation accepted by subsequent snapshots/actions. */
    void Activate(std::uint64_t generation) noexcept;
    void Deactivate() noexcept;

    /** Returns false when the publication is stale or belongs to another session. */
    [[nodiscard]] bool ApplySnapshot(const WidgetsPageSnapshot& snapshot);
    void RefreshLocalizedText();

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        DeveloperToolsFocusTarget(std::string_view focusId) const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        DebugFocusTarget(std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
