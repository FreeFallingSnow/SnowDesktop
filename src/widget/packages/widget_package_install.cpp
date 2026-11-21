#include "widget_package_manager.h"
#include "widget_package_source.h"
#include "widget_package_manifest_fields.h"
#include "widget_package_read.h"

#include "widget/permissions/widget_permission_state.h"

#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::PathContainsReparsePoint;
using detail::Timestamp;
using detail::Utf8ToWide;

struct PermissionExpansion
{
    std::vector<std::string> required;
    std::vector<std::string> optional;
    std::vector<std::string> networkDomains;

    bool Empty() const
    {
        return required.empty() && optional.empty() && networkDomains.empty();
    }
};

std::string DescribePermissionExpansion(const PermissionExpansion& expansion)
{
    std::vector<std::string> descriptions;
    descriptions.reserve(expansion.required.size() + expansion.optional.size() +
        expansion.networkDomains.size());
    for (const auto& permission : expansion.required)
        descriptions.push_back(
            "update requests a new required permission: " + permission);
    for (const auto& permission : expansion.optional)
        descriptions.push_back(
            "update requests a new optional permission: " + permission);
    for (const auto& domain : expansion.networkDomains)
        descriptions.push_back(
            "update requests a new network domain: " + domain);

    std::ostringstream output;
    for (std::size_t index = 0; index < descriptions.size(); ++index)
    {
        if (index != 0) output << "; ";
        output << descriptions[index];
    }
    return output.str();
}

bool MatchesExpectedManifest(const PackageManifest& actual,
    const PackageManifest& expected, std::string& error)
{
    if (actual.id != expected.id)
    {
        error = "package identity does not match the source metadata";
        return false;
    }
    if (actual.version != expected.version)
    {
        error = "package version does not match the source metadata";
        return false;
    }
    if (expected.schemaVersion > 0 &&
        actual.schemaVersion != expected.schemaVersion)
    {
        error = "package schema version does not match the source metadata";
        return false;
    }
    if (expected.apiVersion > 0 && actual.apiVersion != expected.apiVersion)
    {
        error = "package API version does not match the source metadata";
        return false;
    }
    const std::set<std::string> actualPermissions(
        actual.permissions.begin(), actual.permissions.end());
    const std::set<std::string> expectedPermissions(
        expected.permissions.begin(), expected.permissions.end());
    if (actualPermissions != expectedPermissions)
    {
        error = "package permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalPermissions(
        actual.optionalPermissions.begin(),
        actual.optionalPermissions.end());
    const std::set<std::string> expectedOptionalPermissions(
        expected.optionalPermissions.begin(),
        expected.optionalPermissions.end());
    if (actualOptionalPermissions != expectedOptionalPermissions)
    {
        error = "package optional permissions do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualDomains(
        actual.networkDomains.begin(), actual.networkDomains.end());
    const std::set<std::string> expectedDomains(
        expected.networkDomains.begin(), expected.networkDomains.end());
    if (actualDomains != expectedDomains)
    {
        error = "package network domains do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualRequiredFeatures(
        actual.requiredFeatures.begin(), actual.requiredFeatures.end());
    const std::set<std::string> expectedRequiredFeatures(
        expected.requiredFeatures.begin(), expected.requiredFeatures.end());
    if (actualRequiredFeatures != expectedRequiredFeatures)
    {
        error = "package required features do not match the source metadata";
        return false;
    }
    const std::set<std::string> actualOptionalFeatures(
        actual.optionalFeatures.begin(), actual.optionalFeatures.end());
    const std::set<std::string> expectedOptionalFeatures(
        expected.optionalFeatures.begin(), expected.optionalFeatures.end());
    if (actualOptionalFeatures != expectedOptionalFeatures)
    {
        error = "package optional features do not match the source metadata";
        return false;
    }
    if (!expected.resources.empty() && actual.resources != expected.resources)
    {
        error = "package resources do not match the source metadata";
        return false;
    }
    if (!expected.logicalSlots.empty() &&
        actual.logicalSlots != expected.logicalSlots)
    {
        error = "package logical slots do not match the source metadata";
        return false;
    }
    return true;
}
}

