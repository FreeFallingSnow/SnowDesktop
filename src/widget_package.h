/**
 * @file widget_package.h
 * @brief SnowDesktop Lua component package format, validation and lifecycle.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "widget_logical_slot.h"
#include "widget_permission_state.h"

namespace snowdesktop::widget
{
inline constexpr int kLegacyPackageSchemaVersion = 1;
inline constexpr int kLegacyApiVersion = 1;
inline constexpr int kPackageSchemaVersion = 2;
inline constexpr int kHostApiVersion = 2;
inline constexpr std::uint64_t kMaxArchiveBytes = 20ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxExtractedBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxEntryLuaBytes = 1ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxPreviewBytes = 2ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxResourceBytes = 8ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxPackageFiles = 512;
inline constexpr std::size_t kMaxPackageResources = 32;

enum class ValidationSeverity
{
    Info,
    Warning,
    Error,
};

struct ValidationIssue
{
    ValidationSeverity severity = ValidationSeverity::Error;
    std::string code;
    std::filesystem::path path;
    std::string message;
};

struct ValidationReport
{
    std::vector<ValidationIssue> issues;
    std::uint64_t totalBytes = 0;
    std::size_t fileCount = 0;

    bool Ok() const;
    void Add(ValidationSeverity severity, std::string code,
        std::filesystem::path path, std::string message);
    std::string ToJson() const;
};

struct LocalizedMetadata
{
    std::string title;
    std::string description;
    std::unordered_map<std::string, std::string> strings;
};

struct PackageResource
{
    std::string type;
    std::string path;
    std::string license;

    bool operator==(const PackageResource&) const = default;
};

struct PreviewVariant
{
    std::string id;
    std::string title;
    std::string titleKey;
    std::string description;
    std::string descriptionKey;
    int columns = 0;
    int rows = 0;
    std::unordered_map<std::string, std::string> storage;
    std::unordered_map<std::string, std::string> storageKeys;
};

struct PackageManifest
{
    int schemaVersion = 0;
    std::string id;
    std::string slug;
    std::string version;
    int apiVersion = 0;
    int dataVersion = 1;
    std::string entry = "main.lua";
    std::string minHostVersion;
    std::string name;
    std::string nameKey;
    std::string description;
    std::string descriptionKey;
    std::string author;
    std::string license;
    std::string preview;
    std::string previewIntroduction;
    std::string previewIntroductionKey;
    std::unordered_map<std::string, std::string> previewStorage;
    std::unordered_map<std::string, std::string> previewStorageKeys;
    std::vector<PreviewVariant> previewVariants;
    int defaultColumns = 1;
    int defaultRows = 1;
    int minColumns = 1;
    int minRows = 1;
    int maxColumns = 0;
    int maxRows = 0;
    int refreshIntervalMs = 0;
    std::vector<std::string> permissions;
    std::vector<std::string> optionalPermissions;
    std::vector<std::string> networkDomains;
    std::vector<std::string> requiredFeatures;
    std::vector<std::string> optionalFeatures;
    std::unordered_map<std::string, PackageResource> resources;
    snowdesktop::widget_runtime::LogicalSlotDeclarations logicalSlots;
    std::unordered_map<std::string, LocalizedMetadata> locales;
};

PackageManifest LocalizePackageManifest(PackageManifest manifest,
    const std::string& requestedLocale);
std::vector<std::string> DeclaredPermissions(
    const PackageManifest& manifest);

struct PackageSourceRef
{
    std::string providerId;
    std::string externalItemId;

    bool operator==(const PackageSourceRef&) const = default;
};

struct PackageArtifact
{
    std::filesystem::path localPath;
    std::string packageId;
    std::string version;
    std::string sha256;
};

struct ProviderCapabilities
{
    bool query = false;
    bool details = false;
    bool versions = false;
    bool updates = false;
    bool publishing = false;
    bool progress = false;
};

struct PackageQuery
{
    std::string text;
    std::string locale = "en";
    std::size_t offset = 0;
    std::size_t limit = 50;
};

struct PackageDetails
{
    PackageManifest manifest;
    PackageSourceRef source;
    std::vector<std::string> versions;
    bool withdrawn = false;
};

struct PackageVersionRef
{
    std::string packageId;
    std::string version;
};

struct PackageUpdate
{
    PackageVersionRef current;
    PackageDetails available;
};

struct ProviderStatus
{
    bool available = false;
    std::string message;
};

struct PackageSourceInfo
{
    std::string providerId;
    ProviderCapabilities capabilities;
    ProviderStatus status;
};

struct PublishRequest
{
    PackageArtifact artifact;
    std::string title;
    std::string description;
    std::vector<std::string> tags;
    std::string changeNotes;
    std::optional<std::string> externalItemId;
    std::function<bool(std::uint64_t, std::uint64_t)> progress;
};

struct PublishResult
{
    bool ok = false;
    std::string externalItemId;
    std::string error;
};

struct InstalledPackage
{
    PackageManifest manifest;
    PackageSourceRef source;
    std::filesystem::path root;
    std::string sha256;
    PermissionDecisionState permissionState =
        PermissionDecisionState::LegacyImplicit;
    std::vector<std::string> grantedPermissions;
    std::vector<std::string> grantedNetworkDomains;
    bool builtin = false;
    bool development = false;
    bool enabled = true;
    // Selected within its own source. A development override can temporarily
    // make a selected managed package inactive without hiding its state from
    // management UI.
    bool selected = false;
    bool active = true;
};

struct InvalidPackage
{
    std::string packageId;
    PackageManifest manifest;
    PackageSourceRef source;
    std::filesystem::path root;
    ValidationReport report;
    bool builtin = false;
    bool development = false;
    bool selected = false;
};

struct LegacyPackage
{
    std::filesystem::path scriptPath;
    std::filesystem::path manifestPath;
    std::wstring legacyName;
};

struct LegacyMigrationResult
{
    bool ok = false;
    std::wstring legacyName;
    std::string packageId;
    std::filesystem::path backupDirectory;
    ValidationReport report;
    std::string error;
};

struct LegacyLooseImportResult
{
    bool ok = false;
    std::size_t copiedPairs = 0;
    std::string error;
};

/**
 * @brief Copy only legacy loose-file component pairs from an old portable
 *        widgets directory into the writable package migration root.
 * @details Folder packages, authoring tools, orphaned Lua files, symbolic
 *          links, junctions and reparse points are never copied.
 */
