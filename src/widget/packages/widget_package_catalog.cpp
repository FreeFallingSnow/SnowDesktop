#include "widget_package.h"
#include "widget_catalog_refresh.h"
#include "widget_package_read.h"

#include "widget/permissions/widget_permission_broker.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop::widget
{
namespace
{
using detail::HasReparsePoint;
using detail::PathUtf8;
using detail::StartsWithPath;
using detail::Utf8ToWide;
using detail::WideToUtf8;

void SelectActiveSources(std::vector<InstalledPackage>& packages)
{
    // Newly discovered projects become the active source; persisted opt-outs remain off.
    std::set<std::string> developmentIds;
    for (const auto& package : packages)
        if (package.development && package.active)
            developmentIds.insert(package.manifest.id);
    for (auto& package : packages)
        if (!package.development && developmentIds.contains(package.manifest.id))
            package.active = false;

    // Built-ins are the fallback when no installed active version exists.
    std::set<std::string> activeIds;
    for (const auto& package : packages)
        if (!package.builtin && package.active) activeIds.insert(package.manifest.id);
    for (auto& package : packages)
        if (package.builtin && activeIds.contains(package.manifest.id))
            package.active = false;
}
}

struct WidgetPackageManager::CatalogScan
{
private:
    const Registry& registry;
    const WidgetPackageValidator& validator;

public:
    CatalogScan(const Registry& inputRegistry,
        const WidgetPackageValidator& inputValidator)
        : registry(inputRegistry), validator(inputValidator),
          knownDevelopmentIds(inputRegistry.knownDevelopmentIds),
          developmentOverrides(inputRegistry.developmentOverrides)
    {
    }

    enum class Source
    {
        Builtin,
        Installed,
        Development,
    };

    void ReadRoot(const std::filesystem::path& root, Source source,
        std::string& error)
    {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) return;
        for (std::filesystem::directory_iterator it(root, ec), end;
            !ec && it != end; it.increment(ec))
        {
            if (!it->is_directory(ec) || HasReparsePoint(it->path())) continue;
            if (source == Source::Installed)
                ReadInstalledVersions(it->path(), ec);
            else
                ReadUnmanagedPackage(it->path(), source, ec);
        }
        if (ec && error.empty())
            error = "cannot enumerate package directory: " + ec.message();
    }

    std::unordered_set<std::string> knownDevelopmentIds;
    std::unordered_set<std::string> developmentOverrides;
    std::shared_ptr<std::vector<InstalledPackage>> packages =
        std::make_shared<std::vector<InstalledPackage>>();
    std::vector<InvalidPackage> invalidPackages;

private:
    void ReadUnmanagedPackage(const std::filesystem::path& root, Source source,
        std::error_code& ec)
    {
        const bool builtin = source == Source::Builtin;
        const bool development = source == Source::Development;
        PackageManifest manifest;
        const auto report = validator.ValidateDirectory(root, &manifest);
        if (!report.Ok())
        {
            // The bundled Agent Skill intentionally shares the
            // built-in root without being a component package.
            if (!builtin || std::filesystem::is_regular_file(
                    root / L"widget.json", ec))
            {
                InvalidPackage invalid;
                invalid.manifest = std::move(manifest);
                invalid.packageId = invalid.manifest.id;
                invalid.root = root;
                invalid.report = report;
                invalid.builtin = builtin;
                invalid.development = development;
                invalid.source = {
                    builtin ? "builtin" : "local-directory",
                    PathUtf8(root.filename()) };
                invalid.selected = builtin ||
                    developmentOverrides.contains(
                        invalid.manifest.id);
                invalidPackages.push_back(std::move(invalid));
            }
            ec.clear();
            return;
        }
        if (development && knownDevelopmentIds.insert(manifest.id).second)
            developmentOverrides.insert(manifest.id);
        InstalledPackage package;
        package.manifest = std::move(manifest);
        package.root = root;
        package.builtin = builtin;
        package.development = development;
        package.source = { builtin ? "builtin" : "local-directory",
            PathUtf8(root.filename()) };
        package.permissionState =
            PermissionDecisionState::LegacyImplicit;
        package.grantedPermissions =
            DeclaredPermissions(package.manifest);
        package.grantedNetworkDomains =
            package.manifest.networkDomains;
        package.enabled = true;
        ApplyScopeMismatchBlock(package, ApplyExplicitDecision(package));
        package.active = builtin ||
            developmentOverrides.contains(package.manifest.id);
        package.selected = package.active;
        if ((development || !package.manifest.nativeHelpers.empty()) &&
            package.permissionState == PermissionDecisionState::LegacyImplicit &&
            !PermissionsRequiringConsent(DeclaredPermissions(package.manifest)).empty())
        {
            package.permissionState = PermissionDecisionState::Pending;
            package.grantedPermissions.clear();
            package.grantedNetworkDomains.clear();
        }
        packages->push_back(std::move(package));
    }

    void ReadInstalledVersions(const std::filesystem::path& root,
        std::error_code& ec)
    {
        const std::string id = WideToUtf8(root.filename().wstring());
        if (!WidgetPackageValidator::IsUuid(id)) return;
        for (std::filesystem::directory_iterator versionIt(root, ec),
            versionEnd; !ec && versionIt != versionEnd;
            versionIt.increment(ec))
        {
            if (!versionIt->is_directory(ec)) continue;
            PackageManifest manifest;
            const auto report = validator.ValidateDirectory(
                versionIt->path(), &manifest);
            if (!report.Ok() || manifest.id != id)
            {
                InvalidPackage invalid;
                invalid.manifest = std::move(manifest);
                invalid.packageId = id;
                invalid.root = versionIt->path();
                invalid.report = report;
                const auto registryIt = registry.entries.find(id);
                invalid.source = registryIt != registry.entries.end()
                    ? registryIt->second.source
                    : PackageSourceRef{ "local", id };
                invalid.selected = registryIt != registry.entries.end() &&
                    registryIt->second.activeVersion ==
                        invalid.manifest.version;
                if (report.Ok() && invalid.manifest.id != id)
                {
                    invalid.report.Add(ValidationSeverity::Error,
                        "package.idDirectory", versionIt->path(),
                        "manifest id does not match the installed package directory");
                }
                invalidPackages.push_back(std::move(invalid));
                continue;
            }
            InstalledPackage package;
            package.manifest = std::move(manifest);
            package.root = versionIt->path();
            const auto registryIt = registry.entries.find(id);
            package.active = registryIt != registry.entries.end() &&
                registryIt->second.activeVersion == package.manifest.version;
            package.selected = package.active;
            package.enabled = registryIt == registry.entries.end() ||
                registryIt->second.enabled;
            if (registryIt != registry.entries.end())
            {
                package.source = registryIt->second.source;
                package.permissionState =
                    registryIt->second.permissionState;
                const auto grant = WidgetPermissionBroker::Evaluate(
                    package.permissionState,
                    package.manifest.permissions,
                    package.manifest.optionalPermissions,
                    package.manifest.networkDomains,
                    registryIt->second.grantedPermissions,
                    registryIt->second.grantedNetworkDomains);
                package.grantedPermissions = grant.permissions;
                package.grantedNetworkDomains = grant.networkDomains;
                ApplyScopeMismatchBlock(
                    package, ApplyExplicitDecision(package));
            }
            else
            {
                package.source = { "local", id };
                package.unregistered = true;
                package.permissionState =
                    PermissionDecisionState::LegacyImplicit;
                package.grantedPermissions =
                    DeclaredPermissions(package.manifest);
                package.grantedNetworkDomains =
                    package.manifest.networkDomains;
            }
            packages->push_back(std::move(package));
        }
    }

    enum class ExplicitDecisionResult
    {
        NotFound,
        Applied,
        ScopeMismatch,
    };

    ExplicitDecisionResult ApplyExplicitDecision(InstalledPackage& package) const
    {
        const std::string key = package.manifest.id + "\n" +
            package.source.providerId + "\n" +
            package.source.externalItemId;
        const auto decision = registry.permissionDecisions.find(key);
        if (decision == registry.permissionDecisions.end())
            return ExplicitDecisionResult::NotFound;
        const std::set<std::string> requestedPermissions(
            decision->second.requestedPermissions.begin(),
            decision->second.requestedPermissions.end());
        const std::set<std::string> declaredPermissions(
            package.manifest.permissions.begin(),
            package.manifest.permissions.end());
        const std::set<std::string> requestedOptionalPermissions(
            decision->second.requestedOptionalPermissions.begin(),
            decision->second.requestedOptionalPermissions.end());
        const std::set<std::string> declaredOptionalPermissions(
            package.manifest.optionalPermissions.begin(),
            package.manifest.optionalPermissions.end());
        const std::set<std::string> requestedDomains(
            decision->second.requestedNetworkDomains.begin(),
            decision->second.requestedNetworkDomains.end());
        const std::set<std::string> declaredDomains(
            package.manifest.networkDomains.begin(),
            package.manifest.networkDomains.end());
        const std::string requestedScopeFingerprint =
            decision->second.requestedScopeFingerprint.empty()
            ? WidgetPermissionBroker::ScopeFingerprint(
                decision->second.requestedPermissions,
                decision->second.requestedOptionalPermissions,
                decision->second.requestedNetworkDomains)
            : decision->second.requestedScopeFingerprint;
        const std::string declaredScopeFingerprint =
            PackagePermissionScopeFingerprint(package.manifest);
        if (requestedPermissions != declaredPermissions ||
            requestedOptionalPermissions !=
                declaredOptionalPermissions ||
            requestedDomains != declaredDomains ||
            requestedScopeFingerprint != declaredScopeFingerprint)
            return ExplicitDecisionResult::ScopeMismatch;
        package.permissionState = decision->second.state;
        const auto grant = WidgetPermissionBroker::Evaluate(
            package.permissionState,
            package.manifest.permissions,
            package.manifest.optionalPermissions,
            package.manifest.networkDomains,
            decision->second.grantedPermissions,
            decision->second.grantedNetworkDomains);
        package.grantedPermissions = grant.permissions;
        package.grantedNetworkDomains = grant.networkDomains;
        return ExplicitDecisionResult::Applied;
    }

    static void ApplyScopeMismatchBlock(
        InstalledPackage& package, ExplicitDecisionResult result)
    {
        if (result != ExplicitDecisionResult::ScopeMismatch) return;
        const auto declaredPermissions = DeclaredPermissions(
            package.manifest);
        const bool requiresConsent = !PermissionsRequiringConsent(
            declaredPermissions).empty();
        if (!requiresConsent)
        {
            package.permissionState = PermissionDecisionState::Granted;
            package.grantedPermissions = declaredPermissions;
            package.grantedNetworkDomains = package.manifest.networkDomains;
            return;
        }
        package.permissionState = PermissionDecisionState::Pending;
        package.grantedPermissions.clear();
        package.grantedNetworkDomains.clear();
    }
};

