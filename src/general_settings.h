/**
 * @file general_settings.h
 * @brief 通用设置
 * @details 定义通用设置数据结构（双击空白处隐藏桌面等），
 *          以及设置文件的读写函数。
 */

#pragma once

#include <string>

struct GeneralSettings
{
    bool softwareDesktopEnabled = true;
    bool doubleClickHideDesktop = false;
    int quickNavTheme = 1; // 全局自定义时：0=深色，1=浅色，2/3=对应毛玻璃
    bool dockEnabled = false;
};

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
