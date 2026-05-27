#pragma once

#include <d2d1_1.h>
#include <string>

struct PersonalizationSettings
{
    // Widget background RGB
    float widgetBgR = 0.08f;
    float widgetBgG = 0.10f;
    float widgetBgB = 0.13f;

    // Widget border RGB
    float widgetBorderR = 1.0f;
    float widgetBorderG = 1.0f;
    float widgetBorderB = 1.0f;

    // Unified alpha: background fill, border, gradient start
    float widgetAlpha = 0.36f;

    // Gradient bottom end alpha
    float gradientEndA = 0.65f;

    static PersonalizationSettings DarkPreset();
    static PersonalizationSettings LightPreset();
};

// Load/save
bool LoadPersonalization(const wchar_t* path, PersonalizationSettings& s);
bool SavePersonalization(const wchar_t* path, const PersonalizationSettings& s);
std::wstring GetPersonalizationPath();