WidgetPackageManager::WidgetPackageManager(PackagePaths paths)
    : paths_(std::move(paths)),
      catalogueRefresh_(std::make_shared<detail::CatalogRefresh>())
{
}

bool WidgetPackageManager::Initialize(std::string& error)
{
    std::error_code ec;
    for (const auto& path : { paths_.installed, paths_.development,
        paths_.staging, paths_.quarantine, paths_.migrations })
    {
        std::filesystem::create_directories(path, ec);
        if (ec)
        {
            error = "cannot create package directory: " + ec.message();
            return false;
        }
    }
    if (!registry_.Load(paths_, validator_, error)) return false;
    if (!Refresh(error)) return false;
    return true;
}

bool WidgetPackageManager::RefreshCatalog(std::string& error)
{
    error.clear();
    if (catalogueRefresh_->CanReuse(catalogueRevision_)) return true;
    auto refreshed = *this;
    if (!refreshed.Refresh(error)) return false;
    *this = std::move(refreshed);
    return true;
}

bool WidgetPackageManager::Refresh(std::string& error, bool persistDiscovery)
{
    error.clear();
    catalogueRefresh_->Begin({paths_.builtin, paths_.installed, paths_.development});
    CatalogScan scan(registry_, validator_);
    scan.ReadRoot(paths_.builtin, CatalogScan::Source::Builtin, error);
    scan.ReadRoot(paths_.installed, CatalogScan::Source::Installed, error);
    scan.ReadRoot(paths_.development, CatalogScan::Source::Development, error);

    // Swapping leaves the previous discovery fields available for rollback.
    registry_.knownDevelopmentIds.swap(scan.knownDevelopmentIds);
    registry_.developmentOverrides.swap(scan.developmentOverrides);
    invalidPackages_ = std::move(scan.invalidPackages);
    if (!error.empty())
    {
        packages_ = std::move(scan.packages);
        return false;
    }
    if (persistDiscovery && registry_.knownDevelopmentIds != scan.knownDevelopmentIds &&
        !registry_.Save(paths_.registry, error))
    {
        registry_.knownDevelopmentIds.swap(scan.knownDevelopmentIds);
        registry_.developmentOverrides.swap(scan.developmentOverrides);
        scan.packages->clear();
        packages_ = std::move(scan.packages);
        return false;
    }
    SelectActiveSources(*scan.packages);
    packages_ = std::move(scan.packages);
    catalogueRevision_ = std::make_shared<const char>('\0');
    catalogueRefresh_->Complete(catalogueRevision_);
    return error.empty();
}

