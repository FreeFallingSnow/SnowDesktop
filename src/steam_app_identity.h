#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#ifndef SNOWDESKTOP_STEAM_APP_ID
#error SNOWDESKTOP_STEAM_APP_ID must be provided by CMake
#endif

namespace snowdesktop
{
inline constexpr std::uint32_t kSnowDesktopSteamAppId =
    static_cast<std::uint32_t>(SNOWDESKTOP_STEAM_APP_ID);

inline std::wstring SnowDesktopSteamWorkshopUrl()
{
    return L"https://steamcommunity.com/app/" +
        std::to_wstring(kSnowDesktopSteamAppId) + L"/workshop/";
}

inline std::wstring SnowDesktopSteamWorkshopClientUrl()
{
    return L"steam://openurl/" + SnowDesktopSteamWorkshopUrl();
}

inline std::wstring SnowDesktopSteamCommunityItemUrl(
    std::string_view publishedFileId)
{
    return L"https://steamcommunity.com/sharedfiles/filedetails/?id=" +
        std::wstring(publishedFileId.begin(), publishedFileId.end());
}

inline std::wstring SnowDesktopSteamCommunityItemClientUrl(
    std::string_view publishedFileId)
{
    return L"steam://url/CommunityFilePage/" +
        std::wstring(publishedFileId.begin(), publishedFileId.end());
}
}
