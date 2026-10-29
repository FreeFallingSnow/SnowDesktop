#pragma once
#include "icon_library.h"
#include "steam/steam_workshop_cache.h"
#include <atomic>
namespace snowdesktop::icon_workshop
{
bool Available(const std::filesystem::path& bridge);
bool CompatibleConfiguration(std::string_view output);
bool ReadLocalSubscriptions(const widget::SteamWorkshopLocalCache&, std::string account,
    IconPackSubscriptions&, std::string& error);
bool DecodeSubscriptions(std::string_view, IconPackSubscriptions&, std::string& error,
    std::vector<std::wstring>* rejected = nullptr);
bool RenderCover(const icon_package::Package&, const std::filesystem::path& output, std::string& error);
IconLibrarySnapshot Run(IconLibrary&, const IconLibraryRequest&, const std::filesystem::path& bridge,
    const std::filesystem::path& data, const std::atomic_bool* cancel, std::atomic_uint* progress = nullptr);
}
