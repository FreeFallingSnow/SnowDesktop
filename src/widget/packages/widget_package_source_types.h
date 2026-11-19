#pragma once

#include <filesystem>
#include <string>

namespace snowdesktop::widget
{
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

struct PackageVersionRef
{
    std::string packageId;
    std::string version;
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
}