bool WidgetPackageManager::CommitStagedPackage(
    const std::filesystem::path& stagedRoot, const PackageManifest& manifest,
    const PackageSourceRef& sourceRef, bool allowSourceChange,
    bool allowPermissionExpansion, InstalledPackage& installed,
    std::string& error)
{
    const auto existing = registry_.entries.find(manifest.id);
    if (existing != registry_.entries.end() &&
        !(existing->second.source == sourceRef) &&
        !allowSourceChange)
    {
        error = "package is already owned by provider " +
            existing->second.source.providerId +
            "; source changes require explicit confirmation";
        return false;
    }
    if (existing != registry_.entries.end() &&
        !detail::IsNewerValidSemVer(manifest.version, existing->second.activeVersion) &&
        manifest.version != existing->second.activeVersion)
    {
        error = "package updates cannot downgrade the active version; use rollback";
        return false;
    }
    if (existing != registry_.entries.end() && !allowPermissionExpansion)
    {
        std::vector<std::string> currentRequired;
        std::vector<std::string> currentDeclared;
        std::vector<std::string> currentDomains;
        if (const auto current = ResolveSelected(manifest.id))
        {
            currentRequired = current->manifest.permissions;
            currentDeclared = DeclaredPermissions(current->manifest);
            currentDomains = current->manifest.networkDomains;
        }
        const std::set<std::string> previouslyRequired(
            currentRequired.begin(), currentRequired.end());
        const std::set<std::string> previouslyRequested(
            currentDeclared.begin(), currentDeclared.end());
        PermissionExpansion expansion;
        for (const auto& permission : manifest.permissions)
        {
            if (!previouslyRequired.contains(permission))
                expansion.required.push_back(permission);
        }
        for (const auto& permission : manifest.optionalPermissions)
        {
            if (!previouslyRequested.contains(permission))
                expansion.optional.push_back(permission);
        }
        const std::set<std::string> previouslyRequestedDomains(
            currentDomains.begin(), currentDomains.end());
        for (const auto& domain : manifest.networkDomains)
        {
            if (!previouslyRequestedDomains.contains(domain))
                expansion.networkDomains.push_back(domain);
        }
        if (!expansion.Empty())
        {
            error = DescribePermissionExpansion(expansion);
            return false;
        }
    }
    const auto target = paths_.installed / Utf8ToWide(manifest.id) /
        Utf8ToWide(manifest.version);
    std::error_code ec;
    std::filesystem::path unregisteredBackup;
    if (std::filesystem::exists(target, ec))
    {
        if (existing != registry_.entries.end())
        {
            error = "this package version is already installed";
            return false;
        }
        // An interrupted/older installation can leave valid files without an
        // owner. Install the freshly validated payload with fresh permissions;
        // preserve the old tree instead of trusting or deleting its contents.
        if (!std::filesystem::is_directory(target, ec) ||
            PathContainsReparsePoint(target))
        {
            error = "cannot recover an unsafe unregistered package directory";
            return false;
        }
        std::filesystem::create_directories(paths_.quarantine, ec);
        if (ec || PathContainsReparsePoint(paths_.quarantine))
        {
            error = "cannot prepare unregistered package recovery backup";
            return false;
        }
        unregisteredBackup = paths_.quarantine / Utf8ToWide(
            manifest.id + "-" + manifest.version + "-unregistered-" + GenerateUuid());
        std::filesystem::rename(target, unregisteredBackup, ec);
        if (ec)
        {
            error = "cannot preserve unregistered package: " + ec.message();
            return false;
        }
    }
    const auto restoreUnregistered = [&] {
        if (unregisteredBackup.empty()) return;
        std::error_code restoreError;
        std::filesystem::rename(unregisteredBackup, target, restoreError);
        if (restoreError)
            error += "; cannot restore unregistered package: " + restoreError.message();
    };
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec)
    {
        error = "cannot create package version directory: " + ec.message();
        restoreUnregistered();
        return false;
    }
    std::filesystem::rename(stagedRoot, target, ec);
    if (ec)
    {
        error = "cannot atomically activate staged package: " + ec.message();
        restoreUnregistered();
        return false;
    }
    RegistryEntry entry;
    entry.packageId = manifest.id;
    entry.activeVersion = manifest.version;
    entry.source = sourceRef;
    const auto declaredPermissions = DeclaredPermissions(manifest);
    const bool requiresConsent = !PermissionsRequiringConsent(
        declaredPermissions).empty();
    entry.permissionState = requiresConsent
        ? PermissionDecisionState::Pending
        : PermissionDecisionState::Granted;
    if (!requiresConsent)
    {
        entry.grantedPermissions = declaredPermissions;
        entry.grantedNetworkDomains = manifest.networkDomains;
    }
    entry.enabled = registry_.IsEnabled(manifest.id);
    auto candidate = *this;
    candidate.registry_.disabledDevelopmentIds.erase(manifest.id);
    candidate.registry_.entries[manifest.id] = entry;
    const bool refreshed = candidate.Refresh(error, false);
    const auto resolved = refreshed ? candidate.ResolveSelected(manifest.id) : std::nullopt;
    if (!refreshed || !resolved || !candidate.registry_.Save(candidate.paths_.registry, error))
    {
        if (error.empty()) error = "installed package could not be selected";
        const auto quarantine = paths_.quarantine /
            Utf8ToWide(manifest.id + "-" + manifest.version + "-failed-" + GenerateUuid());
        std::filesystem::rename(target, quarantine, ec);
        if (ec) error += "; cannot quarantine uncommitted package: " + ec.message();
        else restoreUnregistered();
        return false;
    }
    *this = std::move(candidate);
    installed = *resolved;
    return true;
}

