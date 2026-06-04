#pragma once

#include <windows.h>

#include <string>

struct NavigationSettings
{
    bool enabled = false;
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT virtualKey = VK_SPACE;
};

std::wstring GetNavigationSettingsPath();
bool LoadNavigationSettings(const wchar_t* path, NavigationSettings& settings);
bool SaveNavigationSettings(const wchar_t* path, const NavigationSettings& settings);
std::wstring FormatNavigationHotkey(const NavigationSettings& settings);
