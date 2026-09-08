#include "general_settings.h"
#include "personalization.h"
#include "dock_gradient_storage.h"

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
    {
        // A gradient must survive restart independently in every taskbar rule,
        // including its inactive colors after switching back to solid fill.
        DockSettings saved;
        auto gradients = snowdesktop::TaskbarGradients(saved);
        for (size_t i = 0; i < gradients.size(); ++i)
        {
            gradients[i]->enabled = i != 2;
            gradients[i]->angle = 37.5 + i * 45;
            gradients[i]->start = .125;
            gradients[i]->end = .875;
            gradients[i]->stops = {{0, 0xff2233, .15}, {.4, 0x3355ff, .8}, {1, 0x11aa22, .45}};
        }
        std::ostringstream serialized;
        Check(snowdesktop::WriteTaskbarGradients(serialized, saved), "serialize all taskbar gradients");
        JsonValue document;
        Check(ParseJson("{" + serialized.str() + "\"schema\":1}", document), "taskbar gradient fields form valid JSON");
        DockSettings restored;
        Check(snowdesktop::ReadTaskbarGradients(document, restored), "read taskbar gradient fields");
        auto loaded = snowdesktop::TaskbarGradients(restored);
        for (size_t i = 0; i < gradients.size(); ++i)
            Check(*loaded[i] == *gradients[i], "taskbar gradients preserve colors, opacity, direction, range and enabled state per rule");
        document.object[snowdesktop::kTaskbarGradientKeys[3]].object["angle"].number = 900;
        Check(!snowdesktop::ReadTaskbarGradients(document, restored), "reject a damaged taskbar gradient");
        for (size_t i = 0; i < gradients.size(); ++i)
            Check(*loaded[i] == *gradients[i], "invalid gradient load never partially overwrites other scenarios");
        Check(ParseJson("{}", document) && snowdesktop::ReadTaskbarGradients(document, restored), "legacy taskbar settings remain readable");
        for (const auto* value : loaded) Check(!value->enabled, "legacy taskbar settings default to no gradient");
        gradients[2]->stops[1].position = 1;
        std::ostringstream invalid;
        Check(!snowdesktop::WriteTaskbarGradients(invalid, saved) && invalid.str().empty(), "invalid taskbar gradients are rejected before writing any fields");
    }
    std::error_code error;
    const auto path = std::filesystem::temp_directory_path(error) /
        (L"SnowDesktopGeneralSettingsTests-" +
            std::to_wstring(GetCurrentProcessId()) + L".json");
    std::filesystem::remove(path, error);

    GeneralSettings saved;
    saved.animationMode = 1;
    saved.popupAnimationEffect = 1;
    saved.animationSpeed = 2;
    saved.animationFrameLimit = 30;
    saved.animationEnergySaver = false;
    saved.animationOnBattery = true;
    saved.demoModeEnabled = true;
    saved.widgetDeveloperToolsEnabled = true;
    saved.quickNavTheme = kFourThemeAcrylicDark;
    saved.collectionPopupTheme = kFourThemeAcrylicLight;
    saved.quickNavigationAppearance.mode = 4;
    saved.quickNavigationAppearance.customized = true;
    saved.quickNavigationAppearance.appearance.backgroundPreset = kAppearancePresetCustom;
    saved.quickNavigationAppearance.appearance.panelGradient.enabled = true;
    saved.quickNavigationAppearance.appearance.panelGradient.angle = 213;
    saved.collectionPopupAppearance.mode = 2;
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
    Check(loaded.quickNavigationAppearance == saved.quickNavigationAppearance &&
        loaded.collectionPopupAppearance.mode == 2,
        "independent surface custom appearance and fixed preset survive save and reload");
    {
        using namespace snowdesktop;
        SurfaceTheme theme;
        auto global = PersonalizationSettings::LightPreset();
        Check(ResolveSurfaceTheme(theme, global, 0, true).contentTheme == 1,
            "new surface theme follows the global preset by default");
        theme.mode = 0;
        Check(ResolveSurfaceTheme(theme, global, 1, true).contentTheme == 0,
            "fixed surface preset applies even when the global theme is not custom");
        theme.mode = -2;
        Check(ResolveSurfaceTheme(theme, global, 0, false).contentTheme == 1,
            "legacy surface follows a non-custom global theme");
        global.backgroundPreset = kAppearancePresetCustom;
        Check(ResolveSurfaceTheme(theme, global, 0, false).contentTheme == 0,
            "legacy surface keeps its old override for a custom global theme");
        theme.mode = -1;
        global.panelGradient.enabled = true;
        global.panelGradient.angle = 123;
        Check(IsCustomSurfaceTheme(theme, global) && ResolveSurfaceTheme(theme, global, 0, true) == global,
            "following a custom global theme exposes the actual custom fill and material");
        theme.customized = true; theme.appearance.widgetAlpha = .23f;
        Check(ResolveSurfaceTheme(theme, global, 0, false).widgetAlpha == .23f,
            "editing a following custom popup applies its independent appearance");
        auto light = PersonalizationSettings::LightPreset();
        Check(!IsCustomSurfaceTheme(theme, light) && ResolveSurfaceTheme(theme, light, 0, false).contentTheme == 1,
            "switching global back to a preset resumes inheritance without deleting custom values");
        DockSettings dock;
        dock.followComponentAppearance = false;
        for (int preset : {0, 1, 6, 7, 10, 11})
        {
            dock.appearancePreset = preset;
            auto expected = MakeAppearancePreset(preset); expected.cornerRadius = global.cornerRadius;
            Check(ResolveDockAppearance(dock, global) == expected, "Dock resolves each global appearance preset independently");
        }
        dock.appearancePreset = kAppearancePresetCustom; dock.customAppearance.widgetAlpha = .39f;
        Check(ResolveDockAppearance(dock, global).widgetAlpha == .39f, "legacy Dock custom appearance remains effective");
        dock.followComponentAppearance = true;
        Check(ResolveDockAppearance(dock, global) == global, "Dock follow mode uses the complete component appearance");
        dock.appearancePreset = 123; NormalizeDockSettings(dock);
        Check(dock.appearancePreset == kAppearancePresetCustom, "unsupported Dock preset falls back to retained custom settings");
        theme = saved.quickNavigationAppearance;
        Check(ResolveSurfaceTheme(theme, global, 0, true) == theme.appearance,
            "custom surface preserves gradient, opacity, material and foreground independently");
        JsonValue encoded;
        Check(ParseJson(EncodeSurfaceTheme(theme), encoded), "custom surface JSON is readable");
        encoded.object["appearance"].object["opacity"].number = -1;
        SurfaceTheme before = theme;
        Check(!DecodeSurfaceTheme(encoded, theme) && theme == before,
            "invalid custom opacity rejects the complete replacement without changing stored appearance");
    }
    Check(loaded.animationMode == 1 && loaded.popupAnimationEffect == 1 &&
        loaded.animationSpeed == 2 && loaded.animationFrameLimit == 30 &&
        !loaded.animationEnergySaver && loaded.animationOnBattery,
        "animation preferences survive a settings save and reload");
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
                   "  \"animationMode\": 99,\n"
                   "  \"popupAnimationEffect\": -1,\n"
                   "  \"animationSpeed\": 99,\n"
                   "  \"animationFrameLimit\": 999,\n"
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
    Check(clamped.animationMode == 0 && clamped.popupAnimationEffect == 2 &&
        clamped.animationSpeed == 1 && clamped.animationFrameLimit == 0,
        "invalid animation choices fall back without disabling the interface");

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
    Check(migrated.animationMode == 0 && migrated.popupAnimationEffect == 2 &&
        migrated.animationSpeed == 1 && migrated.animationFrameLimit == 0 &&
        migrated.animationEnergySaver && !migrated.animationOnBattery,
        "legacy animation defaults preserve current effects and automatic cadence");
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
    savedAppearance.luaWidgetContentRowHeight = 34.0f;
    savedAppearance.panelGradient.enabled = true;
    savedAppearance.panelGradient.angle = 45;
    savedAppearance.panelGradient.start = .1;
    savedAppearance.panelGradient.end = .9;
    savedAppearance.panelGradient.stops.insert(savedAppearance.panelGradient.stops.begin() + 1, {.4, 0x102030, .2});
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
            loadedAppearance.luaWidgetContentRowHeight == 34.0f &&
            !loadedAppearance.glassEnabled && loadedAppearance.panelGradient == savedAppearance.panelGradient &&
            loadedAppearance.gradientEndA == savedAppearance.gradientEndA,
        "appearance and Lua widget row height round trip independently");
    auto invalidAppearance = savedAppearance;
    invalidAppearance.panelGradient.stops[1].position = 1;
    Check(!SavePersonalization(personalizationPath.c_str(), invalidAppearance) &&
        LoadPersonalization(personalizationPath.c_str(), loadedAppearance) &&
        loadedAppearance.panelGradient == savedAppearance.panelGradient,
        "invalid gradient stops are rejected before truncating the last valid appearance file");
    snowdesktop::PanelGradient direction;
    direction.angle = 0; direction.start = .25; direction.end = .75;
    const auto horizontal = snowdesktop::ResolvePanelGradientLine(direction, 200, 100);
    direction.angle = 90;
    const auto vertical = snowdesktop::ResolvePanelGradientLine(direction, 200, 100);
    Check(std::abs(horizontal.x1 - 50) < .0001 && std::abs(horizontal.x2 - 150) < .0001 &&
        std::abs(horizontal.y1 - 50) < .0001 && std::abs(vertical.x1 - 100) < .0001 &&
        std::abs(vertical.y1 - 25) < .0001 && std::abs(vertical.y2 - 75) < .0001,
        "gradient direction and percentage range use the full panel axes after resizing");

    {
        std::ofstream legacyGlass(
            personalizationPath, std::ios::binary | std::ios::trunc);
        legacyGlass << "{\n"
                       "  \"backgroundPreset\": 9,\n"
                       "  \"glassEnabled\": true\n"
                       "}\n";
    }
    PersonalizationSettings migratedGlass;
    migratedGlass.panelGradient = savedAppearance.panelGradient;
    Check(LoadPersonalization(personalizationPath.c_str(), migratedGlass) &&
            migratedGlass.widgetEdgeHighlightEnabled &&
            migratedGlass.widgetEdgeHighlightWidth ==
                kDefaultEdgeHighlightWidth &&
            migratedGlass.widgetEdgeHighlightStrength ==
                kDefaultEdgeHighlightStrength &&
            migratedGlass.widgetBorderWidth == 1.0f &&
            migratedGlass.widgetBorderAlpha == 0.0f && !migratedGlass.panelGradient.enabled,
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
                           "  \"widgetEdgeHighlightStrength\": -1,\n"
                           "  \"luaWidgetTitleAreaHeight\": 999\n"
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
            explicitAppearance.widgetEdgeHighlightStrength == 0.0f &&
            explicitAppearance.luaWidgetContentRowHeight == 48.0f,
        "legacy Lua title-area height migrates to the clamped content row height");
    std::filesystem::remove(personalizationPath, error);

    std::filesystem::remove(path, error);
    if (failures == 0)
        std::cout << "All general settings tests passed.\n";
    return failures == 0 ? 0 : 1;
}
