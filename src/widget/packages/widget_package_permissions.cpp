#include "widget_package.h"

#include "widget/permissions/widget_permission_broker.h"

#include <algorithm>
#include <set>
#include <utility>

namespace snowdesktop::widget
{
std::string PackagePermissionScopeFingerprint(const PackageManifest& manifest)
{
    auto permissions = manifest.permissions;
    if (!manifest.nativeHelpers.empty())
        permissions.push_back("native-content:" + manifest.nativeContentFingerprint);
    return WidgetPermissionBroker::ScopeFingerprint(permissions,
        manifest.optionalPermissions, manifest.networkDomains);
}

std::string PackagePermissionReviewFingerprint(const InstalledPackage& package)
{
    const auto scope = PackagePermissionScopeFingerprint(package.manifest);
    if (package.manifest.nativeHelpers.empty()) return scope;
    const std::vector<std::string> identity{
        "scope:" + scope, "id:" + package.manifest.id,
        "version:" + package.manifest.version, "provider:" + package.source.providerId,
        "item:" + package.source.externalItemId };
    return WidgetPermissionBroker::ScopeFingerprint(identity, {});
}

std::vector<std::string> DeclaredPermissions(
    const PackageManifest& manifest)
{
    std::vector<std::string> result = manifest.permissions;
    for (const auto& permission : manifest.optionalPermissions)
        if (std::find(result.begin(), result.end(), permission) ==
            result.end())
            result.push_back(permission);
    return result;
}

bool WidgetPackageManager::SetPermissionDecision(
    const std::string& packageId, PermissionDecisionState state,
    const std::vector<std::string>& grantedPermissions,
    const std::vector<std::string>& grantedNetworkDomains,
    std::string& error, bool nativeAcknowledged, std::string_view reviewedNativeIdentity)
{
    error.clear();
    // Disabled packages retain their selected source for review and edits.
    std::optional<InstalledPackage> package = ResolveSelected(packageId);
    if (!package)
    {
        if (const auto registered = registry_.entries.find(packageId);
            registered != registry_.entries.end())
        {
            const auto managed = std::find_if(packages_->begin(),
                packages_->end(), [&](const auto& candidate) {
                    return candidate.manifest.id == packageId &&
                        candidate.manifest.version ==
                            registered->second.activeVersion &&
                        candidate.source == registered->second.source;
                });
            if (managed != packages_->end()) package = *managed;
        }
    }
    if (!package)
    {
        error = "package is unavailable";
        return false;
    }
    if (state == PermissionDecisionState::LegacyImplicit)
    {
        error = "legacy implicit permission state cannot be user-authored";
        return false;
    }
    if ((state == PermissionDecisionState::Pending ||
            state == PermissionDecisionState::Denied) &&
        (!grantedPermissions.empty() || !grantedNetworkDomains.empty()))
    {
        error = "pending or denied decisions cannot retain granted scopes";
        return false;
    }
    const std::vector<std::string> allDeclaredPermissions =
        DeclaredPermissions(package->manifest);
    const std::set<std::string> declaredPermissions(
        allDeclaredPermissions.begin(),
        allDeclaredPermissions.end());
    const std::set<std::string> declaredDomains(
        package->manifest.networkDomains.begin(),
        package->manifest.networkDomains.end());
    if (!std::all_of(grantedPermissions.begin(), grantedPermissions.end(),
            [&](const auto& permission) {
                return declaredPermissions.contains(permission);
            }) ||
        !std::all_of(grantedNetworkDomains.begin(),
            grantedNetworkDomains.end(), [&](const auto& domain) {
                return declaredDomains.contains(domain);
            }))
    {
        error = "granted scopes must be declared by the active package";
        return false;
    }

    if (state == PermissionDecisionState::Granted &&
        std::find(grantedPermissions.begin(), grantedPermissions.end(), "native.execute") != grantedPermissions.end())
    {
        if (!nativeAcknowledged || reviewedNativeIdentity.empty())
        { error = "nativeConfirmationRequired"; return false; }
        PackageManifest currentManifest;
        if (!validator_.ValidateDirectory(package->root, &currentManifest).Ok() ||
            currentManifest.nativeHelpers.empty() || currentManifest.id != packageId ||
            currentManifest.version != package->manifest.version)
        { error = "nativeContentChanged"; return false; }
        auto current = *package;
        current.manifest = std::move(currentManifest);
        if (PackagePermissionReviewFingerprint(current) != reviewedNativeIdentity)
        { error = "nativeContentChanged"; return false; }
        package->manifest = std::move(current.manifest);
    }
    PermissionDecisionRecord record;
    record.packageId = packageId;
    record.source = package->source;
    record.state = state;
    record.requestedPermissions = package->manifest.permissions;
    record.requestedOptionalPermissions =
        package->manifest.optionalPermissions;
    record.requestedNetworkDomains = package->manifest.networkDomains;
    record.requestedScopeFingerprint =
        PackagePermissionScopeFingerprint(package->manifest);
    record.grantedPermissions = grantedPermissions;
    record.grantedNetworkDomains = grantedNetworkDomains;
    const std::string key = packageId + "\n" +
        record.source.providerId + "\n" + record.source.externalItemId;
    auto pending = *this;
    pending.registry_.permissionDecisions[key] = std::move(record);
    if (const auto installed = pending.registry_.entries.find(packageId);
        installed != pending.registry_.entries.end() &&
        installed->second.source == package->source)
    {
        installed->second.permissionState = state;
        installed->second.grantedPermissions = grantedPermissions;
        installed->second.grantedNetworkDomains = grantedNetworkDomains;
    }
    if (!pending.Refresh(error, false) ||
        !pending.registry_.Save(paths_.registry, error)) return false;
    *this = std::move(pending);
    return true;
}
}
