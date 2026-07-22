#pragma once

#include "personalization.h"

#include <string>

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
    bool followPersonalization = true;
    bool showFrequentItems = false;
    int frequentItemCount = 3;
    bool systemTaskbarAutoHide = false;
    bool systemTaskbarBackdropEnabled = false;
    bool systemTaskbarFollowPersonalization = true;
    PersonalizationSettings appearance = PersonalizationSettings::DarkPreset();
    PersonalizationSettings systemTaskbarAppearance =
        PersonalizationSettings::GlassDarkPreset();
};

std::wstring GetDockSettingsPath();
bool IsSystemTaskbarAutoHideEnabled();
bool SetSystemTaskbarAutoHideEnabled(bool enabled);
bool IsWindowsSystemLightThemeEnabled();
bool SetWindowsSystemLightThemeEnabled(bool enabled);
SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState();
void NotifySystemTaskbarCreated();
bool ApplySystemTaskbarBackdrop(bool enabled,
    const PersonalizationSettings& appearance);
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
