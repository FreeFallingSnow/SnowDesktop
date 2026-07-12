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

struct DockSettings
{
    DockPosition position = DockPosition::Bottom;
    bool edgeAttached = false;
    bool followPersonalization = true;
    PersonalizationSettings appearance = PersonalizationSettings::DarkPreset();
};

std::wstring GetDockSettingsPath();
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
