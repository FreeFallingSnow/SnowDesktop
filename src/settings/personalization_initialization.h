#pragma once

#include "theme/personalization.h"
#include "ui/menu/modern_menu_appearance_rules.h"

#include <filesystem>

namespace snowdesktop
{
// Keep legacy field defaults and preset recipes unchanged. Only an unused
// profile receives the new initial theme; even unreadable files count as user
// state, so a load failure never opts an existing profile into a new appearance.
inline bool LoadInitialPersonalization(
    const std::filesystem::path& path,
    PersonalizationSettings& settings,
    bool* categorizedTabHeightLoaded = nullptr)
{
    const auto hasUserState = [](const std::filesystem::path& candidate) {
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        return exists || bool(error);
    };
    const bool appearanceHasUserState = hasUserState(path);
    bool newProfile = !appearanceHasUserState;
    if (newProfile)
    {
        // Older profiles can have no separate appearance file. Their other
        // settings or saved desktop still establish that this is an upgrade.
        for (const auto* name : {L"SnowDesktop.general.json", L"SnowDesktop.dock.json",
                 L"SnowDesktop.navigation.json", L"SnowDesktop.categories.json",
                 L"SnowDesktop.layout.json", L"SnowDesktop.storage.json"})
        {
            if (hasUserState(path.parent_path() / name))
            {
                newProfile = false;
                break;
            }
        }
    }
    if (newProfile)
    {
        settings = PersonalizationSettings::GlassDarkPreset();
        settings.contextMenuStyle = static_cast<int>(
            modern_menu::appearance_rules::DefaultForCurrentWindows());
    }
    else if (!appearanceHasUserState)
        settings.contextMenuStyle = static_cast<int>(
            modern_menu::appearance_rules::LegacyFollowForCurrentWindows());
    return LoadPersonalization(path.c_str(), settings, categorizedTabHeightLoaded);
}
}
