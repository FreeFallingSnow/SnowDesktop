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

std::unordered_map<std::string, std::string>
BuildSteamWorkshopPackageAssociations(
    const SteamWorkshopSubscriptionSnapshot& snapshot)
{
    std::unordered_map<std::string, std::string> associations;
    if (!snapshot.authoritative) return associations;
    std::unordered_set<std::string> ambiguousPackages;
    const auto append = [&](const std::string& packageId,
                            const std::string& externalItemId)
    {
        if (packageId.empty() ||
            SteamPublishedFileId(externalItemId).empty() ||
            ambiguousPackages.contains(packageId))
            return;
        const auto [found, inserted] = associations.emplace(
            packageId, externalItemId);
        if (!inserted && found->second != externalItemId)
        {
            associations.erase(found);
            ambiguousPackages.insert(packageId);
        }
    };
    for (const auto& item : snapshot.installable)
        append(item.manifest.id, item.source.externalItemId);
    for (const auto& failure : snapshot.discoveryFailures)
    {
        append(failure.packageId, failure.externalItemId);
    }
    return associations;
}

void ResolveSteamWorkshopSubscriptionRemovals(
    SteamWorkshopSubscriptionSnapshot& snapshot,
    const SteamWorkshopSubscriptionHistory& history,
    const std::vector<InstalledPackage>& installed,
    const SteamLibrarySubscriptionHistory& libraryHistory)
{
    snapshot.explicitlyUnsubscribedPublishedFileIds.clear();
    if (!snapshot.CanSynchronize() || snapshot.activeSteamAccountId.empty())
        return;

    const auto previous = history.find(snapshot.activeSteamAccountId);
    // A newly observed Steam account establishes a baseline. It must never
    // inherit another account's removals.
    if (previous == history.end()) return;

    const std::unordered_set<std::string> current(
        snapshot.subscribedPublishedFileIds.begin(),
        snapshot.subscribedPublishedFileIds.end());
    std::unordered_set<std::string> subscribedByOtherAccounts;
    for (const auto& [accountId, itemIds] : history)
    {
        if (accountId == snapshot.activeSteamAccountId) continue;
        subscribedByOtherAccounts.insert(itemIds.begin(), itemIds.end());
    }
    // Once this account has a baseline, recover managed identities omitted by
    // old/incomplete history directly from the installed source bindings. A
    // package conflict or failed install must not make removals depend on
    // having recorded a successful earlier synchronization.
    std::unordered_set<std::string> candidates(
        previous->second.begin(), previous->second.end());
    std::unordered_set<std::string> readableRemovals(
        snapshot.locallyUnsubscribedPublishedFileIds.begin(),
        snapshot.locallyUnsubscribedPublishedFileIds.end());
    std::unordered_set<std::string> unavailableSubscriptions;
    if (const auto account = libraryHistory.find(snapshot.activeSteamAccountId);
        account != libraryHistory.end())
    {
        for (const auto& [library, subscriptions] : account->second)
        {
            if (snapshot.librarySubscriptions.contains(library))
                readableRemovals.insert(subscriptions.begin(), subscriptions.end());
            else
                unavailableSubscriptions.insert(subscriptions.begin(), subscriptions.end());
        }
    }
    for (const auto& package : installed)
    {
        if (package.builtin || package.development ||
            package.source.providerId != "steam-workshop")
            continue;
        const auto id = SteamPublishedFileId(package.source.externalItemId);
        if (!id.empty()) candidates.insert(id);
    }
    for (const auto& publishedFileId : candidates)
    {
        if (!current.contains(publishedFileId) &&
            !subscribedByOtherAccounts.contains(publishedFileId) &&
            ((snapshot.authoritative && !snapshot.partial) ||
                (readableRemovals.contains(publishedFileId) &&
                    !unavailableSubscriptions.contains(publishedFileId))))
        {
            snapshot.explicitlyUnsubscribedPublishedFileIds.push_back(
                publishedFileId);
        }
    }
    std::sort(snapshot.explicitlyUnsubscribedPublishedFileIds.begin(),
        snapshot.explicitlyUnsubscribedPublishedFileIds.end());
    snapshot.explicitlyUnsubscribedPublishedFileIds.erase(
        std::unique(snapshot.explicitlyUnsubscribedPublishedFileIds.begin(),
            snapshot.explicitlyUnsubscribedPublishedFileIds.end()),
        snapshot.explicitlyUnsubscribedPublishedFileIds.end());
}

