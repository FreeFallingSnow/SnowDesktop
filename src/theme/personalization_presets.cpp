#include "personalization.h"
#include "appearance_edge_presets.h"
#include <algorithm>
#include <cmath>

float NormalizePopupHoverDelayMs(double delay)
{
    if (!std::isfinite(delay)) return kDefaultPopupHoverDelayMs;
    return static_cast<float>(std::round(std::clamp(delay,
        static_cast<double>(kMinimumPopupHoverDelayMs),
        static_cast<double>(kMaximumPopupHoverDelayMs))));
}

float NormalizeTabHoverDelayMs(double delay)
{
    if (!std::isfinite(delay)) return kDefaultTabHoverDelayMs;
    return static_cast<float>(std::round(std::clamp(delay,
        static_cast<double>(kMinimumTabHoverDelayMs),
        static_cast<double>(kMaximumTabHoverDelayMs))));
}

void NormalizeWindowPinBorderAppearance(PersonalizationSettings& settings)
{
    const PersonalizationSettings defaults;
    const auto normalize = [&](float PersonalizationSettings::* member, float minimum, float maximum) {
        settings.*member = std::isfinite(settings.*member)
            ? std::clamp(settings.*member, minimum, maximum) : defaults.*member;
    };
    normalize(&PersonalizationSettings::windowPinBorderR, 0.0f, 1.0f);
    normalize(&PersonalizationSettings::windowPinBorderG, 0.0f, 1.0f);
    normalize(&PersonalizationSettings::windowPinBorderB, 0.0f, 1.0f);
    normalize(&PersonalizationSettings::windowPinBorderOpacity, 0.0f, 1.0f);
    normalize(&PersonalizationSettings::windowPinBorderWidth,
        kMinimumWindowPinBorderWidth, kMaximumWindowPinBorderWidth);
}

/**
 * @brief 获取深色主题预设
 *
 * 返回一组适用于深色背景的组件颜色参数，包括半透明深色背景和白色边框。
 *
 * @return PersonalizationSettings 深色主题配置
 */
PersonalizationSettings PersonalizationSettings::DarkPreset()
{
    PersonalizationSettings s;
    s.widgetBgR = 0.08f; s.widgetBgG = 0.10f; s.widgetBgB = 0.13f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.40f; s.widgetBorderAlpha = 0.32f;
    s.gradientEndA = 0.56f;
    s.backgroundPreset = 0;
    return s;
}

/**
 * @brief 获取浅色主题预设
 *
 * 返回一组适用于浅色背景的组件颜色参数，包括半透明浅色背景和灰色边框。
 *
 * @return PersonalizationSettings 浅色主题配置
 */
PersonalizationSettings PersonalizationSettings::LightPreset()
{
    PersonalizationSettings s;
    s.widgetBgR = 0.95f; s.widgetBgG = 0.96f; s.widgetBgB = 0.97f;
    s.widgetBorderR = 0.5f; s.widgetBorderG = 0.5f; s.widgetBorderB = 0.55f;
    s.widgetAlpha = 0.75f; s.widgetBorderAlpha = 0.20f;
    s.gradientEndA = 0.12f;
    s.backgroundPreset = 1;
    s.contentTheme = 1;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassDarkPreset()
{
    PersonalizationSettings s = DarkPreset();
    s.widgetBgR = 13.f / 255.f; s.widgetBgG = 18.f / 255.f; s.widgetBgB = 26.f / 255.f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.28f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetGlassDark;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    const auto edge = snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::GlassDark);
    s.widgetEdgeHighlightWidth = edge.width;
    s.widgetEdgeHighlightStrength = edge.opacity;
    s.edgeLight = edge.light;
    s.glassBlurRadius = 24.0f;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassLightPreset()
{
    PersonalizationSettings s = LightPreset();
    s.widgetBgR = 235.f / 255.f; s.widgetBgG = 245.f / 255.f; s.widgetBgB = 1.f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.15f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetGlassLight;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    const auto edge = snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::GlassLight);
    s.widgetEdgeHighlightWidth = edge.width;
    s.widgetEdgeHighlightStrength = edge.opacity;
    s.edgeLight = edge.light;
    s.glassBlurRadius = 22.0f;
    s.contentTheme = 0;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassTransparentPreset()
{
    PersonalizationSettings s;
    s.backgroundPreset = kAppearancePresetGlassTransparent;
    s.widgetBgR = s.widgetBgG = s.widgetBgB = 1.0f;
    s.widgetAlpha = 0.02f;
    s.widgetBorderAlpha = 0.05f;
    s.widgetBorderWidth = 0.75f;
    s.widgetEdgeHighlightEnabled = true;
    const auto edge = snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::GlassTransparent);
    s.widgetEdgeHighlightWidth = edge.width;
    s.widgetEdgeHighlightStrength = edge.opacity;
    s.edgeLight = edge.light;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.glassBlurRadius = 10.0f;
    s.contentTheme = 0;
    return s;
}

PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset()
{
    PersonalizationSettings s = DarkPreset();
    // Match the neutral #202020 tint used by Windows dark shell panels.
    s.widgetBgR = s.widgetBgG = s.widgetBgB = 32.f / 255.f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.80f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetAcrylicDark;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.acrylicEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    const auto edge = snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::AcrylicDark);
    s.widgetEdgeHighlightWidth = edge.width;
    s.widgetEdgeHighlightStrength = edge.opacity;
    s.edgeLight = edge.light;
    s.glassBlurRadius = 30.0f;
    s.contentTheme = 0;
    return s;
}

PersonalizationSettings PersonalizationSettings::AcrylicLightPreset()
{
    PersonalizationSettings s = LightPreset();
    // Match the neutral #F3F3F3 tint used by Windows light shell panels.
    s.widgetBgR = s.widgetBgG = s.widgetBgB = 243.f / 255.f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.80f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetAcrylicLight;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.acrylicEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    const auto edge = snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::AcrylicLight);
    s.widgetEdgeHighlightWidth = edge.width;
    s.widgetEdgeHighlightStrength = edge.opacity;
    s.edgeLight = edge.light;
    s.glassBlurRadius = 30.0f;
    s.contentTheme = 1;
    return s;
}

int NormalizeAppearancePresetId(int presetId)
{
    switch (presetId)
    {
    case 0:
    case 1:
    case 6:
    case 7:
    case 9:
    case 10:
    case 11:
    case 12:
    case 13:
        return presetId;
    case 3:
    case 4:
        return 1;
    case 2:
    case 5:
        return 0;
    case 8:
        return 6;
    default:
        return 0;
    }
}

PersonalizationSettings MakeAppearancePreset(int presetId)
{
    switch (NormalizeAppearancePresetId(presetId))
    {
    case 1: return PersonalizationSettings::LightPreset();
    case 6: return PersonalizationSettings::GlassDarkPreset();
    case 7: return PersonalizationSettings::GlassLightPreset();
    case 13: return PersonalizationSettings::GlassTransparentPreset();
    case 10: return PersonalizationSettings::AcrylicDarkPreset();
    case 11: return PersonalizationSettings::AcrylicLightPreset();
    case 9:
    {
        PersonalizationSettings custom = PersonalizationSettings::DarkPreset();
        custom.backgroundPreset = kAppearancePresetCustom;
        return custom;
    }
    default: return PersonalizationSettings::DarkPreset();
    }
}

void ApplyAppearancePreset(PersonalizationSettings& settings, int presetId)
{
    auto preset = MakeAppearancePreset(presetId);
    preset.cornerRadius = settings.cornerRadius;
    preset.barHeight = settings.barHeight;
    preset.scrollableTitleBarOnTop = settings.scrollableTitleBarOnTop;
    preset.widgetTransformCursors = settings.widgetTransformCursors;
    preset.categorizedTabHeight = settings.categorizedTabHeight;
    preset.luaWidgetContentRowHeight = settings.luaWidgetContentRowHeight;
    preset.showCategoryTabCounts = settings.showCategoryTabCounts;
    preset.showGroupTabCounts = settings.showGroupTabCounts;
    preset.groupTabHoverSwitch = settings.groupTabHoverSwitch;
    preset.categoryTabHoverSwitch = settings.categoryTabHoverSwitch;
    preset.tabHoverDelayMs = settings.tabHoverDelayMs;
    preset.popupHoverOpen = settings.popupHoverOpen;
    preset.popupHoverDelayMs = settings.popupHoverDelayMs;
    preset.popupCloseOnMouseLeave = settings.popupCloseOnMouseLeave;
    preset.contextMenuStyle = settings.contextMenuStyle;
    preset.windowPinBorderEnabled = settings.windowPinBorderEnabled;
    preset.windowPinBorderR = settings.windowPinBorderR;
    preset.windowPinBorderG = settings.windowPinBorderG;
    preset.windowPinBorderB = settings.windowPinBorderB;
    preset.windowPinBorderWidth = settings.windowPinBorderWidth;
    preset.windowPinBorderOpacity = settings.windowPinBorderOpacity;
    settings = std::move(preset);
}

