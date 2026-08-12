/**
 * @file general_settings.cpp
 * @brief 通用设置的实现
 * @details 提供通用设置 JSON 文件的路径获取、加载、保存。
 */

#include "general_settings.h"
#include "data_paths.h"

#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace
{
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

    bool ReadIntField(const std::string& text, const char* field, int& out)
    {
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find_first_not_of(" \t\r\n", p + 1);
        if (p == std::string::npos) return false;
        try { out = std::stoi(text.substr(p)); return true; }
        catch (...) { return false; }
    }

    bool ReadStringField(const std::string& text, const char* field, char* out, size_t outSize)
    {
        if (!out || outSize == 0) return false;
        std::string marker = "\"" + std::string(field) + "\"";
        size_t p = text.find(marker);
        if (p == std::string::npos) return false;
        p = text.find(':', p);
        if (p == std::string::npos) return false;
        p = text.find('"', p + 1);
        if (p == std::string::npos) return false;
        size_t end = text.find('"', p + 1);
        if (end == std::string::npos) return false;
        std::string value = text.substr(p + 1, end - p - 1);
        const size_t copyLength = std::min(value.size(), outSize - 1);
        std::memcpy(out, value.data(), copyLength);
        out[copyLength] = '\0';
        return true;
    }

}

std::wstring GetGeneralSettingsPath()
{
    return GetDataFilePath(L"SnowDesktop.general.json");
}

bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    bool val = false;
    if (ReadBoolField(text, "softwareDesktopEnabled", val))
        settings.softwareDesktopEnabled = val;
    if (ReadBoolField(text, "demoModeEnabled", val))
        settings.demoModeEnabled = val;
    if (ReadBoolField(text, "doubleClickHideDesktop", val))
        settings.doubleClickHideDesktop = val;
    if (ReadBoolField(text, "desktopPassthroughHotkeyEnabled", val))
        settings.desktopPassthroughHotkeyEnabled = val;
    if (ReadBoolField(text, "widgetDeveloperToolsEnabled", val))
        settings.widgetDeveloperToolsEnabled = val;
    int hotkeyValue = 0;
    if (ReadIntField(text, "desktopPassthroughHotkeyModifiers",
        hotkeyValue))
    {
        settings.desktopPassthroughHotkeyModifiers =
            static_cast<UINT>(hotkeyValue) &
            (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
    }
    if (ReadIntField(text, "desktopPassthroughHotkeyVirtualKey",
        hotkeyValue) &&
        hotkeyValue > 0 && hotkeyValue <= 0xFF)
    {
        settings.desktopPassthroughHotkeyVirtualKey =
            static_cast<UINT>(hotkeyValue);
    }
    int theme = 0;
    if (ReadIntField(text, "quickNavTheme", theme))
    {
        if (theme >= 4) theme -= 2;
        settings.quickNavTheme = std::clamp(theme, 0, 3);
    }
    int agentSkillTargetMask = 0;
    if (ReadIntField(text, "agentSkillTargetMask", agentSkillTargetMask) &&
        agentSkillTargetMask >= 0 &&
        agentSkillTargetMask <= GeneralSettings::kAllAgentSkillTargetsMask)
    {
        settings.agentSkillTargetMask = agentSkillTargetMask;
    }
    ReadStringField(text, "language", settings.language, sizeof(settings.language));
    return true;
}

bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"softwareDesktopEnabled\": "
         << (settings.softwareDesktopEnabled ? "true" : "false") << ",\n";
    file << "  \"demoModeEnabled\": "
         << (settings.demoModeEnabled ? "true" : "false") << ",\n";
    file << "  \"doubleClickHideDesktop\": " << (settings.doubleClickHideDesktop ? "true" : "false") << ",\n";
    file << "  \"desktopPassthroughHotkeyEnabled\": "
         << (settings.desktopPassthroughHotkeyEnabled ? "true" : "false")
         << ",\n";
    file << "  \"desktopPassthroughHotkeyModifiers\": "
         << settings.desktopPassthroughHotkeyModifiers << ",\n";
    file << "  \"desktopPassthroughHotkeyVirtualKey\": "
         << settings.desktopPassthroughHotkeyVirtualKey << ",\n";
    file << "  \"quickNavTheme\": " << settings.quickNavTheme << ",\n";
    file << "  \"widgetDeveloperToolsEnabled\": "
         << (settings.widgetDeveloperToolsEnabled ? "true" : "false")
         << ",\n";
    file << "  \"agentSkillTargetMask\": "
         << settings.agentSkillTargetMask << ",\n";
    file << "  \"language\": \"" << settings.language << "\"\n";
    file << "}\n";
    return true;
}
