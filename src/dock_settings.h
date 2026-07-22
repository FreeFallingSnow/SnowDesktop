#pragma once

#include "personalization.h"

#include <algorithm>
#include <string>

constexpr float kDockMinimumScale = 0.50f;
constexpr float kDockMaximumScale = 1.00f;

inline float ClampDockScale(float scale)
{
    return std::clamp(scale, kDockMinimumScale, kDockMaximumScale);
}

enum class DockPosition
{
    Bottom = 0,
    Top = 1,
    Left = 2,
    Right = 3
};

enum class DockMonitorScope
{
    First = 0,
    Last = 1,
    All = 2
};

enum class SystemTaskbarBackdropRuntimeState
{
    Disabled,
    Loading,
    Active,
    Unsupported,
    Failed
};

struct DockSettings
{
    DockPosition position = DockPosition::Bottom;
    bool edgeAttached = false;
    DockMonitorScope monitorScope = DockMonitorScope::First;
    bool showWindowsButton = true;
    bool showRunningApps = true;
    bool showFrequentItems = false;
    int frequentItemCount = 3;
    float thicknessScale = 1.0f;
    bool systemTaskbarAutoHide = false;
    bool systemTaskbarBackdropEnabled = false;
    bool systemTaskbarFollowPersonalization = true;
    PersonalizationSettings systemTaskbarAppearance =
        PersonalizationSettings::GlassDarkPreset();
};

std::wstring GetDockSettingsPath();
bool IsSystemTaskbarAutoHideEnabled();
bool SetSystemTaskbarAutoHideEnabled(bool enabled);
bool IsWindowsSystemLightThemeEnabled();
bool SetWindowsSystemLightThemeEnabled(bool enabled);
bool RestartWindowsExplorer();
SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState();
void NotifySystemTaskbarCreated();
bool ApplySystemTaskbarBackdrop(bool enabled,
    const PersonalizationSettings& appearance);
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
