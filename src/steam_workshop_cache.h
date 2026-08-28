/**
 * @file steam_workshop_cache.h
 * @brief Read-only Steam Workshop cache discovery without starting SteamAPI.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::widget
{
struct SteamWorkshopCachedItem
{
    std::string publishedFileId;
    std::filesystem::path contentDirectory;
};

struct SteamWorkshopLocalCache
{
    bool authoritative = false;
    std::vector<std::string> subscribedPublishedFileIds;
    std::vector<SteamWorkshopCachedItem> readyItems;
    std::string error;
};

std::string ReadSteamActiveUserAccountId();
std::vector<std::filesystem::path> DiscoverSteamLibraryRoots(
    std::uint32_t appId, std::string& error);
SteamWorkshopLocalCache ReadSteamWorkshopLocalCache(
    const std::vector<std::filesystem::path>& libraryRoots,
    std::uint32_t appId);
}
