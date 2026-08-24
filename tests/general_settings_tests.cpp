#include "general_settings.h"
#include "personalization.h"

#include <windows.h>

#include <cstring>
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
    saved.settingsWindowTheme = SettingsWindowTheme::Dark;
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
        loaded.settingsWindowTheme == SettingsWindowTheme::Dark &&
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
        std::ofstream invalidTheme(path,
            std::ios::binary | std::ios::trunc);
        invalidTheme << "{\n  \"settingsWindowTheme\": 99,\n"
            "  \"language\": \"system\"\n}\n";
    }
    GeneralSettings invalidWindowTheme;
    Check(LoadGeneralSettings(path.c_str(), invalidWindowTheme) &&
            invalidWindowTheme.settingsWindowTheme ==
                SettingsWindowTheme::System,
        "invalid settings-window themes fall back to system");

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
        migrated.settingsWindowTheme == SettingsWindowTheme::System &&
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

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