std::vector<InstalledPackage> WidgetPackageManager::ListPackages() const
{
    return *packages_;
}

std::vector<InvalidPackage> WidgetPackageManager::ListInvalidPackages() const
{
    return invalidPackages_;
}

bool WidgetPackageManager::ContainsPackage(const std::string& packageId) const
{
    return std::any_of(packages_->begin(), packages_->end(),
        [&](const auto& package)
        {
            return package.manifest.id == packageId;
        });
}

std::optional<InstalledPackage> WidgetPackageManager::Resolve(
    const std::string& packageId) const
{
    const auto* package = FindResolved(packageId);
    return package ? std::optional<InstalledPackage>(*package) : std::nullopt;
}

const InstalledPackage* WidgetPackageManager::FindResolved(
    const std::string& packageId) const
{
    if (const auto registry = registry_.entries.find(packageId);
        registry != registry_.entries.end() && !registry->second.enabled &&
        !registry_.developmentOverrides.contains(packageId))
        return nullptr;
    const InstalledPackage* builtinFallback = nullptr;
    for (const auto& package : *packages_)
    {
        if (package.manifest.id != packageId) continue;
        if (package.active && package.enabled) return &package;
        if (package.builtin && package.enabled && !builtinFallback)
            builtinFallback = &package;
    }
    return builtinFallback;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveSelected(
    const std::string& packageId) const
{
    const auto found = std::find_if(packages_->begin(), packages_->end(),
        [&](const InstalledPackage& package) {
            return package.manifest.id == packageId && package.active;
        });
    return found == packages_->end() ? std::nullopt : std::optional<InstalledPackage>(*found);
}

std::optional<std::filesystem::path> WidgetPackageManager::ResolveEntry(
    const std::string& packageId) const
{
    const auto package = Resolve(packageId);
    if (!package) return std::nullopt;
    const auto entry = package->root / Utf8ToWide(package->manifest.entry);
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(
        package->root, ec);
    const auto canonicalEntry = std::filesystem::weakly_canonical(entry, ec);
    if (ec || !StartsWithPath(canonicalEntry, canonicalRoot) ||
        HasReparsePoint(canonicalEntry))
        return std::nullopt;
    return canonicalEntry;
}

std::optional<std::filesystem::path> WidgetPackageManager::ResolveSelectedEntry(
    const std::string& packageId) const
{
    const auto package = ResolveSelected(packageId);
    if (!package) return std::nullopt;
    std::error_code ec;
    const auto root = std::filesystem::weakly_canonical(package->root, ec);
    if (ec) return std::nullopt;
    const auto entry = std::filesystem::weakly_canonical(
        package->root / Utf8ToWide(package->manifest.entry), ec);
    if (ec || !StartsWithPath(entry, root) || HasReparsePoint(entry))
        return std::nullopt;
    return entry;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveSelectedEntryPath(
    const std::filesystem::path& entryPath) const
{
    std::error_code ec;
    const auto input = std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) return std::nullopt;
    for (const auto& package : *packages_)
    {
        if (!package.active) continue;
        const auto entry = ResolveSelectedEntry(package.manifest.id);
        if (entry && std::filesystem::equivalent(*entry, input, ec)) return package;
        ec.clear();
    }
    return std::nullopt;
}

std::optional<InstalledPackage> WidgetPackageManager::ResolveEntryPath(
    const std::filesystem::path& entryPath) const
{
    std::error_code ec;
    const auto canonicalInput =
        std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) return std::nullopt;
    for (const auto& package : *packages_)
    {
        if (!package.active || !package.enabled)
            continue;
        const auto resolved = ResolveEntry(package.manifest.id);
        if (!resolved)
            continue;
        if (std::filesystem::equivalent(*resolved, canonicalInput, ec))
            return package;
        ec.clear();
    }
    return std::nullopt;
}

