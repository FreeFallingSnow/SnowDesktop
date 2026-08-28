/**
 * @file steam_workshop_source.h
 * @brief Process-boundary package source for subscribed Steam Workshop items.
 */

#pragma once

#include "steam_workshop_sync.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
class SteamWorkshopSource final : public IWidgetPackageSource
{
public:
    SteamWorkshopSource();
    explicit SteamWorkshopSource(std::filesystem::path bridgeExecutable);

    std::string ProviderId() const override;
    ProviderCapabilities Capabilities() const override;
    ProviderStatus Status() override;
    SteamWorkshopSubscriptionSnapshot QuerySubscriptions(
        const PackageQuery& query, std::string& error);
    SteamWorkshopSubscriptionSnapshot QuerySubscriptionsOnline(
        const PackageQuery& query, std::string& error);
    bool Unsubscribe(const std::string& externalItemId,
        std::string& error) const;
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
    struct BridgeResult;
    struct ResolvedItem
    {
        PackageDetails details;
        std::filesystem::path artifact;
    };

    bool RunBridge(const std::vector<std::wstring>& arguments,
        int timeoutSeconds, BridgeResult& result, std::string& error) const;
    std::optional<ResolvedItem> ResolveCurrent(
        const std::string& externalItemId, bool verifyOwner,
        std::string& error) const;
    std::optional<ResolvedItem> ResolveInstalledFolder(
        const std::string& publishedFileId, const std::string& ownerSteamId,
        const std::filesystem::path& folder, std::string& error,
        PackageManifest* detectedManifest = nullptr) const;

    std::filesystem::path bridgeExecutable_;
    ProviderStatus cachedStatus_;
    std::chrono::steady_clock::time_point statusCheckedAt_{};
    mutable std::optional<ResolvedItem> resolvedCache_;
    mutable std::chrono::steady_clock::time_point resolvedCacheCheckedAt_{};
};
}
