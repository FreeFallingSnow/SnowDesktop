#pragma once

#include "steam_app_identity.h"
#include "steam_runtime_environment.h"

namespace snowdesktop
{
inline std::vector<wchar_t> BuildSnowDesktopSteamChildEnvironment()
{
    std::vector<std::wstring> entries =
        detail::ReadCurrentEnvironmentEntries();
    if (entries.empty()) return {};
    entries.erase(std::remove_if(entries.begin(), entries.end(),
        [](const std::wstring& entry)
        {
            if (!detail::EnvironmentEntryHasName(
                    entry, L"SteamAppId") &&
                !detail::EnvironmentEntryHasName(
                    entry, L"SteamGameId"))
                return false;
            return true;
        }), entries.end());

    const std::wstring appId = std::to_wstring(kSnowDesktopSteamAppId);
    entries.push_back(L"SteamAppId=" + appId);
    entries.push_back(L"SteamGameId=" + appId);
    return detail::BuildUnicodeEnvironmentBlock(std::move(entries));
}
}