bool WidgetPackageManager::InstallDirectory(
    const std::filesystem::path& source, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    PackageManifest manifest;
    report = validator_.ValidateDirectory(source, &manifest);
    if (!report.Ok())
    {
        error = "package validation failed";
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
        return false;
    const auto staging = CreateStagingPath("install");
    if (!CopyPackageTree(source, staging, error)) return false;
    PackageManifest copiedManifest;
    report = validator_.ValidateDirectory(staging, &copiedManifest);
    if (!report.Ok() || copiedManifest.id != manifest.id ||
        copiedManifest.version != manifest.version)
    {
        error = "staged package failed validation";
        const auto quarantine = paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, copiedManifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(copiedManifest.id + "-" + copiedManifest.version +
                "-" + Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::InstallArchive(
    const std::filesystem::path& archive, const PackageSourceRef& sourceRef,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion, const PackageManifest* expectedManifest)
{
    report = validator_.ValidateArchive(archive);
    if (!report.Ok())
    {
        error = "archive validation failed";
        return false;
    }
    const auto staging = CreateStagingPath("archive");
    report = {};
    if (!ExtractArchive(archive, staging, report, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    PackageManifest manifest;
    report = validator_.ValidateDirectory(staging, &manifest);
    if (!report.Ok())
    {
        error = "extracted package validation failed";
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (expectedManifest &&
        !MatchesExpectedManifest(manifest, *expectedManifest, error))
    {
        const auto quarantine = paths_.quarantine /
            (archive.stem().wstring() + L"-" + Utf8ToWide(Timestamp()));
        std::error_code ec;
        std::filesystem::rename(staging, quarantine, ec);
        return false;
    }
    if (CommitStagedPackage(staging, manifest, sourceRef,
        allowSourceChange, allowPermissionExpansion, installed, error))
        return true;
    std::error_code commitError;
    if (std::filesystem::exists(staging, commitError))
        std::filesystem::rename(staging, paths_.quarantine /
            (Utf8ToWide(manifest.id + "-" + manifest.version + "-" +
                Timestamp())), commitError);
    return false;
}

bool WidgetPackageManager::InstallFromSource(IWidgetPackageSource& source,
    const std::string& externalItemId, const std::string& version,
    bool allowSourceChange, InstalledPackage& installed,
    ValidationReport& report, std::string& error,
    bool allowPermissionExpansion)
{
    const auto status = source.Status();
    if (!status.available)
    {
        error = status.message.empty() ? "package source is unavailable" :
            status.message;
        return false;
    }
    const auto details =
        source.GetVersionDetails(externalItemId, version, error);
    if (!details)
        return false;
    const auto staging = CreateStagingPath("provider");
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec)
    {
        error = "cannot create provider staging directory: " + ec.message();
        return false;
    }
    const auto destination = staging / L"artifact.snowwidget";
    const auto artifact =
        source.Materialize(externalItemId, version, destination, error);
    auto quarantineOrRemove = [&]()
    {
        bool empty = std::filesystem::is_empty(staging, ec);
        if (!ec && empty)
        {
            std::filesystem::remove_all(staging, ec);
            return;
        }
        ec.clear();
        std::filesystem::rename(staging, paths_.quarantine /
            Utf8ToWide("provider-" + Timestamp() + "-" + GenerateUuid()), ec);
        if (ec) std::filesystem::remove_all(staging, ec);
    };
    if (!artifact)
    {
        quarantineOrRemove();
        return false;
    }
    if (artifact->packageId != details->manifest.id ||
        !WidgetPackageValidator::IsUuid(artifact->packageId))
    {
        error = "provider returned an invalid package identity: artifact=" +
            (artifact->packageId.empty() ? "<empty>" : artifact->packageId) +
            ", expected=" +
            (details->manifest.id.empty() ? "<empty>" : details->manifest.id);
        quarantineOrRemove();
        return false;
    }
    if (artifact->version != version)
    {
        error = "provider returned a different package version";
        quarantineOrRemove();
        return false;
    }

    const PackageSourceRef sourceRef = details->source;
    if (sourceRef.providerId != source.ProviderId() ||
        sourceRef.externalItemId.empty())
    {
        error = "provider returned an invalid source identity";
        quarantineOrRemove();
        return false;
    }
    const bool ok = std::filesystem::is_directory(artifact->localPath, ec)
        ? InstallDirectory(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest)
        : InstallArchive(artifact->localPath, sourceRef, allowSourceChange,
            installed, report, error, allowPermissionExpansion,
            &details->manifest);
    if (!ok)
    {
        quarantineOrRemove();
        return false;
    }
    std::filesystem::remove_all(staging, ec);
    return true;
}
}
