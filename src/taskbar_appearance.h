#pragma once
#include "dock_settings.h"
#include "surface_theme.h"
#include <array>
#include <type_traits>

namespace snowdesktop
{
inline constexpr std::array<const char*, 4> kTaskbarMaterialKeys{
    "taskbarAppearance", "systemTaskbarVisibleWindowAppearance",
    "systemTaskbarMaximizedWindowAppearance", "systemTaskbarShellUiAppearance"};
template<class Settings> inline auto TaskbarMaterials(Settings& settings)
{
    using Appearance = std::conditional_t<std::is_const_v<Settings>, const PersonalizationSettings, PersonalizationSettings>;
    return std::array<Appearance*, 4>{&settings.systemTaskbarAppearance,
        &settings.systemTaskbarVisibleWindow.appearance, &settings.systemTaskbarMaximizedWindow.appearance,
        &settings.systemTaskbarShellUi.appearance};
}
inline bool ReadTaskbarMaterials(const JsonValue& document, DockSettings& settings)
{
    auto replacement = settings;
    const auto materials = TaskbarMaterials(replacement);
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        const auto* json = document.Find(kTaskbarMaterialKeys[i]);
        if (!json)
        {
            // Old custom taskbars only drew a one-DIP outline, without a rim.
            materials[i]->widgetBorderWidth = 1.f;
            materials[i]->widgetEdgeHighlightEnabled = false;
            continue;
        }
        const int preset = materials[i]->backgroundPreset;
        if (!DecodePanelAppearance(*json, *materials[i])) return false;
        materials[i]->backgroundPreset = preset;
    }
    settings = std::move(replacement);
    return true;
}
inline bool WriteTaskbarMaterials(std::ostream& output, const DockSettings& settings)
{
    std::ostringstream fields;
    const auto materials = TaskbarMaterials(settings);
    for (std::size_t i = 0; i < materials.size(); ++i)
    {
        const auto encoded = EncodePanelAppearance(*materials[i]);
        if (encoded.empty()) return false;
        fields << "  \"" << kTaskbarMaterialKeys[i] << "\": " << encoded << ",\n";
    }
    output << fields.str();
    return true;
}

inline PersonalizationSettings ResolveTaskbarAppearance(const DockSettings& settings,
    const PersonalizationSettings& global)
{
    auto value = settings.systemTaskbarFollowPersonalization ? global : settings.systemTaskbarAppearance;
    if (settings.systemTaskbarContentTheme >= 0) value.contentTheme = settings.systemTaskbarContentTheme;
    else if (!settings.systemTaskbarFollowPersonalization && value.backgroundPreset != kAppearancePresetCustom)
        value.contentTheme = MakeAppearancePreset(value.backgroundPreset).contentTheme;
    return value;
}

inline PersonalizationSettings ResolveTaskbarRuleAppearance(const SystemTaskbarDynamicRule& rule,
    const PersonalizationSettings& global)
{
    PersonalizationSettings value;
    switch (rule.themeMode)
    {
    case SystemTaskbarThemeMode::FollowGlobal: value = global; break;
    case SystemTaskbarThemeMode::Dark: value = MakeAppearancePreset(kAppearancePresetDark); break;
    case SystemTaskbarThemeMode::Light: value = MakeAppearancePreset(kAppearancePresetLight); break;
    case SystemTaskbarThemeMode::GlassDark: value = MakeAppearancePreset(kAppearancePresetGlassDark); break;
    case SystemTaskbarThemeMode::GlassLight: value = MakeAppearancePreset(kAppearancePresetGlassLight); break;
    case SystemTaskbarThemeMode::AcrylicDark: value = MakeAppearancePreset(kAppearancePresetAcrylicDark); break;
    case SystemTaskbarThemeMode::AcrylicLight: value = MakeAppearancePreset(kAppearancePresetAcrylicLight); break;
    case SystemTaskbarThemeMode::Transparent: value = MakeTransparentTaskbarAppearance(); break;
    case SystemTaskbarThemeMode::Custom: value = rule.appearance; break;
    case SystemTaskbarThemeMode::Native: value = PersonalizationSettings::DarkPreset(); break;
    }
    if (rule.contentTheme >= 0) value.contentTheme = rule.contentTheme;
    return value;
}

inline void SelectTaskbarCustomAppearance(DockSettings& settings, const PersonalizationSettings& global)
{
    settings.systemTaskbarAppearance = ResolveTaskbarAppearance(settings, global);
    settings.systemTaskbarAppearance.backgroundPreset = kAppearancePresetCustom;
    settings.systemTaskbarContentTheme = settings.systemTaskbarAppearance.contentTheme;
    settings.systemTaskbarBackdropEnabled = true;
    settings.systemTaskbarFollowPersonalization = false;
}

inline void SelectTaskbarRuleTheme(SystemTaskbarDynamicRule& rule, SystemTaskbarThemeMode mode,
    const PersonalizationSettings& global)
{
    if (mode == SystemTaskbarThemeMode::Custom && rule.themeMode != mode)
    {
        rule.appearance = ResolveTaskbarRuleAppearance(rule, global);
        rule.appearance.backgroundPreset = kAppearancePresetCustom;
        rule.contentTheme = rule.appearance.contentTheme;
    }
    rule.themeMode = mode;
}
}
