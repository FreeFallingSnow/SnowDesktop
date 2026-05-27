#pragma once

#include <d2d1_1.h>
#include <string>

struct PersonalizationSettings
{
    // Widget background
    float widgetBgR = 0.08f;
    float widgetBgG = 0.10f;
    float widgetBgB = 0.13f;
    float widgetBgA = 0.36f;

    // Widget border
    float widgetBorderR = 1.0f;
    float widgetBorderG = 1.0f;
    float widgetBorderB = 1.0f;
    float widgetBorderA = 0.40f;

    // Gradient bottom
    float gradientStartA = 0.05f;
    float gradientEndA = 0.65f;

    static PersonalizationSettings DarkPreset();
    static PersonalizationSettings LightPreset();
};

// Load/save
bool LoadPersonalization(const wchar_t* path, PersonalizationSettings& s);
bool SavePersonalization(const wchar_t* path, const PersonalizationSettings& s);
std::wstring GetPersonalizationPath();
