/**
 * @file steam_workshop_sync.h
 * @brief Pure subscription reconciliation model shared by Steam source/runtime.
 */

#pragma once

#include "widget_package.h"

#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget
{
struct SteamWorkshopSubscriptionSnapshot
{
    bool authoritative = false;
    std::vector<std::string> subscribedPublishedFileIds;
    std::vector<PackageDetails> installable;
    std::string error;
};

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
};

struct SteamWorkshopSyncPlan
{
    std::vector<SteamWorkshopSyncAction> actions;
    std::vector<std::string> conflicts;
};

struct SteamWorkshopSyncResult
{
    int installed = 0;
    int updated = 0;
    int uninstalled = 0;
    std::vector<std::string> errors;

    bool Changed() const
    {
        return installed != 0 || updated != 0 || uninstalled != 0;
    }
};

std::string SteamPublishedFileId(std::string_view externalItemId);
SteamWorkshopSyncPlan BuildSteamWorkshopSyncPlan(
    const std::vector<InstalledPackage>& installed,
    const SteamWorkshopSubscriptionSnapshot& snapshot);
}
