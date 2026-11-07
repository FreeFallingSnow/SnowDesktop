#include "navigation_settings.h"
#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace
{
std::wstring NormalizeFavoriteTarget(const std::string& value)
{
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
        return {};
    std::wstring normalized(
        static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()),
        normalized.data(), size);
    CharUpperBuffW(
        normalized.data(),
        static_cast<DWORD>(normalized.size()));
    return normalized;
}
}

const char* QuickNavigationDesktopViewModeToJson(
    QuickNavigationDesktopViewMode mode)
{
    switch (mode)
    {
    case QuickNavigationDesktopViewMode::Source: return "source";
    case QuickNavigationDesktopViewMode::Initial: return "initial";
    case QuickNavigationDesktopViewMode::Frequency: return "frequency";
    case QuickNavigationDesktopViewMode::Tile:
    default: return "tile";
    }
}

bool QuickNavigationDesktopViewModeFromJson(
    std::string_view value,
    QuickNavigationDesktopViewMode& mode)
{
    if (value == "tile")
        mode = QuickNavigationDesktopViewMode::Tile;
    else if (value == "source")
        mode = QuickNavigationDesktopViewMode::Source;
    else if (value == "initial")
        mode = QuickNavigationDesktopViewMode::Initial;
    else if (value == "frequency")
        mode = QuickNavigationDesktopViewMode::Frequency;
    else
        return false;
    return true;
}

bool ValidateNavigationSearchConfiguration(const NavigationSettings& settings)
{
    std::unordered_set<std::string> prefixes, identifiers;
    auto validPrefix = [&](const std::string& prefix, bool calculator = false) {
        if (prefix.empty() || prefix.size() > 32) return false;
        if (!(calculator && prefix == "="))
            for (unsigned char c : prefix) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        return prefixes.insert(prefix).second;
    };
    for (const auto type : kQuickNavigationSearchTypes)
        if (type != QuickNavigationSearchType::All && !validPrefix(settings.prefixes[static_cast<size_t>(type) - 1], type == QuickNavigationSearchType::Calculator)) return false;
    if (settings.engines.empty() || settings.engines.size() > 32) return false;
    for (const auto& e : settings.engines)
    {
        if (e.id.empty() || !identifiers.insert(e.id).second || e.name.empty() || e.name.size() > 128 ||
            !validPrefix(e.prefix) || e.url.size() > 4096 || e.url.find("{query}") == std::string::npos ||
            (!e.url.starts_with("https://") && !e.url.starts_with("http://"))) return false;
        for (unsigned char c : e.url) if (c < 32 || c == ' ' || c == '\\') return false;
        const auto hostStart = e.url.find("://") + 3;
        const auto hostEnd = e.url.find_first_of("/?#",hostStart);
        const auto host = e.url.substr(hostStart,hostEnd == std::string::npos ? hostEnd : hostEnd - hostStart);
        if (host.empty() || host.find_first_of("{}") != std::string::npos) return false;
    }
    return identifiers.contains(settings.defaultEngine);
}

void NormalizeNavigationSettings(NavigationSettings& settings)
{
    // Frequency is a desktop-only choice; mapping tabs retain their own modes.
    if (settings.mappingViewMode == QuickNavigationDesktopViewMode::Frequency)
        settings.mappingViewMode = QuickNavigationDesktopViewMode::Source;
    auto& l = settings.layout;
#define SD_NAV_CLAMP(name, low, high) l.name = std::clamp(l.name, low, high)
    SD_NAV_CLAMP(expandedWidth, 400, 1800); SD_NAV_CLAMP(collapsedWidth, 360, 1400); SD_NAV_CLAMP(maximumHeight, 220, 1400);
    SD_NAV_CLAMP(visibleRows, 1, 20); SD_NAV_CLAMP(padding, 8, 40); SD_NAV_CLAMP(searchHeight, 40, 80);
    SD_NAV_CLAMP(iconSize, 24, 96); SD_NAV_CLAMP(gridGap, 4, 48); SD_NAV_CLAMP(rowGap, 0, 48);
    SD_NAV_CLAMP(fontSize, 10, 24); SD_NAV_CLAMP(secondaryFontSize, 10, 20); SD_NAV_CLAMP(searchFontSize, 12, 24);
    SD_NAV_CLAMP(resultRowHeight, 40, 96); SD_NAV_CLAMP(labelLines, 1, 3);
    SD_NAV_CLAMP(cornerRadius, 0, 32); SD_NAV_CLAMP(searchRadius, 0, 24); SD_NAV_CLAMP(tabRadius, 0, 20); SD_NAV_CLAMP(itemRadius, 0, 24);
#undef SD_NAV_CLAMP
    l.searchHeight = std::max(l.searchHeight,l.searchFontSize + 16);
    l.resultRowHeight = std::max(l.resultRowHeight,l.fontSize + l.secondaryFontSize + 16);
    if (!ValidateNavigationSearchConfiguration(settings))
    { const NavigationSettings defaults; settings.prefixes = defaults.prefixes; settings.engines = defaults.engines; settings.defaultEngine = defaults.defaultEngine; }
    std::unordered_set<std::wstring> favorites;
    std::vector<QuickNavigationFavorite> normalizedFavorites;
    normalizedFavorites.reserve(std::min<size_t>(settings.favorites.size(), 200));
    for (auto& favorite : settings.favorites)
    {
        if (normalizedFavorites.size() >= 200 || favorite.target.empty() ||
            favorite.name.empty() || favorite.target.size() > 32768 ||
            favorite.desktopKey.size() > 32768 || favorite.name.size() > 1024 ||
            favorite.target.find('\0') != std::string::npos ||
            favorite.desktopKey.find('\0') != std::string::npos ||
            favorite.name.find('\0') != std::string::npos)
            continue;
        const std::wstring key =
            NormalizeFavoriteTarget(favorite.target);
        if (key.empty() || !favorites.insert(key).second) continue;
        normalizedFavorites.push_back(std::move(favorite));
    }
    settings.favorites = std::move(normalizedFavorites);
    for (auto it = settings.colors.begin(); it != settings.colors.end();)
    {
        bool valid = (it->second.size() == 7 || it->second.size() == 9) && it->second[0] == '#';
        for (size_t i = 1; valid && i < it->second.size(); ++i) valid = std::isxdigit(static_cast<unsigned char>(it->second[i])) != 0;
        if (!valid) it = settings.colors.erase(it); else ++it;
    }
}
