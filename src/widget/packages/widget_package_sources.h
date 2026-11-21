#pragma once

#include "widget_package_source.h"
#include "widget_package_validation.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget
{
class WidgetPackageManager;

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
}
