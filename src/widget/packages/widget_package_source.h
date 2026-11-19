#pragma once

#include "widget_package.h"
#include "widget_package_source_types.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
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

struct PackageUpdate
{
    PackageVersionRef current;
    PackageDetails available;
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
}