std::vector<std::string> BuildSteamWorkshopSubscriptionHistory(
    const SteamWorkshopSubscriptionSnapshot& snapshot,
    const SteamWorkshopSubscriptionHistory& history)
{
    std::unordered_set<std::string> remembered;
    if (!snapshot.authoritative || snapshot.partial)
        if (const auto previous = history.find(snapshot.activeSteamAccountId);
            previous != history.end())
            remembered.insert(previous->second.begin(), previous->second.end());
    remembered.insert(snapshot.subscribedPublishedFileIds.begin(),
        snapshot.subscribedPublishedFileIds.end());
    for (const auto& id : snapshot.confirmedUnsubscribedPublishedFileIds)
        if (std::find(snapshot.subscribedPublishedFileIds.begin(),
                snapshot.subscribedPublishedFileIds.end(), id) ==
            snapshot.subscribedPublishedFileIds.end())
            remembered.erase(id);
    if (snapshot.authoritative || snapshot.partial)
        for (const auto& id : snapshot.explicitlyUnsubscribedPublishedFileIds)
            if (std::find(snapshot.subscribedPublishedFileIds.begin(),
                    snapshot.subscribedPublishedFileIds.end(), id) ==
                snapshot.subscribedPublishedFileIds.end())
                remembered.erase(id);
    std::vector<std::string> result(remembered.begin(), remembered.end());
    std::sort(result.begin(), result.end());
    return result;
}

SteamLibrarySubscriptions BuildSteamWorkshopLibraryHistory(
    const SteamWorkshopSubscriptionSnapshot& snapshot,
    const SteamLibrarySubscriptionHistory& history,
    const std::vector<InstalledPackage>& remainingInstalled)
{
    SteamLibrarySubscriptions result;
    if (const auto account = history.find(snapshot.activeSteamAccountId);
        account != history.end())
        result = account->second;
    if (!snapshot.CanSynchronize()) return result;
    if (snapshot.authoritative && !snapshot.partial &&
        !snapshot.librarySubscriptions.empty())
        result.clear();
    for (const auto& [library, subscriptions] : snapshot.librarySubscriptions)
        result[library] = subscriptions;
    const auto current = BuildSteamWorkshopSubscriptionHistory(snapshot, {});
    const std::unordered_set<std::string> subscribed(current.begin(), current.end());
    // Explicit removals clear all remembered origins, including a library that
    // is currently unavailable. Complete global queries also retire stale IDs.
    for (auto& [library, subscriptions] : result)
    {
        (void)library;
        std::erase_if(subscriptions, [&](const auto& id) {
            if (std::find(snapshot.subscribedPublishedFileIds.begin(),
                    snapshot.subscribedPublishedFileIds.end(), id) !=
                snapshot.subscribedPublishedFileIds.end())
                return false;
            return (snapshot.authoritative && !snapshot.partial && !subscribed.contains(id)) ||
                std::find(snapshot.confirmedUnsubscribedPublishedFileIds.begin(),
                    snapshot.confirmedUnsubscribedPublishedFileIds.end(), id) !=
                    snapshot.confirmedUnsubscribedPublishedFileIds.end() ||
                std::find(snapshot.explicitlyUnsubscribedPublishedFileIds.begin(),
                    snapshot.explicitlyUnsubscribedPublishedFileIds.end(), id) !=
                    snapshot.explicitlyUnsubscribedPublishedFileIds.end();
        });
    }
    // A failed uninstall must retain its origin for the next scoped retry even
    // when Steam has already removed its detail entry from the local manifest.
    if (const auto account = history.find(snapshot.activeSteamAccountId);
        account != history.end())
    {
        for (const auto& package : remainingInstalled)
        {
            if (package.builtin || package.development ||
                package.source.providerId != "steam-workshop") continue;
            const auto id = SteamPublishedFileId(package.source.externalItemId);
            if (std::find(snapshot.explicitlyUnsubscribedPublishedFileIds.begin(),
                    snapshot.explicitlyUnsubscribedPublishedFileIds.end(), id) ==
                    snapshot.explicitlyUnsubscribedPublishedFileIds.end() &&
                std::find(snapshot.confirmedUnsubscribedPublishedFileIds.begin(),
                    snapshot.confirmedUnsubscribedPublishedFileIds.end(), id) ==
                    snapshot.confirmedUnsubscribedPublishedFileIds.end()) continue;
            for (const auto& [library, subscriptions] : account->second)
                if (std::find(subscriptions.begin(), subscriptions.end(), id) != subscriptions.end())
                {
                    auto& retained = result[library];
                    if (std::find(retained.begin(), retained.end(), id) == retained.end())
                        retained.push_back(id);
                }
        }
    }
    return result;
}

