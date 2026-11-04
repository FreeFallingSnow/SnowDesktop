#pragma once
#include <windows.h>
#include <string>
#include <array>
#include <map>
#include <vector>

/** @brief 快捷导航“桌面”聚合标签的显示方式。 */
enum class QuickNavigationDesktopViewMode
{
    Tile,
    Source,
    Initial,
    Frequency,
};





/**
 * @brief 快捷导航设置
 * @details 存储快捷导航面板的启用状态和热键组合（修饰键+虚拟键码）
 */
// File retains its stored value for compatibility; composite search includes files.
enum class QuickNavigationSearchType { All, App, File, Web, Settings, Run, Calculator };
inline constexpr std::array kQuickNavigationSearchTypes{
    QuickNavigationSearchType::All, QuickNavigationSearchType::App,
    QuickNavigationSearchType::Web, QuickNavigationSearchType::Settings,
    QuickNavigationSearchType::Run, QuickNavigationSearchType::Calculator};

struct QuickNavigationSearchEngine
{
    std::string id, name, prefix, url;
    bool operator==(const QuickNavigationSearchEngine&) const = default;
};

enum class QuickNavigationFavoriteType
{
    File,
    Folder,
    DesktopItem,
    Application,
};

struct QuickNavigationFavorite
{
    QuickNavigationFavoriteType type = QuickNavigationFavoriteType::File;
    // File/folder path, desktop parsing name, or application parsing name.
    std::string target;
    // Stable desktop layout key when the favorite came from a desktop item.
    std::string desktopKey;
    std::string name;
    bool operator==(const QuickNavigationFavorite&) const = default;
};

struct QuickNavigationLayout
{
    int expandedWidth = 860, collapsedWidth = 640, maximumHeight = 640;
    int visibleRows = 8, padding = 16, searchHeight = 52;
    int iconSize = 56, gridGap = 20, rowGap = 16;
    int fontSize = 14, secondaryFontSize = 12, searchFontSize = 17;
    int resultRowHeight = 56, labelLines = 2;
    int cornerRadius = 8, searchRadius = 6, tabRadius = 6, itemRadius = 6;
    bool operator==(const QuickNavigationLayout&) const = default;
};

struct NavigationSettings
{
    bool enabled = false;
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT virtualKey = VK_SPACE;
    QuickNavigationDesktopViewMode desktopViewMode =
        QuickNavigationDesktopViewMode::Tile;
    QuickNavigationDesktopViewMode mappingViewMode =
        QuickNavigationDesktopViewMode::Source;
    // Stable widget IDs own independent Quick Navigation sorting choices.
    // Missing/false means original (Tile) order; true means alphabetical order.
    std::map<std::string, bool> widgetSortByInitial;
    bool lastCollapsed = false;
    bool appSuggestionsEnabled = false;
    bool favoritesEnabled = false;
    std::vector<QuickNavigationFavorite> favorites;
    QuickNavigationLayout layout;
    std::array<std::string, 6> prefixes{"app", "file", "web", "set", "run", "="};
    std::string defaultEngine = "bing";
    std::vector<QuickNavigationSearchEngine> engines{
        {"bing", "Bing", "bing", "https://www.bing.com/search?q={query}"},
        {"google", "Google", "google", "https://www.google.com/search?q={query}"},
        {"baidu", "Baidu", "baidu", "https://www.baidu.com/s?wd={query}"},
        {"duckduckgo", "DuckDuckGo", "ddg", "https://duckduckgo.com/?q={query}"}};
    // Missing entries follow the resolved theme. #RRGGBB is opaque;
    // #RRGGBBAA stores explicit opacity in the final byte.
    std::map<std::string, std::string> colors;
    bool operator==(const NavigationSettings&) const = default;
};
