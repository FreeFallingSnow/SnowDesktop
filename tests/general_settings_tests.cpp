#include "general_settings.h"
#include "personalization.h"

#include <windows.h>

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

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
        loaded.quickNavTheme == kFourThemeAcrylicDark &&
        loaded.collectionPopupTheme == kFourThemeAcrylicLight &&
        !loaded.pageNavigationKeyboardEnabled &&
        loaded.pageNavigationPreviousModifiers == MOD_CONTROL &&
        loaded.pageNavigationPreviousVirtualKey == VK_HOME &&
        loaded.pageNavigationNextModifiers == MOD_ALT &&
        loaded.pageNavigationNextVirtualKey == VK_END &&
        std::strcmp(loaded.language, "zh-CN") == 0,
        "general flags, page keys, and four-theme selections persist");

    {
        std::ifstream persisted(path, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(persisted)),
            std::istreambuf_iterator<char>());
        Check(text.find("agentSkillTargetMask") == std::string::npos,
            "detected Agent Skill installations are not persisted as settings");
    }

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
        legacy << "{\n"
                  "  \"agentSkillTargetMask\": 21,\n"
                  "  \"language\": \"system\"\n"
                  "}\n";
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
        migrated.pageNavigationNextVirtualKey == VK_NEXT,
        "legacy settings ignore the obsolete Agent Skill mask and preserve defaults");

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
    Check(!darkPreset.widgetEdgeHighlightEnabled &&
            !lightPreset.widgetEdgeHighlightEnabled &&
            darkPreset.widgetBorderWidth == 1.0f &&
            lightPreset.widgetBorderWidth == 1.0f,
        "ordinary appearance presets keep a one-pixel border without edge highlight");
    Check(glassDarkPreset.widgetEdgeHighlightEnabled &&
            glassLightPreset.widgetEdgeHighlightEnabled &&
            acrylicDarkPreset.widgetEdgeHighlightEnabled &&
            acrylicLightPreset.widgetEdgeHighlightEnabled &&
            glassDarkPreset.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            acrylicLightPreset.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            glassDarkPreset.widgetBorderAlpha == 0.0f &&
            acrylicLightPreset.widgetBorderAlpha == 0.0f &&
            glassLightPreset.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            acrylicDarkPreset.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength,
        "glass and acrylic presets disable the border and load the recommended edge highlight");

    const auto quickNavigationAcrylic =
        MakeQuickNavigationAppearancePreset(
            kAppearancePresetAcrylicDark);
    const auto collectionPopupAcrylic =
        MakeCollectionPopupAppearancePreset(
            kAppearancePresetAcrylicDark);
    const auto collectionPopupDark =
        MakeCollectionPopupAppearancePreset(
            kAppearancePresetDark);
    Check(quickNavigationAcrylic.widgetBorderAlpha > 0.0f &&
            quickNavigationAcrylic.widgetEdgeHighlightEnabled &&
            collectionPopupAcrylic.widgetBorderAlpha == 0.0f &&
            collectionPopupAcrylic.widgetEdgeHighlightEnabled &&
            collectionPopupAcrylic.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            collectionPopupAcrylic.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            collectionPopupDark.widgetBorderAlpha > 0.0f &&
            !collectionPopupDark.widgetEdgeHighlightEnabled,
        "collection acrylic keeps the popup palette but replaces the Quick Navigation outline with an independent edge highlight");

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
    savedAppearance.widgetBorderWidth = 3.5f;
    savedAppearance.widgetEdgeHighlightEnabled = true;
    savedAppearance.widgetEdgeHighlightWidth = 2.5f;
    savedAppearance.widgetEdgeHighlightStrength = 0.42f;
    Check(SavePersonalization(
            personalizationPath.c_str(), savedAppearance),
        "personalization save succeeds");
    PersonalizationSettings loadedAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), loadedAppearance) &&
            loadedAppearance.widgetBorderWidth == 3.5f &&
            loadedAppearance.widgetEdgeHighlightEnabled &&
            loadedAppearance.widgetEdgeHighlightWidth == 2.5f &&
            std::abs(loadedAppearance.widgetEdgeHighlightStrength -
                0.42f) < 0.0001f &&
            !loadedAppearance.glassEnabled,
        "border and edge-highlight fields round trip independently from glass");

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
            migratedGlass.widgetEdgeHighlightEnabled &&
            migratedGlass.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            migratedGlass.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            migratedGlass.widgetBorderWidth == 1.0f &&
            migratedGlass.widgetBorderAlpha == 0.0f,
        "legacy glass appearance migrates to an independent edge highlight");
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
            !migratedOpaque.widgetEdgeHighlightEnabled &&
            migratedOpaque.widgetBorderWidth == 1.0f,
        "legacy non-glass appearance keeps the ordinary border only");
    {
        std::ofstream explicitAcrylic(
            personalizationPath, std::ios::binary | std::ios::trunc);
        explicitAcrylic << "{\n"
                           "  \"backgroundPreset\": 10,\n"
                           "  \"glassEnabled\": true,\n"
                           "  \"acrylicEnabled\": true,\n"
                           "  \"widgetBorderStyle\": 1,\n"
                           "  \"widgetBorderWidth\": 99,\n"
                           "  \"widgetEdgeHighlightEnabled\": false,\n"
                           "  \"widgetEdgeHighlightWidth\": 99,\n"
                           "  \"widgetEdgeHighlightStrength\": -1\n"
                           "}\n";
    }
    PersonalizationSettings explicitAppearance;
    Check(LoadPersonalization(
            personalizationPath.c_str(), explicitAppearance) &&
            !explicitAppearance.widgetEdgeHighlightEnabled &&
            explicitAppearance.widgetBorderWidth ==
                kMaximumWidgetBorderWidth &&
            explicitAppearance.widgetEdgeHighlightWidth ==
                kMaximumWidgetBorderWidth &&
            explicitAppearance.widgetEdgeHighlightStrength == 0.0f,
        "explicit acrylic border and edge fields take priority and clamp to supported ranges");
    std::filesystem::remove(personalizationPath, error);

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