SteamWorkshopSyncPlan BuildSteamWorkshopSyncPlan(
    const std::vector<InstalledPackage>& installed,
    const SteamWorkshopSubscriptionSnapshot& snapshot)
{
    SteamWorkshopSyncPlan plan;
    if (!snapshot.CanSynchronize()) return plan;

    const std::unordered_set<std::string> subscribed(
        snapshot.subscribedPublishedFileIds.begin(),
        snapshot.subscribedPublishedFileIds.end());
    std::unordered_set<std::string> explicitlyUnsubscribed(
        snapshot.confirmedUnsubscribedPublishedFileIds.begin(),
        snapshot.confirmedUnsubscribedPublishedFileIds.end());
    if (snapshot.authoritative || snapshot.partial)
        explicitlyUnsubscribed.insert(
            snapshot.explicitlyUnsubscribedPublishedFileIds.begin(),
            snapshot.explicitlyUnsubscribedPublishedFileIds.end());
    std::unordered_set<std::string> scheduledRemoval;
    for (const auto& package : installed)
    {
        if (package.builtin || package.development ||
            package.source.providerId != "steam-workshop")
            continue;
        const std::string publishedFileId =
            SteamPublishedFileId(package.source.externalItemId);
        if (publishedFileId.empty() ||
            !explicitlyUnsubscribed.contains(publishedFileId) ||
            subscribed.contains(publishedFileId))
            continue;
        if (scheduledRemoval.insert(package.manifest.id).second)
        {
            plan.actions.push_back({ SteamWorkshopSyncActionKind::Uninstall,
                package.manifest.id, package.source.externalItemId,
                package.manifest.version, package.manifest });
        }
    }

    if (!snapshot.authoritative && !snapshot.partial) return plan;
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
        std::string boundExternalItemId;
        for (const auto& package : installed)
        {
            if (package.manifest.id != available.manifest.id) continue;
            // Development packages intentionally shadow managed copies. They
            // must not prevent the subscribed Workshop version from being
            // installed and kept up to date in the background.
            if (package.development) continue;
            packageIdExists = true;
            if (package.source.providerId != "steam-workshop" ||
                SteamPublishedFileId(package.source.externalItemId) !=
                    publishedFileId)
                continue;
            sameWorkshopItemExists = true;
            if (boundExternalItemId.empty() ||
                package.source.externalItemId.find('@') != std::string::npos)
                boundExternalItemId = package.source.externalItemId;
            if (package.manifest.version == available.manifest.version)
                requestedVersionExists = true;
        }
        if (sameWorkshopItemExists)
        {
            if (!requestedVersionExists)
            {
                plan.actions.push_back({ SteamWorkshopSyncActionKind::Update,
                    available.manifest.id,
                    boundExternalItemId.empty()
                        ? available.source.externalItemId
                        : boundExternalItemId,
                    available.manifest.version, available.manifest });
            }
            continue;
        }
        if (!packageIdExists ||
            scheduledRemoval.contains(available.manifest.id))
        {
            plan.actions.push_back({ SteamWorkshopSyncActionKind::Install,
                available.manifest.id, available.source.externalItemId,
                available.manifest.version, available.manifest });
            continue;
        }
        plan.conflicts.push_back(available.manifest.id +
            ": an installed package already owns this package ID");
    }
    return plan;
}
}
