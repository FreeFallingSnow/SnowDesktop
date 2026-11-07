#pragma once

#include "general_settings_types.h"

void NormalizeGeneralAnimationSettings(GeneralSettings& settings) noexcept;

std::wstring GetGeneralSettingsPath();
bool LoadGeneralSettings(const wchar_t* path, GeneralSettings& settings);
bool SaveGeneralSettings(const wchar_t* path, const GeneralSettings& settings);
