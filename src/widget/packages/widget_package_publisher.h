#pragma once

#include "widget_package_source_types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
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

class IWidgetPackagePublisher
{
public:
    virtual ~IWidgetPackagePublisher() = default;
    virtual std::string ProviderId() const = 0;
    virtual ProviderCapabilities Capabilities() const = 0;
    virtual PublishResult Publish(const PublishRequest& request) = 0;
};

class LocalCatalogPublisher final : public IWidgetPackagePublisher
{
public:
    LocalCatalogPublisher(std::filesystem::path catalogDirectory,
        std::filesystem::path validationStaging);
    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    PublishResult Publish(const PublishRequest& request) override;

private:
    std::filesystem::path catalogDirectory_;
    std::filesystem::path validationStaging_;
};
}
