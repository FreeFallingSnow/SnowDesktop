#include "general_settings.h"
#include "personalization.h"

#include <windows.h>

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

std::wstring GetDataFilePath(const wchar_t* filename)
{
    return filename ? std::wstring(filename) : std::wstring{};
}

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopGeneralSettingsTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path, error);

    GeneralSettings saved;
    saved.demoModeEnabled = true;
    saved.widgetDeveloperToolsEnabled = true;
    saved.agentSkillTargetMask = 0x15;
    saved.quickNavTheme = kFourThemeAcrylicDark;
    saved.collectionPopupTheme = kFourThemeAcrylicLight;
    saved.pageNavigationKeyboardEnabled = false;
    saved.pageNavigationPreviousModifiers = MOD_CONTROL;
    saved.pageNavigationPreviousVirtualKey = VK_HOME;
    saved.pageNavigationNextModifiers = MOD_ALT;
    saved.pageNavigationNextVirtualKey = VK_END;
    strcpy_s(saved.language, "zh-CN");
    Check(SaveGeneralSettings(path.c_str(), saved),
        "general settings save succeeds");

    GeneralSettings loaded;
    Check(LoadGeneralSettings(path.c_str(), loaded),
        "general settings load succeeds");
    Check(loaded.demoModeEnabled &&
        loaded.widgetDeveloperToolsEnabled &&
        loaded.agentSkillTargetMask == 0x15 &&
        loaded.quickNavTheme == kFourThemeAcrylicDark &&
        loaded.collectionPopupTheme == kFourThemeAcrylicLight &&
        !loaded.pageNavigationKeyboardEnabled &&
        loaded.pageNavigationPreviousModifiers == MOD_CONTROL &&
        loaded.pageNavigationPreviousVirtualKey == VK_HOME &&
        loaded.pageNavigationNextModifiers == MOD_ALT &&
        loaded.pageNavigationNextVirtualKey == VK_END &&
        std::strcmp(loaded.language, "zh-CN") == 0,
        "general flags, page keys, four-theme selections, and Agent Skill targets persist");

    {
        std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
        invalid << "{\n"
                   "  \"collectionPopupTheme\": 99,\n"
                   "  \"pageNavigationPreviousVirtualKey\": 999,\n"
                   "  \"pageNavigationNextVirtualKey\": -1,\n"
                   "  \"language\": \"system\"\n"
                   "}\n";
    }
    GeneralSettings clamped;
    Check(LoadGeneralSettings(path.c_str(), clamped) &&
            clamped.collectionPopupTheme == kFourThemeAcrylicLight &&
            clamped.pageNavigationPreviousVirtualKey == VK_PRIOR &&
            clamped.pageNavigationNextVirtualKey == VK_NEXT,
        "general settings reject invalid persisted themes and page keys");

    {
        std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
        legacy << "{\n  \"language\": \"system\"\n}\n";
    }
    GeneralSettings migrated;
    Check(LoadGeneralSettings(path.c_str(), migrated),
        "legacy general settings still load");
    Check(!migrated.demoModeEnabled &&
        !migrated.widgetDeveloperToolsEnabled &&
        migrated.collectionPopupTheme == kFourThemeDark &&
        migrated.pageNavigationKeyboardEnabled &&
        migrated.pageNavigationPreviousModifiers == 0 &&
        migrated.pageNavigationPreviousVirtualKey == VK_PRIOR &&
        migrated.pageNavigationNextModifiers == 0 &&
        migrated.pageNavigationNextVirtualKey == VK_NEXT &&
        migrated.agentSkillTargetMask ==
            GeneralSettings::kAllAgentSkillTargetsMask,
        "legacy settings preserve the dark popup and page-navigation defaults");

    Check(FourThemeSelectionFromAppearancePreset(
            kAppearancePresetDark) == kFourThemeDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetLight) == kFourThemeLight &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetGlassDark) == kFourThemeAcrylicDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetAcrylicDark) == kFourThemeAcrylicDark &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetGlassLight) == kFourThemeAcrylicLight &&
        FourThemeSelectionFromAppearancePreset(
            kAppearancePresetAcrylicLight) == kFourThemeAcrylicLight,
        "six global themes map to the four overlay themes");
    Check(AppearancePresetFromFourThemeSelection(kFourThemeDark) ==
            kAppearancePresetDark &&
        AppearancePresetFromFourThemeSelection(kFourThemeLight) ==
            kAppearancePresetLight &&
        AppearancePresetFromFourThemeSelection(kFourThemeAcrylicDark) ==
            kAppearancePresetAcrylicDark &&
        AppearancePresetFromFourThemeSelection(kFourThemeAcrylicLight) ==
            kAppearancePresetAcrylicLight,
        "four overlay themes map back to stable appearance preset IDs");

    const auto darkPreset = PersonalizationSettings::DarkPreset();
    const auto lightPreset = PersonalizationSettings::LightPreset();
    const auto glassDarkPreset =
        PersonalizationSettings::GlassDarkPreset();
    const auto glassLightPreset =
        PersonalizationSettings::GlassLightPreset();
    const auto acrylicDarkPreset =
        PersonalizationSettings::AcrylicDarkPreset();
    const auto acrylicLightPreset =
        PersonalizationSettings::AcrylicLightPreset();
    Check(darkPreset.widgetBorderStyle == PanelBorderStyle::Standard &&
            lightPreset.widgetBorderStyle == PanelBorderStyle::Standard &&
            darkPreset.widgetBorderWidth == 1.0f &&
            lightPreset.widgetBorderWidth == 1.0f,
        "ordinary appearance presets keep the one-pixel standard border");
    Check(glassDarkPreset.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            glassLightPreset.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            acrylicDarkPreset.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            acrylicLightPreset.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            glassDarkPreset.widgetBorderWidth ==
                kDefaultDimensionalBorderWidth &&
            acrylicLightPreset.widgetBorderWidth ==
                kDefaultDimensionalBorderWidth &&
            glassLightPreset.widgetBorderEffectStrength ==
                kDefaultDimensionalBorderStrength &&
            acrylicDarkPreset.widgetBorderEffectStrength ==
                kDefaultDimensionalBorderStrength,
        "glass and acrylic presets load the recommended dimensional border");

    const auto personalizationPath =
        std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopPersonalizationTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(personalizationPath, error);
    PersonalizationSettings savedAppearance =
        PersonalizationSettings::DarkPreset();
    savedAppearance.backgroundPreset = kAppearancePresetCustom;
    savedAppearance.glassEnabled = false;
    savedAppearance.acrylicEnabled = false;
    savedAppearance.widgetBorderStyle = PanelBorderStyle::Dimensional;
    savedAppearance.widgetBorderWidth = 3.5f;
    savedAppearance.widgetBorderEffectStrength = 0.42f;
    Check(SavePersonalization(
            personalizationPath.c_str(), savedAppearance),
        "personalization save succeeds");
    PersonalizationSettings loadedAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), loadedAppearance) &&
            loadedAppearance.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            loadedAppearance.widgetBorderWidth == 3.5f &&
            std::abs(loadedAppearance.widgetBorderEffectStrength -
                0.42f) < 0.0001f &&
            !loadedAppearance.glassEnabled,
        "border style, width, and strength round trip independently from glass");

    {
        std::ofstream legacyGlass(
            personalizationPath, std::ios::binary | std::ios::trunc);
        legacyGlass << "{\n"
                       "  \"backgroundPreset\": 9,\n"
                       "  \"glassEnabled\": true\n"
                       "}\n";
    }
    PersonalizationSettings migratedGlass;
    Check(LoadPersonalization(personalizationPath.c_str(), migratedGlass) &&
            migratedGlass.widgetBorderStyle ==
                PanelBorderStyle::Dimensional &&
            migratedGlass.widgetBorderWidth ==
                kDefaultDimensionalBorderWidth &&
            migratedGlass.widgetBorderEffectStrength ==
                kDefaultDimensionalBorderStrength,
        "legacy glass appearance migrates to the recommended dimensional border");
    {
        std::ofstream legacyOpaque(
            personalizationPath, std::ios::binary | std::ios::trunc);
        legacyOpaque << "{\n"
                        "  \"backgroundPreset\": 9,\n"
                        "  \"glassEnabled\": false\n"
                        "}\n";
    }
    PersonalizationSettings migratedOpaque;
    Check(LoadPersonalization(personalizationPath.c_str(), migratedOpaque) &&
            migratedOpaque.widgetBorderStyle ==
                PanelBorderStyle::Standard &&
            migratedOpaque.widgetBorderWidth == 1.0f,
        "legacy non-glass appearance migrates to the standard border");
    {
        std::ofstream explicitAcrylic(
            personalizationPath, std::ios::binary | std::ios::trunc);
        explicitAcrylic << "{\n"
                           "  \"backgroundPreset\": 10,\n"
                           "  \"glassEnabled\": true,\n"
                           "  \"acrylicEnabled\": true,\n"
                           "  \"widgetBorderStyle\": 0,\n"
                           "  \"widgetBorderWidth\": 99,\n"
                           "  \"widgetBorderEffectStrength\": -1\n"
                           "}\n";
    }
    PersonalizationSettings explicitAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), explicitAppearance) &&
            explicitAppearance.widgetBorderStyle ==
                PanelBorderStyle::Standard &&
            explicitAppearance.widgetBorderWidth ==
                kMaximumWidgetBorderWidth &&
            explicitAppearance.widgetBorderEffectStrength == 0.0f,
        "explicit acrylic border fields take priority and clamp to supported ranges");
    std::filesystem::remove(personalizationPath, error);

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