LegacyLooseImportResult ImportLegacyLooseWidgetPairs(
    const std::filesystem::path& sourceWidgets,
    const std::filesystem::path& destinationWidgets);

struct PackagePaths
{
    std::filesystem::path builtin;
    std::filesystem::path installed;
    std::filesystem::path development;
    std::filesystem::path staging;
    std::filesystem::path quarantine;
    std::filesystem::path migrations;
    std::filesystem::path registry;

    static PackagePaths ForCurrentDeployment();
};

class WidgetPackageValidator
{
public:
    ValidationReport ValidateDirectory(const std::filesystem::path& root,
        PackageManifest* manifest = nullptr) const;
    ValidationReport ValidateArchive(
        const std::filesystem::path& archive) const;
    bool ReadManifest(const std::filesystem::path& manifestPath,
        PackageManifest& manifest, ValidationReport& report) const;

    static bool IsUuid(std::string_view value);
    static bool IsSemVer(std::string_view value);
    static bool IsNewerSemVer(
        std::string_view candidate, std::string_view current);
    static bool IsSafeRelativePath(const std::filesystem::path& path);
};

class IWidgetPackageSource
{
public:
    virtual ~IWidgetPackageSource() = default;
    virtual std::string ProviderId() const = 0;
    virtual ProviderCapabilities Capabilities() const = 0;
    virtual ProviderStatus Status() = 0;
    virtual std::vector<PackageDetails> Query(const PackageQuery& query,
        std::string& error) = 0;
    virtual std::optional<PackageDetails> GetDetails(
        const std::string& externalItemId, std::string& error) = 0;
    virtual std::optional<PackageDetails> GetVersionDetails(
        const std::string& externalItemId, const std::string& version,
        std::string& error);
    virtual std::optional<PackageArtifact> Materialize(
        const std::string& externalItemId, const std::string& version,
        const std::filesystem::path& destination, std::string& error) = 0;
    virtual std::vector<PackageUpdate> CheckUpdates(
        const std::vector<PackageVersionRef>& installed,
        std::string& error) = 0;
};