bool WidgetPackageManager::RefreshChangedPermissionScope(
    const std::string& packageId, bool& changed, std::string& error)
{
    changed = false;
    const auto package = Resolve(packageId);
    if (!package)
    {
        error = "package is unavailable";
        return false;
    }

    PackageManifest diskManifest;
    const ValidationReport report = validator_.ValidateDirectory(
        package->root, &diskManifest);
    if (!report.Ok() || diskManifest.id != packageId)
    {
        error = "active package failed validation while refreshing permissions";
        return false;
    }

    const std::string knownScope = PackagePermissionScopeFingerprint(package->manifest);
    const std::string diskScope = PackagePermissionScopeFingerprint(diskManifest);
    if (knownScope == diskScope)
    {
        error.clear();
        return true;
    }

    if (!Refresh(error)) return false;
    changed = true;
    return true;
}

bool WidgetPackageManager::SetEnabled(const std::string& packageId,
    bool enabled, std::string& error)
{
    auto found = registry_.entries.find(packageId);
    if (found == registry_.entries.end())
    {
        const auto package = Resolve(packageId);
        if (!package || package->builtin)
        {
            error = "built-in packages cannot be disabled in the v1 registry";
            return false;
        }
        return false;
    }
    auto candidate = *this;
    candidate.registry_.entries.at(packageId).enabled = enabled;
    if (!candidate.Refresh(error, false) ||
        !candidate.registry_.Save(candidate.paths_.registry, error)) return false;
    *this = std::move(candidate);
    return true;
}

