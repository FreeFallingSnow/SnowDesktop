/**
 * @file steam_workshop_sync.h
 * @brief Pure subscription reconciliation model shared by Steam source/runtime.
 */

#pragma once

#include "widget_package.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget
{
struct SteamWorkshopSubscriptionSnapshot
{
    bool authoritative = false;
    std::string activeSteamAccountId;
    std::vector<std::string> subscribedPublishedFileIds;
    std::vector<std::string> explicitlyUnsubscribedPublishedFileIds;
    std::vector<PackageDetails> installable;
    // Query workers resolve and validate these paths off the UI thread. Before
    // reconciliation, packages that need applying are copied into the local
    // package staging area so the UI thread never touches a slow/offline Steam
    // library or starts SteamAPI merely to install an already-detected item.
    std::unordered_map<std::string, std::filesystem::path> localArtifacts;
    std::unordered_map<std::string, std::filesystem::path> preparedArtifacts;
    std::vector<std::string> preparationErrors;
    std::string error;
};

using SteamWorkshopSubscriptionHistory =
    std::unordered_map<std::string, std::vector<std::string>>;

enum class SteamWorkshopSyncActionKind
{
    Install,
    Update,
    Uninstall,
};

struct SteamWorkshopSyncAction
{
    SteamWorkshopSyncActionKind kind =
        SteamWorkshopSyncActionKind::Install;
    std::string packageId;
    std::string externalItemId;
    std::string version;
    PackageManifest expectedManifest;
};

struct SteamWorkshopSyncPlan
{
    std::vector<SteamWorkshopSyncAction> actions;
    std::vector<std::string> conflicts;
};

struct SteamWorkshopInstallFailure
{
    std::string packageId;
    std::string externalItemId;
    PackageManifest manifest;
    std::string error;
};

struct SteamWorkshopSyncResult
{
    int installed = 0;
    int updated = 0;
    int uninstalled = 0;
    std::vector<std::string> errors;
    std::vector<SteamWorkshopInstallFailure> installFailures;

    bool Changed() const
    {
        return installed != 0 || updated != 0 || uninstalled != 0;
    }
};

std::string SteamPublishedFileId(std::string_view externalItemId);
std::unordered_map<std::string, std::string>
BuildSteamWorkshopPackageAssociations(
    const SteamWorkshopSubscriptionSnapshot& snapshot);
void ResolveSteamWorkshopSubscriptionRemovals(
    SteamWorkshopSubscriptionSnapshot& snapshot,
    const SteamWorkshopSubscriptionHistory& history);
SteamWorkshopSyncPlan BuildSteamWorkshopSyncPlan(
    const std::vector<InstalledPackage>& installed,
    const SteamWorkshopSubscriptionSnapshot& snapshot);
}