class IWidgetPackagePublisher
{
public:
    virtual ~IWidgetPackagePublisher() = default;
    virtual std::string ProviderId() const = 0;
    virtual ProviderCapabilities Capabilities() const = 0;
    virtual PublishResult Publish(const PublishRequest& request) = 0;
};

class WidgetPackageManager
{
public:
    explicit WidgetPackageManager(
        PackagePaths paths = PackagePaths::ForCurrentDeployment());

    bool Initialize(std::string& error);
    const PackagePaths& Paths() const { return paths_; }
    std::vector<InstalledPackage> ListPackages() const;
    std::vector<InvalidPackage> ListInvalidPackages() const;
    bool ContainsPackage(const std::string& packageId) const;
    std::unordered_map<std::string, std::vector<std::string>>
        SteamSubscriptionHistory() const;
    bool UpdateSteamSubscriptionHistory(const std::string& accountId,
        const std::vector<std::string>& publishedFileIds,
        std::string& error);
    std::optional<InstalledPackage> Resolve(const std::string& packageId) const;
    std::optional<std::filesystem::path> ResolveEntry(
        const std::string& packageId) const;
    std::optional<InstalledPackage> ResolveEntryPath(
        const std::filesystem::path& entryPath) const;

    ValidationReport ValidateDirectory(const std::filesystem::path& root,
        PackageManifest* manifest = nullptr) const;
    ValidationReport ValidateArchive(const std::filesystem::path& archive,
        PackageManifest* manifest = nullptr) const;
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
        std::string& error);
    bool CreateDevelopmentProject(const std::string& packageId,
        std::filesystem::path& projectRoot, std::string& error);
    bool SetDevelopmentOverride(const std::string& packageId, bool active,
        std::string& error);
    bool Rollback(const std::string& packageId, const std::string& version,
        std::string& error);
    bool Uninstall(const std::string& packageId, std::string& error);

    // Only user-authored or third-party loose packages are returned here.
    // Shipped legacy components are replaced automatically during Initialize.
    std::vector<LegacyPackage> FindLegacyPackages() const;
    const std::vector<LegacyMigrationResult>&
        AutomaticLegacyMigrationResults() const
    {
        return automaticLegacyMigrationResults_;
    }
    std::filesystem::path PendingLegacyStoragePath() const;
    std::optional<std::string> ResolveLegacyPackageId(
        const std::wstring& legacyName) const;
    LegacyMigrationResult MigrateLegacy(const LegacyPackage& legacy,
        const std::optional<std::string>& preferredId = std::nullopt);

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

    bool Refresh(std::string& error);
    bool LoadRegistry(std::string& error);
    bool SaveRegistry(std::string& error) const;
    bool ExtractArchive(const std::filesystem::path& archive,
        const std::filesystem::path& destination,
        ValidationReport& report, std::string& error) const;
    bool CopyPackageTree(const std::filesystem::path& source,
        const std::filesystem::path& destination, std::string& error) const;
    bool CommitStagedPackage(const std::filesystem::path& stagedRoot,
        const PackageManifest& manifest, const PackageSourceRef& sourceRef,
        bool allowSourceChange, bool allowPermissionExpansion,
        InstalledPackage& installed,
        std::string& error);
    std::vector<LegacyPackage> ScanLegacyPackages() const;
    std::optional<std::string> BundledReplacementId(
        const LegacyPackage& legacy) const;
    LegacyMigrationResult ReplaceBundledLegacy(
        const LegacyPackage& legacy, const std::string& packageId);
    bool PrepareBundledLegacyStorage(std::string& error);
    void MigrateBundledLegacyPackages();
    std::filesystem::path CreateStagingPath(const char* purpose) const;

    PackagePaths paths_;
    WidgetPackageValidator validator_;
    std::vector<InstalledPackage> packages_;
    std::vector<InvalidPackage> invalidPackages_;
    std::unordered_map<std::string, RegistryEntry> registry_;
    std::unordered_map<std::string, PermissionDecisionRecord>
        permissionDecisions_;
    std::unordered_set<std::string> developmentOverrides_;
    std::unordered_map<std::string, std::string> legacyAliases_;
    std::unordered_map<std::string, std::unordered_set<std::string>>
        steamSubscriptionsByAccount_;
    std::vector<LegacyMigrationResult> automaticLegacyMigrationResults_;
};

