/**
 * @file navigation_settings.cpp
 * @brief 快捷导航面板设置的实现
 * @details 提供导航设置 JSON 文件的路径获取、加载、保存，
 *          以及热键格式化的具体实现。内部使用匿名命名空间中的
 *          辅助函数完成 JSON 字段的解析和虚拟键码的名称转换。
 */

#include "navigation_settings.h"
#include "data/data_paths.h"
#include "common/json_value.h"
#include "quick_navigation_query.h"

#include <shlwapi.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <unordered_set>

namespace
{
    bool IsExtendedKeyNameVirtualKey(UINT virtualKey)
    {
        switch (virtualKey)
        {
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_LEFT:
        case VK_UP:
        case VK_RIGHT:
        case VK_DOWN:
        case VK_NUMLOCK:
        case VK_DIVIDE:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    }

    /**
     * @brief 从 JSON 文本中读取布尔字段值
     * @details 在给定的 JSON 字符串中查找指定字段名，提取其后的 true/false 值
     * @param text  JSON 格式的文本字符串
     * @param field 字段名（不含引号）
     * @param out   输出参数，存储读取到的布尔值
     * @return true  成功找到并解析字段值
     * @return false 未找到字段或格式错误
     */
    bool ReadBoolField(const std::string& text, const char* field, bool& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        if (text.compare(p, 4, "true") == 0) { out = true; return true; }
        if (text.compare(p, 5, "false") == 0) { out = false; return true; }
        return false;
    }

    /**
     * @brief 从 JSON 文本中读取整数字段值
     * @details 在给定的 JSON 字符串中查找指定字段名，提取其后的整数值
     * @param text  JSON 格式的文本字符串
     * @param field 字段名（不含引号）
     * @param out   输出参数，存储读取到的整数值
     * @return true  成功找到并解析字段值
     * @return false 未找到字段或格式错误
     */
    bool ReadIntField(const std::string& text, const char* field, int& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        out = std::atoi(text.c_str() + p);
        return true;
    }

    bool ReadStringField(
        const std::string& text, const char* field,
        std::string& out)
    {
        const std::string marker =
            "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find('"', p + 1);
        if (p == std::string::npos) return false;
        const size_t end = text.find('"', p + 1);
        if (end == std::string::npos) return false;
        out = text.substr(p + 1, end - p - 1);
        return true;
    }

    /**
     * @brief 将虚拟键码转换为可读的键名
     * @details 将 Windows 虚拟键码映射为对应的文本表示。
     *          字母键和数字键直接返回字符，功能键返回 F1~F24 格式，
     *          特殊键（Space、Tab 等）返回英文名称，无法识别的返回 "VK n"
     * @param vk 虚拟键码（VK_* 常量或 ASCII 码）
     * @return std::wstring 可读的键名字符串
     */
    std::wstring KeyName(UINT vk)
    {
        if (vk >= 'A' && vk <= 'Z')
            return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= '0' && vk <= '9')
            return std::wstring(1, static_cast<wchar_t>(vk));
        if (vk >= VK_F1 && vk <= VK_F24)
            return L"F" + std::to_wstring(vk - VK_F1 + 1);
        switch (vk)
        {
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_RETURN: return L"Enter";
        case VK_ESCAPE: return L"Esc";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_OEM_3: return L"` / ~";
        default:
            break;
        }

        const UINT scanCode =
            MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
        if (scanCode != 0)
        {
            LONG keyNameParam =
                static_cast<LONG>((scanCode & 0xFF) << 16);
            if ((scanCode & 0xFF00) != 0 ||
                IsExtendedKeyNameVirtualKey(vk))
                keyNameParam |= 1 << 24;
            wchar_t keyName[64]{};
            if (GetKeyNameTextW(
                    keyNameParam, keyName,
                    static_cast<int>(
                        sizeof(keyName) / sizeof(keyName[0]))) > 0)
                return keyName;
        }
        return L"VK " + std::to_wstring(vk);
    }
}

/**
 * @brief 获取导航设置文件路径
 * @details 通过统一数据路径获取 data 目录下的
 *          "SnowDesktop.navigation.json" 作为设置文件的完整路径
 * @return std::wstring 设置文件的绝对路径
 */
std::wstring GetNavigationSettingsPath()
{
    return GetDataFilePath(L"SnowDesktop.navigation.json");
}

/**
 * @brief 从文件加载导航设置
 * @details 以二进制方式读取指定 JSON 文件，解析其中的 enabled（布尔）、
 *          modifiers（整数）和 virtualKey（整数）字段，并写入 settings 结构体。
 *          若文件不存在、内容为空或字段缺失，对应字段保持默认值
 * @param path     设置文件的完整路径
 * @param settings 输出参数，接收加载的导航设置数据
 * @return true  加载成功（文件存在且非空）
 * @return false 加载失败（文件不存在或内容为空）
 */
