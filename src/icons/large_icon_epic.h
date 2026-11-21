#pragma once

#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::large_icon_epic
{
bool IsCoverUrl(std::string_view url);
std::string CoverUrl(std::string_view gameKey, bool portrait, std::stop_token stop);
std::vector<std::string> CachedCovers(std::string_view url, std::stop_token stop);
}
