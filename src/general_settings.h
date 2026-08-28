/**
 * @file general_settings.h
 * @brief 通用设置
 * @details 定义通用设置数据结构（双击空白处隐藏桌面等），
 *          以及设置文件的读写函数。
 */

#pragma once

#include <windows.h>

#include <string>

struct GeneralSettings
{
    static constexpr int kAllAgentSkillTargetsMask = 0x3F;

    // Runtime projection of SnowDesktop's unified per-user logon task. The
    // scheduled task remains the source of truth; this field is deliberately
    // not serialized into SnowDesktop.general.json.
    bool autoStartEnabled = false;
    bool softwareDesktopEnabled = true;
    bool demoModeEnabled = false;
    bool doubleClickHideDesktop = false;
    bool desktopPassthroughHotkeyEnabled = false;
    UINT desktopPassthroughHotkeyModifiers = MOD_CONTROL | MOD_ALT;
    UINT desktopPassthroughHotkeyVirtualKey = VK_OEM_3;
    bool pageNavigationKeyboardEnabled = true;
    UINT pageNavigationPreviousModifiers = 0;
    UINT pageNavigationPreviousVirtualKey = VK_PRIOR;
    UINT pageNavigationNextModifiers = 0;
    UINT pageNavigationNextVirtualKey = VK_NEXT;
    int quickNavTheme = 1;
    // 0=dark, 1=light, 2=dark acrylic, 3=light acrylic.
    // Dark preserves the legacy collection-popup appearance when absent.
    int collectionPopupTheme = 0;
    bool dockEnabled = false;
    bool widgetDeveloperToolsEnabled = false;
    int agentSkillTargetMask = kAllAgentSkillTargetsMask;
    char language[85] = "system";
};

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