class BuiltinPackageSource final : public IWidgetPackageSource
{
public:
    explicit BuiltinPackageSource(const WidgetPackageManager& manager)
        : manager_(manager) {}
    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    ProviderStatus Status() override;
    std::vector<PackageDetails> Query(const PackageQuery& query,
        std::string& error) override;
    std::optional<PackageDetails> GetDetails(
        const std::string& externalItemId, std::string& error) override;
    std::optional<PackageArtifact> Materialize(
        const std::string& externalItemId, const std::string& version,
        const std::filesystem::path& destination, std::string& error) override;
    std::vector<PackageUpdate> CheckUpdates(
        const std::vector<PackageVersionRef>& installed,
        std::string& error) override;

private:
    const WidgetPackageManager& manager_;
};

class LocalDirectorySource final : public IWidgetPackageSource
{
public:
    explicit LocalDirectorySource(std::filesystem::path root);
    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    ProviderStatus Status() override;
    std::vector<PackageDetails> Query(const PackageQuery& query,
        std::string& error) override;
    std::optional<PackageDetails> GetDetails(
        const std::string& externalItemId, std::string& error) override;
    std::optional<PackageArtifact> Materialize(
        const std::string& externalItemId, const std::string& version,
        const std::filesystem::path& destination, std::string& error) override;
    std::vector<PackageUpdate> CheckUpdates(
        const std::vector<PackageVersionRef>& installed,
        std::string& error) override;

private:
    std::filesystem::path root_;
    WidgetPackageValidator validator_;
};

class StaticCatalogSource final : public IWidgetPackageSource
{
public:
    explicit StaticCatalogSource(std::filesystem::path catalogPath);
    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    ProviderStatus Status() override;
    std::vector<PackageDetails> Query(const PackageQuery& query,
        std::string& error) override;
    std::optional<PackageDetails> GetDetails(
        const std::string& externalItemId, std::string& error) override;
    std::optional<PackageDetails> GetVersionDetails(
        const std::string& externalItemId, const std::string& version,
        std::string& error) override;
    std::optional<PackageArtifact> Materialize(
        const std::string& externalItemId, const std::string& version,
        const std::filesystem::path& destination, std::string& error) override;
    std::vector<PackageUpdate> CheckUpdates(
        const std::vector<PackageVersionRef>& installed,
        std::string& error) override;

private:
    bool ReadCatalog(std::vector<PackageDetails>& entries,
        std::unordered_map<std::string, PackageArtifact>& artifacts,
        std::string& error) const;
    std::filesystem::path catalogPath_;
};

class LocalCatalogPublisher final : public IWidgetPackagePublisher
{
public:
    explicit LocalCatalogPublisher(std::filesystem::path catalogDirectory);
    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    PublishResult Publish(const PublishRequest& request) override;

private:
    std::filesystem::path catalogDirectory_;
};
}
