#include "navigation_settings.h"

#include <shlwapi.h>

#include <cstdlib>
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
        out = std::atoi(text.c_str() + p);
        return true;
    }

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

std::wstring GetNavigationSettingsPath()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    PathRemoveFileSpecW(path);
    PathAppendW(path, L"SnowDesktop.navigation.json");
    return path;
}

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