PersonalizationSettings MakeQuickNavigationAppearancePreset(int presetId)
{
    PersonalizationSettings s;
    int normalizedId = NormalizeAppearancePresetId(presetId);
    if (normalizedId == kAppearancePresetGlassDark ||
        normalizedId == kAppearancePresetGlassTransparent)
        normalizedId = kAppearancePresetAcrylicDark;
    else if (normalizedId == kAppearancePresetGlassLight)
        normalizedId = kAppearancePresetAcrylicLight;
    switch (normalizedId)
    {
    case kAppearancePresetLight:
        s = PersonalizationSettings::LightPreset();
        s.widgetBgR = s.widgetBgG = s.widgetBgB = 0.970f;
        s.widgetBorderR = s.widgetBorderG = s.widgetBorderB = 0.710f;
        s.widgetAlpha = 1.0f; s.widgetBorderAlpha = 0.34f;
        s.glassEnabled = false;
        break;
    case kAppearancePresetAcrylicDark:
        s = PersonalizationSettings::AcrylicDarkPreset();
        s.widgetBgR = s.widgetBgG = s.widgetBgB = 44.0f / 255.0f;
        s.widgetBorderR = s.widgetBorderG = s.widgetBorderB = 0.580f;
        s.widgetAlpha = 0.83f; s.widgetBorderAlpha = 0.32f;
        s.glassBlurRadius = 30.0f;
        break;
    case kAppearancePresetAcrylicLight:
        s = PersonalizationSettings::AcrylicLightPreset();
        s.widgetBgR = s.widgetBgG = s.widgetBgB = 0.970f;
        s.widgetBorderR = s.widgetBorderG = s.widgetBorderB = 0.710f;
        s.widgetAlpha = 0.88f; s.widgetBorderAlpha = 0.34f;
        s.glassBlurRadius = 28.0f;
        break;
    case kAppearancePresetCustom:
        s = PersonalizationSettings::DarkPreset();
        s.backgroundPreset = kAppearancePresetCustom;
        break;
    default:
        s = PersonalizationSettings::DarkPreset();
        s.widgetBgR = 0.105f; s.widgetBgG = 0.108f; s.widgetBgB = 0.116f;
        s.widgetBorderR = 0.471f; s.widgetBorderG = 0.510f; s.widgetBorderB = 0.588f;
        s.widgetAlpha = 1.0f; s.widgetBorderAlpha = 0.30f;
        s.glassEnabled = false;
        break;
    }
    s.widgetEdgeHighlightStrength = 0.25f;
    return s;
}

PersonalizationSettings MakeCollectionPopupAppearancePreset(int presetId)
{
    PersonalizationSettings s =
        MakeQuickNavigationAppearancePreset(presetId);
    // Preserve collection materials independently of navigation styling.
    s.widgetEdgeHighlightStrength = MakeAppearancePreset(s.backgroundPreset).widgetEdgeHighlightStrength;
    switch (s.backgroundPreset)
    {
    case kAppearancePresetLight:
        s.widgetBgR = .965f; s.widgetBgG = .973f; s.widgetBgB = .988f;
        s.widgetBorderR = .706f; s.widgetBorderG = .745f; s.widgetBorderB = .784f;
        s.widgetAlpha = .96f; s.widgetBorderAlpha = .70f;
        break;
    case kAppearancePresetAcrylicDark:
        s.widgetBgR = .065f; s.widgetBgG = .080f; s.widgetBgB = .110f;
        s.widgetBorderR = .58f; s.widgetBorderG = .66f; s.widgetBorderB = .78f;
        s.widgetAlpha = .75f; s.widgetBorderAlpha = .72f;
        break;
    case kAppearancePresetAcrylicLight:
        s.widgetBgR = .935f; s.widgetBgG = .955f; s.widgetBgB = .985f;
        s.widgetBorderR = .72f; s.widgetBorderG = .77f; s.widgetBorderB = .86f;
        s.widgetAlpha = .75f; s.widgetBorderAlpha = .78f;
        break;
    case kAppearancePresetDark:
        s.widgetBgR = .055f; s.widgetBgG = .071f; s.widgetBgB = .102f;
        s.widgetBorderR = .471f; s.widgetBorderG = .510f; s.widgetBorderB = .588f;
        s.widgetAlpha = .96f; s.widgetBorderAlpha = .62f;
        break;
    default: break;
    }
    if (s.widgetEdgeHighlightEnabled)
    {
        // Collection popups participate in the independent edge-light
        // treatment. Keep the readability-tuned panel tint, but do not stack
        // the Quick Navigation outline underneath the additive bevel.
        s.widgetBorderAlpha = 0.0f;
    }
    return s;
}
