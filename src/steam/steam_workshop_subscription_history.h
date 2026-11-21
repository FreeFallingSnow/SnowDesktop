#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::widget
{
using SteamWorkshopSubscriptionHistory =
    std::unordered_map<std::string, std::vector<std::string>>;

// Private host registry metadata, independent of the public package manifest.
using SteamLibrarySubscriptions =
    std::unordered_map<std::string, std::vector<std::string>>;
using SteamLibrarySubscriptionHistory =
    std::unordered_map<std::string, SteamLibrarySubscriptions>;
}