bool WidgetPackageManager::SetDevelopmentOverride(
    const std::string& packageId, bool active, std::string& error)
{
    const bool developmentExists = std::any_of(
        packages_->begin(), packages_->end(), [&](const auto& package)
        {
            return package.development &&
                package.manifest.id == packageId;
        });
    if (active && !developmentExists)
    {
        error = "development package is unavailable";
        return false;
    }

    const bool previous = registry_.developmentOverrides.contains(packageId);
    if (active)
        registry_.developmentOverrides.insert(packageId);
    else
        registry_.developmentOverrides.erase(packageId);
    if (!registry_.Save(paths_.registry, error))
    {
        if (previous)
            registry_.developmentOverrides.insert(packageId);
        else
            registry_.developmentOverrides.erase(packageId);
        return false;
    }
    const auto rollbackOverride = [&](std::string originalError) {
        if (previous)
            registry_.developmentOverrides.insert(packageId);
        else
            registry_.developmentOverrides.erase(packageId);
        std::string rollbackError;
        const bool saved = registry_.Save(paths_.registry, rollbackError);
        if (saved) Refresh(rollbackError);
        error = std::move(originalError);
        if (!rollbackError.empty())
            error += "; development override rollback failed: " +
                rollbackError;
        return false;
    };

    if (!Refresh(error))
        return rollbackOverride(error);

    if (active)
    {
        const auto resolved = Resolve(packageId);
        if (!resolved || !resolved->development)
        {
            return rollbackOverride(
                "development package could not be activated");
        }
        const std::vector<std::string> declaredPermissions =
            DeclaredPermissions(resolved->manifest);
        const bool sensitive = !PermissionsRequiringConsent(
            declaredPermissions).empty();
        if (sensitive && resolved->permissionState ==
                PermissionDecisionState::LegacyImplicit)
        {
            std::string permissionError;
            if (!SetPermissionDecision(packageId,
                    PermissionDecisionState::Pending, {}, {},
                    permissionError))
            {
                return rollbackOverride(std::move(permissionError));
            }
        }
    }
    return true;
}

}
