#pragma once

#include "widget_package_catalog_snapshot.h"
#include "widget_package_paths.h"
#include "widget_package_validation.h"
#include "steam/steam_workshop_subscription_history.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget
{
namespace detail { class CatalogRefresh; }
class IWidgetPackageSource;

class WidgetPackageManager
{
public:
    explicit WidgetPackageManager(
        PackagePaths paths = PackagePaths::ForCurrentDeployment());

    bool Initialize(std::string& error);
    // Host UI discovery; preserve the last catalog if scanning/persistence fails.
    bool RefreshCatalog(std::string& error);
    const PackagePaths& Paths() const { return paths_; }
    std::vector<InstalledPackage> ListPackages() const;
    // Capture on the manager's owner thread. The published catalog can be
    // retained and read independently while a later refresh replaces it.
    InstalledPackageCatalog CatalogSnapshot() const { return packages_; }
    std::vector<InvalidPackage> ListInvalidPackages() const;
    bool ContainsPackage(const std::string& packageId) const;
    std::unordered_map<std::string, std::vector<std::string>>
        SteamSubscriptionHistory() const;
    SteamLibrarySubscriptionHistory SteamLibraryHistory() const;
    bool UpdateSteamSubscriptionHistory(const std::string& accountId,
        const std::vector<std::string>& publishedFileIds,
        std::string& error,
        const SteamLibrarySubscriptions* libraries = nullptr);
    std::optional<InstalledPackage> Resolve(const std::string& packageId) const;
    // Synchronous inspection only; invalidated by any catalog mutation.
    // Callers needing ownership across callbacks must use Resolve instead.
    const InstalledPackage* FindResolved(const std::string& packageId) const;
    // Selection and inspection do not grant execution of a disabled package.
    std::optional<InstalledPackage> ResolveSelected(const std::string& packageId) const;
    std::optional<std::filesystem::path> ResolveSelectedEntry(const std::string& packageId) const;
    std::optional<InstalledPackage> ResolveSelectedEntryPath(const std::filesystem::path& entryPath) const;
    std::optional<std::filesystem::path> ResolveEntry(
        const std::string& packageId) const;
    std::optional<InstalledPackage> ResolveEntryPath(
        const std::filesystem::path& entryPath) const;
    /**
     * Re-scan the package catalog when the active package changed its
     * declared permission scope in place. This keeps development hot reloads
     * on the same consent path as a host restart.
     */
    bool RefreshChangedPermissionScope(const std::string& packageId,
        bool& changed, std::string& error);

    ValidationReport ValidateDirectory(const std::filesystem::path& root,
        PackageManifest* manifest = nullptr) const;
    ValidationReport ValidateArchive(const std::filesystem::path& archive,
        PackageManifest* manifest = nullptr, bool* malformedArchive = nullptr) const;
    bool InstallDirectory(const std::filesystem::path& source,
        const PackageSourceRef& sourceRef, bool allowSourceChange,
        InstalledPackage& installed, ValidationReport& report,
        std::string& error, bool allowPermissionExpansion = false,
        const PackageManifest* expectedManifest = nullptr);
    bool InstallArchive(const std::filesystem::path& archive,
        const PackageSourceRef& sourceRef, bool allowSourceChange,
        InstalledPackage& installed, ValidationReport& report,
        std::string& error, bool allowPermissionExpansion = false,
        const PackageManifest* expectedManifest = nullptr);
    bool InstallFromSource(IWidgetPackageSource& source,
        const std::string& externalItemId, const std::string& version,
        bool allowSourceChange, InstalledPackage& installed,
        ValidationReport& report, std::string& error,
        bool allowPermissionExpansion = false);
    bool ExportArchive(const std::string& packageId,
        const std::filesystem::path& output, PackageArtifact& artifact,
        ValidationReport& report, std::string& error) const;
    bool ExportDirectory(const std::filesystem::path& source,
        const std::filesystem::path& output, PackageArtifact& artifact,
        ValidationReport& report, std::string& error) const;
    bool SetEnabled(const std::string& packageId, bool enabled,
        std::string& error);
    bool SetPermissionDecision(const std::string& packageId,
        PermissionDecisionState state,
        const std::vector<std::string>& grantedPermissions,
        const std::vector<std::string>& grantedNetworkDomains,
        std::string& error, bool nativeAcknowledged = false,
        std::string_view reviewedNativeIdentity = {});
    bool CreateDevelopmentProject(const std::string& packageId,
        std::filesystem::path& projectRoot, std::string& error);
    bool SetDevelopmentOverride(const std::string& packageId, bool active,
        std::string& error);
    bool Rollback(const std::string& packageId, const std::string& version,
        std::string& error);
    // Call after startup discovery or after the installed version has passed verification.
    bool TrimInstalledVersions(std::string& error, std::string_view packageId = {});
    bool QuarantineInactiveVersion(const InstalledPackage& package,
        std::string& error);
    bool Uninstall(const std::string& packageId, std::string& error);

    static std::string Sha256File(const std::filesystem::path& path);
    static std::string GenerateUuid();

private:
    struct RegistryEntry
    {
        std::string packageId;
        std::string activeVersion;
        PackageSourceRef source;
        PermissionDecisionState permissionState =
            PermissionDecisionState::LegacyImplicit;
        std::vector<std::string> grantedPermissions;
        std::vector<std::string> grantedNetworkDomains;
        bool enabled = true;
    };

    struct PermissionDecisionRecord
    {
        std::string packageId;
        PackageSourceRef source;
        PermissionDecisionState state = PermissionDecisionState::Pending;
        std::vector<std::string> requestedPermissions;
        std::vector<std::string> requestedOptionalPermissions;
        std::vector<std::string> requestedNetworkDomains;
        std::string requestedScopeFingerprint;
        std::vector<std::string> grantedPermissions;
        std::vector<std::string> grantedNetworkDomains;
    };

    // Persisted package state, copied with the manager during staged changes.
    struct Registry
    {
        bool Load(const PackagePaths& paths, const WidgetPackageValidator& validator,
            std::string& error);
        bool Save(const std::filesystem::path& path, std::string& error) const;
        bool IsEnabled(const std::string& packageId) const;

        std::unordered_map<std::string, RegistryEntry> entries;
        std::unordered_map<std::string, PermissionDecisionRecord>
            permissionDecisions;
        std::unordered_set<std::string> developmentOverrides;
        std::unordered_set<std::string> knownDevelopmentIds;
        std::unordered_set<std::string> disabledDevelopmentIds;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            steamSubscriptionsByAccount;
        SteamLibrarySubscriptionHistory steamLibrarySubscriptionsByAccount;
    };

    struct CatalogScan;
    bool Refresh(std::string& error, bool persistDiscovery = true);
    bool ExtractArchive(const std::filesystem::path& archive,
        const std::filesystem::path& destination,
        ValidationReport& report, std::string& error,
        bool* malformedArchive = nullptr) const;
    bool CopyPackageTree(const std::filesystem::path& source,
        const std::filesystem::path& destination, std::string& error) const;
    bool CommitStagedPackage(const std::filesystem::path& stagedRoot,
        const PackageManifest& manifest, const PackageSourceRef& sourceRef,
        bool allowSourceChange, bool allowPermissionExpansion,
        InstalledPackage& installed,
        std::string& error);
    std::filesystem::path CreateStagingPath(const char* purpose) const;

    PackagePaths paths_;
    std::shared_ptr<detail::CatalogRefresh> catalogueRefresh_;
    std::shared_ptr<const char> catalogueRevision_;
    WidgetPackageValidator validator_;
    InstalledPackageCatalog packages_ = std::make_shared<const std::vector<InstalledPackage>>();
    std::vector<InvalidPackage> invalidPackages_;
    Registry registry_;
};
}
