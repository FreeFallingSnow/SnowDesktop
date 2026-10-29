#pragma once
#include "steam_workshop_cache.h"

namespace snowdesktop::widget
{
// Account membership comes from userdata/<account>/ugc; library manifests only
// establish which of those subscriptions have completed downloads.
std::filesystem::path CurrentSteamWorkshopAccountCachePath(std::uint32_t appId);
// Pure explicit-file entry point for fixtures. Failure leaves cache untouched.
bool ApplySteamWorkshopAccountCache(const std::filesystem::path& path,
    std::uint32_t appId, SteamWorkshopLocalCache& cache);
SteamWorkshopLocalCache ReadCurrentSteamWorkshopLocalCache(
    const std::vector<std::filesystem::path>& libraryRoots, std::uint32_t appId);
}
