/**
 * @file general_settings.cpp
 * @brief 通用设置的实现
 * @details 提供通用设置 JSON 文件的路径获取、加载、保存。
 */

#include "general_settings.h"

#include <shlwapi.h>

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
}

std::wstring GetGeneralSettingsPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.general.json");
    return path;
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
    if (ReadBoolField(text, "doubleClickHideDesktop", val))
        settings.doubleClickHideDesktop = val;
    int theme = 0;
    if (ReadIntField(text, "quickNavTheme", theme))
        settings.quickNavTheme = theme;
    return true;
}

bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"doubleClickHideDesktop\": " << (settings.doubleClickHideDesktop ? "true" : "false") << ",\n";
    file << "  \"quickNavTheme\": " << settings.quickNavTheme << "\n";
    file << "}\n";
    return true;
}