bool LoadNavigationSettings(const wchar_t* path, NavigationSettings& settings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;
    JsonValue document;
    if (!ParseJson(text, document) || !document.IsObject()) return false;
    settings = NavigationSettings{};

    bool enabled = false;
    int modifiers = 0;
    int virtualKey = 0;
    std::string desktopViewMode;
    settings.desktopViewMode =
        QuickNavigationDesktopViewMode::Tile;
    if (ReadBoolField(text, "enabled", enabled))
        settings.enabled = enabled;
    if (ReadIntField(text, "modifiers", modifiers))
        settings.modifiers = static_cast<UINT>(modifiers);
    if (ReadIntField(text, "virtualKey", virtualKey) && virtualKey > 0)
        settings.virtualKey = static_cast<UINT>(virtualKey);
    QuickNavigationDesktopViewMode parsedMode{};
    if (ReadStringField(
            text, "desktopViewMode",
            desktopViewMode) &&
        QuickNavigationDesktopViewModeFromJson(
            desktopViewMode, parsedMode))
        settings.desktopViewMode = parsedMode;
    // Seed the remembered state from the former opening preference once.
    const auto* collapsed = document.Find("lastCollapsed");
    if (!collapsed || !collapsed->IsBoolean()) collapsed = document.Find("defaultCollapsed");
    if (collapsed && collapsed->IsBoolean()) settings.lastCollapsed = collapsed->boolean;
    if (const auto* layout = document.Find("layout"); layout && layout->IsObject())
    {
#define SD_NAV_READ(name) if (const auto* value = layout->Find(#name); value && value->IsNumber() && std::isfinite(value->number) && value->number >= -32768 && value->number <= 32768) settings.layout.name = static_cast<int>(value->number)
        SD_NAV_READ(expandedWidth); SD_NAV_READ(collapsedWidth); SD_NAV_READ(maximumHeight);
        SD_NAV_READ(visibleRows); SD_NAV_READ(padding); SD_NAV_READ(searchHeight);
        SD_NAV_READ(iconSize); SD_NAV_READ(gridGap); SD_NAV_READ(rowGap);
        SD_NAV_READ(fontSize); SD_NAV_READ(secondaryFontSize); SD_NAV_READ(searchFontSize);
        SD_NAV_READ(resultRowHeight); SD_NAV_READ(labelLines);
        SD_NAV_READ(cornerRadius); SD_NAV_READ(searchRadius); SD_NAV_READ(tabRadius); SD_NAV_READ(itemRadius);
#undef SD_NAV_READ
        const auto* layoutVersion = document.Find("layoutVersion");
        if ((!layoutVersion || (layoutVersion->IsNumber() && layoutVersion->number < 2)) && settings.layout.iconSize == 48)
            settings.layout.iconSize = QuickNavigationLayout{}.iconSize;
        // Only the unreleased layout's default radii migrate. Explicit values
        // in the current layout version and all other customization survive.
        if (!document.Find("layoutVersion"))
        {
            if (settings.layout.cornerRadius == 16) settings.layout.cornerRadius = QuickNavigationLayout{}.cornerRadius;
            if (settings.layout.searchRadius == 10) settings.layout.searchRadius = QuickNavigationLayout{}.searchRadius;
            if (settings.layout.tabRadius == 8) settings.layout.tabRadius = QuickNavigationLayout{}.tabRadius;
            if (settings.layout.itemRadius == 10) settings.layout.itemRadius = QuickNavigationLayout{}.itemRadius;
        }
    }
    if (const auto* value = document.Find("prefixes"); value && value->IsArray() && value->array.size() == settings.prefixes.size())
        for (size_t i = 0; i < value->array.size(); ++i) if (value->array[i].IsString()) settings.prefixes[i] = value->array[i].string;
    // Migrate the previous trial default once; explicitly saved current prefixes survive.
    if (!document.Find("prefixesVersion") && settings.prefixes.back() == "calc") settings.prefixes.back() = "=";
    if (const auto* value = document.Find("defaultEngine"); value && value->IsString()) settings.defaultEngine = value->string;
    if (const auto* value = document.Find("engines"); value && value->IsArray())
    {
        std::vector<QuickNavigationSearchEngine> engines;
        for (const auto& entry : value->array)
        {
            QuickNavigationSearchEngine engine;
            auto field = [&](const char* key, std::string& out) { if (const auto* v = entry.Find(key); v && v->IsString()) out = v->string; };
            field("id", engine.id); field("name", engine.name); field("prefix", engine.prefix); field("url", engine.url);
            engines.push_back(std::move(engine));
        }
        settings.engines = std::move(engines);
    }
    if (const auto* value = document.Find("colors"); value && value->IsObject())
        for (const auto& [key, color] : value->object) if (color.IsString()) settings.colors[key] = color.string;
    NormalizeNavigationSettings(settings);
    return true;
}

