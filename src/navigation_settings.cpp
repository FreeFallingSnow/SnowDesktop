/**
 * @file navigation_settings.cpp
 * @brief 快捷导航面板设置的实现
 * @details 提供导航设置 JSON 文件的路径获取、加载、保存，
 *          以及热键格式化的具体实现。内部使用匿名命名空间中的
 *          辅助函数完成 JSON 字段的解析和虚拟键码的名称转换。
 */

#include "navigation_settings.h"

#include <shlwapi.h>

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace
{
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
        case VK_OEM_3: return L"`";
        default: return L"VK " + std::to_wstring(vk);
        }
    }
}

/**
 * @brief 获取导航设置文件路径
 * @details 通过获取当前可执行文件所在目录，在其下拼接
 *          "SnowDesktop.navigation.json" 作为设置文件的完整路径
 * @return std::wstring 设置文件的绝对路径
 */
std::wstring GetNavigationSettingsPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.navigation.json");
    return path;
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

    bool enabled = false;
    int modifiers = 0;
    int virtualKey = 0;
    if (ReadBoolField(text, "enabled", enabled))
        settings.enabled = enabled;
    if (ReadIntField(text, "modifiers", modifiers))
        settings.modifiers = static_cast<UINT>(modifiers);
    if (ReadIntField(text, "virtualKey", virtualKey) && virtualKey > 0)
        settings.virtualKey = static_cast<UINT>(virtualKey);
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
    file << "  \"enabled\": " << (settings.enabled ? "true" : "false") << ",\n";
    file << "  \"modifiers\": " << settings.modifiers << ",\n";
    file << "  \"virtualKey\": " << settings.virtualKey << "\n";
    file << "}\n";
    return true;
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
    if (!text.empty()) text += L" + ";
    text += KeyName(settings.virtualKey);
    return text;
}
