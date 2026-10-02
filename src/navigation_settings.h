/**
 * @file navigation_settings.h
 * @brief 快捷导航面板设置
 * @details 定义快捷导航面板的数据结构（启用状态和热键组合），
 *          以及设置文件的读写与热键格式化函数。
 */

#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cctype>
#include <unordered_set>
#include <map>
#include <vector>

/** @brief 快捷导航“桌面”聚合标签的显示方式。 */
enum class QuickNavigationDesktopViewMode
{
    Tile,
    Source,
    Initial,
};

inline const char* QuickNavigationDesktopViewModeToJson(
    QuickNavigationDesktopViewMode mode)
{
    switch (mode)
    {
    case QuickNavigationDesktopViewMode::Source: return "source";
    case QuickNavigationDesktopViewMode::Initial: return "initial";
    case QuickNavigationDesktopViewMode::Tile:
    default: return "tile";
    }
}

inline bool QuickNavigationDesktopViewModeFromJson(
    std::string_view value,
    QuickNavigationDesktopViewMode& mode)
{
    if (value == "tile")
        mode = QuickNavigationDesktopViewMode::Tile;
    else if (value == "source")
        mode = QuickNavigationDesktopViewMode::Source;
    else if (value == "initial")
        mode = QuickNavigationDesktopViewMode::Initial;
    else
        return false;
    return true;
}

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
    bool defaultCollapsed = false;
    QuickNavigationLayout layout;
    std::array<std::string, 6> prefixes{"app", "file", "web", "set", "run", "calc"};
    std::string defaultEngine = "bing";
    std::vector<QuickNavigationSearchEngine> engines{
        {"bing", "Bing", "bing", "https://www.bing.com/search?q={query}"},
        {"google", "Google", "google", "https://www.google.com/search?q={query}"},
        {"baidu", "Baidu", "baidu", "https://www.baidu.com/s?wd={query}"},
        {"duckduckgo", "DuckDuckGo", "ddg", "https://duckduckgo.com/?q={query}"}};
    // Missing entries follow the resolved theme. Colors are #RRGGBB.
    std::map<std::string, std::string> colors;
    bool operator==(const NavigationSettings&) const = default;
};

void NormalizeNavigationSettings(NavigationSettings& settings);
bool ValidateNavigationSearchConfiguration(const NavigationSettings& settings);

/**
 * @brief 获取导航设置文件路径
 * @details 返回存储快捷导航设置的 JSON 文件的完整路径
 * @return std::wstring 设置文件的绝对路径
 */
std::wstring GetNavigationSettingsPath();
/**
 * @brief 从文件加载导航设置
 * @details 读取指定路径的 JSON 文件，反序列化到 NavigationSettings 结构体
 * @param path   设置文件路径
 * @param settings 输出参数，接收加载的设置数据
 * @return true  加载成功
 * @return false 加载失败（文件不存在或格式错误）
 */
bool LoadNavigationSettings(const wchar_t* path, NavigationSettings& settings);
/**
 * @brief 保存导航设置到文件
 * @details 将 NavigationSettings 序列化为 JSON 并写入指定路径
 * @param path     设置文件路径
 * @param settings 待保存的设置数据
 * @return true  保存成功
 * @return false 保存失败
 */
bool SaveNavigationSettings(const wchar_t* path, const NavigationSettings& settings);
/**
 * @brief 格式化导航热键文本
 * @details 将 NavigationSettings 中的修饰键与虚拟键码转换为可读的字符串（如 "Ctrl+Alt+Space"）
 * @param settings 导航设置数据
 * @return std::wstring 格式化后的热键文本
 */
std::wstring FormatNavigationHotkey(const NavigationSettings& settings);

inline bool ValidateNavigationSearchConfiguration(const NavigationSettings& settings)
{
    std::unordered_set<std::string> prefixes, identifiers;
    auto validPrefix = [&](const std::string& prefix) {
        if (prefix.empty() || prefix.size() > 32) return false;
        for (unsigned char c : prefix) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        return prefixes.insert(prefix).second;
    };
    for (const auto type : kQuickNavigationSearchTypes)
        if (type != QuickNavigationSearchType::All && !validPrefix(settings.prefixes[static_cast<size_t>(type) - 1])) return false;
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

inline void NormalizeNavigationSettings(NavigationSettings& settings)
{
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
    for (auto it = settings.colors.begin(); it != settings.colors.end();)
    {
        bool valid = it->second.size() == 7 && it->second[0] == '#';
        for (size_t i = 1; valid && i < it->second.size(); ++i) valid = std::isxdigit(static_cast<unsigned char>(it->second[i])) != 0;
        if (!valid) it = settings.colors.erase(it); else ++it;
    }
}
