// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#ifndef SNOWDESKTOP_STEAM_APP_ID
#error SNOWDESKTOP_STEAM_APP_ID must be provided by CMake
#endif

#ifndef SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID
#error SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID must be provided by CMake
#endif

namespace snowdesktop::steam_bridge
{
inline constexpr std::uint32_t kSteamAppId =
    static_cast<std::uint32_t>(SNOWDESKTOP_STEAM_APP_ID);
inline constexpr std::uint32_t kSteamWindowsDepotId =
    static_cast<std::uint32_t>(SNOWDESKTOP_STEAM_WINDOWS_DEPOT_ID);

inline bool IsExpectedSteamAppId(std::uint32_t appId) noexcept
{
    return appId == kSteamAppId;
}

inline bool EnsureExpectedSteamEnvironmentIfMissing()
{
    wchar_t existing[32]{};
    const bool hasSteamAppId =
        GetEnvironmentVariableW(L"SteamAppId", existing,
            static_cast<DWORD>(_countof(existing))) > 0;
    const bool hasSteamGameId =
        GetEnvironmentVariableW(L"SteamGameId", existing,
            static_cast<DWORD>(_countof(existing))) > 0;
    if (hasSteamAppId || hasSteamGameId) return true;

    const std::wstring appId = std::to_wstring(kSteamAppId);
    if (!SetEnvironmentVariableW(L"SteamAppId", appId.c_str())) return false;
    if (SetEnvironmentVariableW(L"SteamGameId", appId.c_str())) return true;
    SetEnvironmentVariableW(L"SteamAppId", nullptr);
    return false;
}

inline std::string SteamWorkshopHomeUrl()
{
    return "https://steamcommunity.com/app/" +
        std::to_string(kSteamAppId) + "/workshop/";
}

inline std::string SteamWorkshopClientUrl()
{
    return "steam://openurl/" + SteamWorkshopHomeUrl();
}

inline std::string SteamCommunityItemClientUrl(
    std::uint64_t publishedFileId)
{
    return "steam://url/CommunityFilePage/" +
        std::to_string(publishedFileId);
}

inline std::string SteamAppIdMismatchMessage(std::uint32_t actualAppId)
{
    return "Steam App ID mismatch: expected " +
        std::to_string(kSteamAppId) + " but Steam provided " +
        std::to_string(actualAppId) +
        ". Launch SnowDesktop through its Steam library entry or use "
        "scripts\\steam-dev.bat for local development.";
}
}
