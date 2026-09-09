#pragma once

#include "../settings_ipc_values.h"
#include "settings_window_host.h"

// Private same-executable wire schema. Keep field lists explicit: no
// runtime pointers, object layout, native padding or persistence APIs.
namespace snowdesktop::settings_ipc
{
#define SD_IPC_FIELDS(Type, ...) \
    template<> struct Fields<Type> { \
        template<class Value> static auto Tie(Value& v) { return std::tie(__VA_ARGS__); } \
    }

SD_IPC_FIELDS(winui::WidgetPermissionSnapshot,
    v.id, v.labelKey, v.label, v.description,
    v.risk, v.required, v.requiresConsent, v.granted);
SD_IPC_FIELDS(winui::WidgetPackageValidationIssueSnapshot,
    v.code, v.message);
SD_IPC_FIELDS(winui::InvalidWidgetPackageSourceSnapshot,
    v.sourceId, v.sourceName, v.version, v.rootName,
    v.builtIn, v.development, v.selected, v.issues);
SD_IPC_FIELDS(winui::WidgetWorkshopInstallFailureSnapshot,
    v.sourceId, v.externalItemId, v.version, v.error);
SD_IPC_FIELDS(winui::WidgetInstallConfirmationReasonSnapshot,
    v.kind, v.value, v.valueLabelKey);
SD_IPC_FIELDS(winui::WidgetInstallConfirmationRequest,
    v.packageId, v.packageName, v.version, v.sourceId,
    v.externalItemId, v.sha256, v.reasons, v.technicalDetails);
SD_IPC_FIELDS(winui::WidgetInstanceSnapshot,
    v.instanceId, v.displayName, v.settingsAvailable);
SD_IPC_FIELDS(winui::WidgetRestorableVersionSnapshot,
    v.version);
SD_IPC_FIELDS(winui::WidgetRuntimeLogSnapshot,
    v.level, v.message);
SD_IPC_FIELDS(winui::WidgetRuntimeErrorSnapshot,
    v.key, v.message);
SD_IPC_FIELDS(winui::WidgetRuntimeViewNodeSnapshot,
    v.type, v.key, v.debugName, v.testId,
    v.depth, v.x, v.y, v.width,
    v.height);
SD_IPC_FIELDS(winui::WidgetRuntimeDiagnosticSnapshot,
    v.instanceId, v.displayName, v.packageId, v.scriptPath,
    v.valid, v.hasManifest, v.lastError, v.memoryBytes,
    v.memoryLimit, v.lastCallbackMs, v.executionQuotaExceeded, v.memoryQuotaExceeded,
    v.circuitOpen, v.permissions, v.recentLogs, v.auxiliarySurface,
    v.desktopViewNodes, v.auxiliaryViewNodes);
SD_IPC_FIELDS(winui::WidgetAgentSkillTargetSnapshot,
    v.kind, v.id, v.targetPath, v.state,
    v.installedRevision, v.bundledRevision, v.selected, v.installed);
SD_IPC_FIELDS(winui::InstalledWidgetPackageSnapshot,
    v.packageId, v.name, v.description, v.version,
    v.author, v.sourceId, v.sourceName, v.sourceExternalItemId,
    v.workshopExternalItemId, v.permissionScopeFingerprint, v.builtIn, v.development,
    v.valid, v.enabled, v.active, v.canEnable,
    v.canUninstall, v.showAddToDesktop, v.canAddToDesktop, v.canUseDevelopmentOverride,
    v.developmentOverrideActive, v.canCreateDevelopmentProject, v.canInstallDevelopmentSnapshot, v.canPublishDevelopmentPackage,
    v.restorableVersions, v.permissionState, v.canRevokePermissions, v.permissions,
    v.declaredNetworkDomains, v.grantedNetworkDomains, v.invalidSources, v.workshopInstallFailures,
    v.instances);
SD_IPC_FIELDS(winui::WidgetPermissionEditorRequest,
    v.packageId, v.packageName, v.version, v.sourceId,
    v.sourceExternalItemId, v.scopeFingerprint, v.permissionState, v.canRevoke,
    v.permissions, v.declaredNetworkDomains);
SD_IPC_FIELDS(winui::WidgetPermissionEditorResult,
    v.action, v.grantedPermissions, v.grantedNetworkDomains);
SD_IPC_FIELDS(winui::WidgetCatalogItemSnapshot,
    v.sourceId, v.externalItemId, v.packageId, v.name,
    v.description, v.version, v.author, v.installed,
    v.updateAvailable, v.installAllowed);
SD_IPC_FIELDS(winui::WidgetSourceGroupSnapshot,
    v.sourceId, v.kind, v.nameKey, v.name,
    v.status, v.available, v.supportsSearch, v.supportsSynchronization,
    v.supportsInstall, v.workshop, v.results);
SD_IPC_FIELDS(winui::WidgetsPageTaskSnapshot,
    v.taskId, v.kind, v.packageId, v.sourceId,
    v.status, v.cancellable, v.progress);
SD_IPC_FIELDS(winui::WidgetsPageFeedbackSnapshot,
    v.severity, v.titleKey, v.title, v.message);
SD_IPC_FIELDS(winui::WidgetsPageSnapshot,
    v.generation, v.revision, v.searchRevision, v.searchQuery,
    v.installed, v.agentSkills, v.agentSkillTargetMask, v.agentSkillStatusError,
    v.developerActionStatus, v.developmentWorkspace, v.componentCliPath, v.workshopAvailable,
    v.developerPublisherAvailable, v.errors, v.diagnostics, v.sources,
    v.task, v.feedback, v.developerOverridesVisible);
SD_IPC_FIELDS(winui::WidgetsPageRequest,
    v.command, v.taskId, v.searchRevision, v.query,
    v.packageId, v.sourceId, v.externalItemId, v.version,
    v.scopeFingerprint, v.enabled, v.permissionState, v.grantedPermissions,
    v.grantedNetworkDomains, v.agentSkillTargetMask);
SD_IPC_FIELDS(winui::GeneralStartupConflict,
    v.kind, v.ownerCommand);
SD_IPC_FIELDS(winui::GeneralAdvancedFeatureStatus,
    v.state, v.failure, v.bridgeAvailable, v.registered,
    v.validUntil, v.cardVisible, v.offerSteamStore);
SD_IPC_FIELDS(winui::HomeAboutStatusPatch,
    v.generation, v.revision, v.applicationVersion, v.installedWidgetCount,
    v.packaged,
    v.backupState, v.backupCount, v.backupDetail, v.animationDiagnosticsEnabled,
    v.animationDiagnosticsStatus, v.temporaryInitializationEnabled);
SD_IPC_FIELDS(winui::LayoutBackupEntry,
    v.id, v.displayName, v.createdAt, v.hasStorageCompanion);
SD_IPC_FIELDS(winui::FullDataBackupEntry,
    v.id, v.displayName, v.createdAt, v.sourceType,
    v.fileCount, v.totalBytes, v.migrationRollback);
SD_IPC_FIELDS(winui::BackupDataOperationState,
    v.requestId, v.operation, v.running, v.cancellable,
    v.indeterminate, v.progress, v.message);
SD_IPC_FIELDS(winui::BackupDataNotice,
    v.severity, v.title, v.message);
SD_IPC_FIELDS(winui::BackupDataPageSnapshot,
    v.generation, v.revision, v.initialized, v.replacementPending,
    v.layoutBackups, v.fullBackups, v.dataDirectory, v.fullBackupDirectory,
    v.operation, v.notice);
SD_IPC_FIELDS(winui::BackupDataConfirmationRequest,
    v.kind, v.subjectId, v.subjectLabel, v.completionPolicy);
SD_IPC_FIELDS(winui::BackupDataPickerRequest,
    v.kind, v.subjectId, v.suggestedFileName);
SD_IPC_FIELDS(winui::BackupDataActionRequest,
    v.command, v.subjectId, v.displayName, v.selectedPath,
    v.completionPolicy);

#undef SD_IPC_FIELDS
} // namespace snowdesktop::settings_ipc