/**
 * @brief 保存导航设置到文件
 * @details 将 NavigationSettings 结构体序列化为 JSON 格式，
 *          以二进制截写模式写入指定路径。生成的 JSON 包含
 *          enabled、modifiers 和 virtualKey 三个字段
 * @param path     目标设置文件的完整路径
 * @param settings 待保存的导航设置数据
 * @return true  保存成功
 * @return false 保存失败（文件无法创建或写入）
 */
bool SaveNavigationSettings(const wchar_t* path, const NavigationSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"layoutVersion\": 2,\n";
    file << "  \"enabled\": " << (settings.enabled ? "true" : "false") << ",\n";
    file << "  \"modifiers\": " << settings.modifiers << ",\n";
    file << "  \"virtualKey\": " << settings.virtualKey << ",\n";
    file << "  \"desktopViewMode\": \""
         << QuickNavigationDesktopViewModeToJson(
                settings.desktopViewMode)
         << "\",\n";
    file << "  \"lastCollapsed\": " << (settings.lastCollapsed ? "true" : "false") << ",\n  \"layout\": {";
    bool first = true;
#define SD_NAV_WRITE(name) if (!first) file << ','; first = false; file << "\"" #name "\":" << settings.layout.name
    SD_NAV_WRITE(expandedWidth); SD_NAV_WRITE(collapsedWidth); SD_NAV_WRITE(maximumHeight);
    SD_NAV_WRITE(visibleRows); SD_NAV_WRITE(padding); SD_NAV_WRITE(searchHeight);
    SD_NAV_WRITE(iconSize); SD_NAV_WRITE(gridGap); SD_NAV_WRITE(rowGap);
    SD_NAV_WRITE(fontSize); SD_NAV_WRITE(secondaryFontSize); SD_NAV_WRITE(searchFontSize);
    SD_NAV_WRITE(resultRowHeight); SD_NAV_WRITE(labelLines);
    SD_NAV_WRITE(cornerRadius); SD_NAV_WRITE(searchRadius); SD_NAV_WRITE(tabRadius); SD_NAV_WRITE(itemRadius);
#undef SD_NAV_WRITE
    const auto quoted = [](const std::string& input) {
        std::string out = "\"";
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned char c : input)
        {
            if (c == '\\' || c == '"') { out += '\\'; out += static_cast<char>(c); }
            else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
            else out += static_cast<char>(c);
        }
        return out + '"';
    };
    file << "},\n  \"prefixesVersion\": 1,\n  \"prefixes\": [";
    for (size_t i = 0; i < settings.prefixes.size(); ++i) { if (i) file << ','; file << quoted(settings.prefixes[i]); }
    file << "],\n  \"defaultEngine\": " << quoted(settings.defaultEngine) << ",\n  \"engines\": [";
    for (size_t i = 0; i < settings.engines.size(); ++i)
    {
        if (i) file << ',';
        const auto& e = settings.engines[i];
        file << "{\"id\":" << quoted(e.id) << ",\"name\":" << quoted(e.name)
             << ",\"prefix\":" << quoted(e.prefix) << ",\"url\":" << quoted(e.url) << '}';
    }
    file << "],\n  \"colors\": {"; first = true;
    for (const auto& [key, value] : settings.colors) { if (!first) file << ','; first = false; file << quoted(key) << ':' << quoted(value); }
    file << "}\n";
    file << "}\n";
    return file.good();
}

/**
 * @brief 格式化导航热键文本
 * @details 将 NavigationSettings 中的修饰键位（Ctrl、Alt、Shift、Win）
 *          与虚拟键码拼接为人可读的热键字符串，各部分以 " + " 分隔。
 *          例如 Ctrl + Alt + Space、Ctrl + Shift + F1
 * @param settings 导航设置数据
 * @return std::wstring 格式化后的热键文本，如 "Ctrl + Alt + Space"
 */
std::wstring FormatNavigationHotkey(const NavigationSettings& settings)
{
    std::wstring text;
    auto append = [&](const wchar_t* part) {
        if (!text.empty()) text += L" + ";
        text += part;
    };
    if (settings.modifiers & MOD_CONTROL) append(L"Ctrl");
    if (settings.modifiers & MOD_ALT) append(L"Alt");
    if (settings.modifiers & MOD_SHIFT) append(L"Shift");
    if (settings.modifiers & MOD_WIN) append(L"Win");
    if (settings.virtualKey != 0)
        append(KeyName(settings.virtualKey).c_str());
    return text;
}
