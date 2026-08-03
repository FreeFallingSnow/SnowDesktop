/**
 * @file steam_workshop_sync.cpp
 * @brief Deterministic subscribed/install/update/unsubscribe action planning.
 */

#include "steam_workshop_sync.h"

#include <algorithm>
#include <unordered_set>

namespace snowdesktop::widget
{
namespace
{
bool DigitsOnly(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char character)
        {
            return character >= '0' && character <= '9';
        });
}
}

std::string SteamPublishedFileId(std::string_view externalItemId)
{
    const std::size_t separator = externalItemId.find('@');
    const std::string_view id = externalItemId.substr(0, separator);
    return DigitsOnly(id) ? std::string(id) : std::string{};
}

SteamWorkshopSyncPlan BuildSteamWorkshopSyncPlan(
    const std::vector<InstalledPackage>& installed,
    const SteamWorkshopSubscriptionSnapshot& snapshot)
{
    SteamWorkshopSyncPlan plan;
    if (!snapshot.authoritative) return plan;

    const std::unordered_set<std::string> subscribed(
        snapshot.subscribedPublishedFileIds.begin(),
        snapshot.subscribedPublishedFileIds.end());
    std::unordered_set<std::string> scheduledRemoval;
    for (const auto& package : installed)
    {
        if (package.source.providerId != "steam-workshop")
            continue;
        const std::string publishedFileId =
            SteamPublishedFileId(package.source.externalItemId);
        if (!publishedFileId.empty() && subscribed.contains(publishedFileId))
            continue;
        if (scheduledRemoval.insert(package.manifest.id).second)
        {
            plan.actions.push_back({ SteamWorkshopSyncActionKind::Uninstall,
                package.manifest.id, package.source.externalItemId,
                package.manifest.version });
        }
    }

    std::unordered_set<std::string> plannedPackages;
    for (const auto& available : snapshot.installable)
    {
        const std::string publishedFileId =
            SteamPublishedFileId(available.source.externalItemId);
        if (available.source.providerId != "steam-workshop" ||
            publishedFileId.empty() || !subscribed.contains(publishedFileId))
            continue;
        if (!plannedPackages.insert(available.manifest.id).second)
        {
            plan.conflicts.push_back(available.manifest.id +
                ": multiple subscribed Workshop items expose the same package ID");
            continue;
        }

        bool packageIdExists = false;
        bool sameWorkshopItemExists = false;
        bool requestedVersionExists = false;
        for (const auto& package : installed)
        {
            if (package.manifest.id != available.manifest.id) continue;
            packageIdExists = true;
            if (package.source.providerId != "steam-workshop" ||
                package.source.externalItemId !=
                    available.source.externalItemId)
                continue;
            sameWorkshopItemExists = true;
            if (package.manifest.version == available.manifest.version)
                requestedVersionExists = true;
        }
        if (sameWorkshopItemExists)
        {
            if (!requestedVersionExists)
            {
                plan.actions.push_back({ SteamWorkshopSyncActionKind::Update,
                    available.manifest.id, available.source.externalItemId,
                    available.manifest.version });
            }
            continue;
        }
        if (!packageIdExists ||
            scheduledRemoval.contains(available.manifest.id))
        {
            plan.actions.push_back({ SteamWorkshopSyncActionKind::Install,
                available.manifest.id, available.source.externalItemId,
                available.manifest.version });
            continue;
        }
        plan.conflicts.push_back(available.manifest.id +
            ": an installed package already owns this package ID");
    }
    return plan;
}
}
