/**
 * @file personalization.cpp
 * @brief 个性化设置持久化实现
 *
 * 提供个性化配置（深色/浅色预设）的加载、保存及路径管理功能。
 * 配置以 JSON 格式存储于 data 目录下的 SnowDesktop.personalization.json 文件中。
 */

#include "personalization.h"
#include "data_paths.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

/**
 * @brief 从 JSON 文本中读取指定字段的 double 值
 *
 * 在文本中搜索 "fieldName" 标记，定位到其后的冒号并解析数值。
 *
 * @param text   JSON 格式的字符串
 * @param field  要读取的字段名（不含引号）
 * @param out    输出参数，解析成功时写入对应的 double 值
 * @return true  字段找到且数值解析成功
 * @return false 字段不存在或解析失败
 */
static bool ReadDoubleField(const std::string& text, const char* field, double& out)
{
    std::string marker = "\"" + std::string(field) + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p);
    if (p == std::string::npos) return false;
    p = text.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return false;
    out = atof(text.c_str() + p);
    return true;
}

/**
 * @brief 从 JSON 文本中读取指定字段的 bool 值
 * @param text   JSON 格式的字符串
 * @param field  要读取的字段名（不含引号）
 * @param out    输出参数，解析成功时写入对应的 bool 值
 * @return true  字段找到且解析成功
 * @return false 字段不存在或解析失败
 */
static bool ReadBoolField(const std::string& text, const char* field, bool& out)
{
    std::string marker = "\"" + std::string(field) + "\"";
    size_t p = text.find(marker);
    if (p == std::string::npos) return false;
    p = text.find(':', p);
    if (p == std::string::npos) return false;
    p = text.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return false;
    if (text.compare(p, 4, "true") == 0) { out = true; return true; }
    if (text.compare(p, 5, "false") == 0) { out = false; return true; }
    return false;
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
    s.widgetBgR = 0.05f; s.widgetBgG = 0.07f; s.widgetBgB = 0.10f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.28f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetGlassDark;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    s.widgetEdgeHighlightWidth = kDefaultEdgeHighlightWidth;
    s.widgetEdgeHighlightStrength = kDefaultEdgeHighlightStrength;
    s.glassBlurRadius = 24.0f;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassLightPreset()
{
    PersonalizationSettings s = LightPreset();
    s.widgetBgR = 0.92f; s.widgetBgG = 0.96f; s.widgetBgB = 1.0f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.15f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetGlassLight;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    s.widgetEdgeHighlightWidth = kDefaultEdgeHighlightWidth;
    s.widgetEdgeHighlightStrength = kDefaultEdgeHighlightStrength;
    s.glassBlurRadius = 22.0f;
    s.contentTheme = 0;
    return s;
}

PersonalizationSettings PersonalizationSettings::AcrylicDarkPreset()
{
    PersonalizationSettings s = DarkPreset();
    // Match the neutral #202020 tint used by Windows dark shell panels.
    s.widgetBgR = 0.125f; s.widgetBgG = 0.125f; s.widgetBgB = 0.125f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.80f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetAcrylicDark;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.acrylicEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    s.widgetEdgeHighlightWidth = kDefaultEdgeHighlightWidth;
    s.widgetEdgeHighlightStrength = kDefaultEdgeHighlightStrength;
    s.glassBlurRadius = 30.0f;
    s.contentTheme = 0;
    return s;
}

PersonalizationSettings PersonalizationSettings::AcrylicLightPreset()
{
    PersonalizationSettings s = LightPreset();
    // Match the neutral #F3F3F3 tint used by Windows light shell panels.
    s.widgetBgR = 0.953f; s.widgetBgG = 0.953f; s.widgetBgB = 0.953f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.80f; s.widgetBorderAlpha = 0.0f;
    s.backgroundPreset = kAppearancePresetAcrylicLight;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.acrylicEnabled = true;
    s.widgetBorderWidth = 1.0f;
    s.widgetEdgeHighlightEnabled = true;
    s.widgetEdgeHighlightWidth = kDefaultEdgeHighlightWidth;
    s.widgetEdgeHighlightStrength = kDefaultEdgeHighlightStrength;
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

PersonalizationSettings MakeQuickNavigationAppearancePreset(int presetId)
{
    PersonalizationSettings s;
    int normalizedId = NormalizeAppearancePresetId(presetId);
    if (normalizedId == kAppearancePresetGlassDark)
        normalizedId = kAppearancePresetAcrylicDark;
    else if (normalizedId == kAppearancePresetGlassLight)
        normalizedId = kAppearancePresetAcrylicLight;
    switch (normalizedId)
    {
    case kAppearancePresetLight:
        s = PersonalizationSettings::LightPreset();
        s.widgetBgR = 0.965f; s.widgetBgG = 0.973f; s.widgetBgB = 0.988f;
        s.widgetBorderR = 0.706f; s.widgetBorderG = 0.745f; s.widgetBorderB = 0.784f;
        s.widgetAlpha = 0.96f; s.widgetBorderAlpha = 0.70f;
        s.glassEnabled = false;
        break;
    case kAppearancePresetAcrylicDark:
        s = PersonalizationSettings::AcrylicDarkPreset();
        s.widgetBgR = 0.065f; s.widgetBgG = 0.080f; s.widgetBgB = 0.110f;
        s.widgetBorderR = 0.58f; s.widgetBorderG = 0.66f; s.widgetBorderB = 0.78f;
        s.widgetAlpha = 0.75f; s.widgetBorderAlpha = 0.72f;
        s.glassBlurRadius = 30.0f;
        break;
case kAppearancePresetAcrylicLight:
        s = PersonalizationSettings::AcrylicLightPreset();
        s.widgetBgR = 0.935f; s.widgetBgG = 0.955f; s.widgetBgB = 0.985f;
        s.widgetBorderR = 0.72f; s.widgetBorderG = 0.77f; s.widgetBorderB = 0.86f;
        s.widgetAlpha = 0.75f; s.widgetBorderAlpha = 0.78f;
        s.glassBlurRadius = 28.0f;
        break;
    case kAppearancePresetCustom:
        s = PersonalizationSettings::DarkPreset();
        s.backgroundPreset = kAppearancePresetCustom;
        break;
    default:
        s = PersonalizationSettings::DarkPreset();
        s.widgetBgR = 0.055f; s.widgetBgG = 0.071f; s.widgetBgB = 0.102f;
        s.widgetBorderR = 0.471f; s.widgetBorderG = 0.510f; s.widgetBorderB = 0.588f;
        s.widgetAlpha = 0.96f; s.widgetBorderAlpha = 0.62f;
        s.glassEnabled = false;
        break;
    }
    return s;
}

PersonalizationSettings MakeCollectionPopupAppearancePreset(int presetId)
{
    PersonalizationSettings s =
        MakeQuickNavigationAppearancePreset(presetId);
    if (s.widgetEdgeHighlightEnabled)
    {
        // Collection popups participate in the independent edge-light
        // treatment. Keep the readability-tuned panel tint, but do not stack
        // the Quick Navigation outline underneath the additive bevel.
        s.widgetBorderAlpha = 0.0f;
    }
    return s;
}

/**
 * @brief 获取个性化配置文件的完整路径
 *
 * 构造 data 目录下的 SnowDesktop.personalization.json 路径。
 *
 * @return std::wstring 配置文件的绝对路径
 */
std::wstring GetPersonalizationPath()
{
    return GetDataFilePath(L"SnowDesktop.personalization.json");
}

/**
 * @brief 从 JSON 文件加载个性化设置
 *
 * 读取指定路径的 JSON 文件并反序列化各字段到 PersonalizationSettings 结构体。
 * 文件中不存在的字段将保持结构体中的原值不变。
 *
 * @param path JSON 配置文件路径
 * @param s    输出参数，从文件中读取到的设置值
 * @return true  加载成功（文件存在且非空）
 * @return false 文件打开失败或内容为空
 */
bool LoadPersonalization(
    const wchar_t* path,
    PersonalizationSettings& s,
    bool* categorizedTabHeightLoaded)
{
    if (categorizedTabHeightLoaded)
        *categorizedTabHeightLoaded = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    double v = 0;
    if (ReadDoubleField(text, "widgetBgR", v)) s.widgetBgR = (float)v;
    if (ReadDoubleField(text, "widgetBgG", v)) s.widgetBgG = (float)v;
    if (ReadDoubleField(text, "widgetBgB", v)) s.widgetBgB = (float)v;
    if (ReadDoubleField(text, "widgetBorderR", v)) s.widgetBorderR = (float)v;
    if (ReadDoubleField(text, "widgetBorderG", v)) s.widgetBorderG = (float)v;
    if (ReadDoubleField(text, "widgetBorderB", v)) s.widgetBorderB = (float)v;
    if (ReadDoubleField(text, "widgetAlpha", v)) s.widgetAlpha = (float)v;
    if (ReadDoubleField(text, "widgetBorderAlpha", v)) s.widgetBorderAlpha = (float)v;
    bool edgeHighlightEnabled = false;
    const bool edgeHighlightEnabledLoaded = ReadBoolField(
        text, "widgetEdgeHighlightEnabled", edgeHighlightEnabled);
    if (edgeHighlightEnabledLoaded)
        s.widgetEdgeHighlightEnabled = edgeHighlightEnabled;
    int legacyBorderStyle = 0;
    const bool legacyBorderStyleLoaded =
        ReadDoubleField(text, "widgetBorderStyle", v) && std::isfinite(v);
    if (legacyBorderStyleLoaded)
        legacyBorderStyle = static_cast<int>(v);
    bool borderWidthLoaded = false;
    if (ReadDoubleField(text, "widgetBorderWidth", v) && std::isfinite(v))
    {
        s.widgetBorderWidth = std::clamp(static_cast<float>(v),
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
        borderWidthLoaded = true;
    }
    bool edgeHighlightWidthLoaded = false;
    if (ReadDoubleField(text, "widgetEdgeHighlightWidth", v) &&
        std::isfinite(v))
    {
        s.widgetEdgeHighlightWidth = std::clamp(static_cast<float>(v),
            kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
        edgeHighlightWidthLoaded = true;
    }
    bool edgeHighlightStrengthLoaded = false;
    if (ReadDoubleField(text, "widgetEdgeHighlightStrength", v) &&
        std::isfinite(v))
    {
        s.widgetEdgeHighlightStrength = std::clamp(
            static_cast<float>(v), 0.0f, 1.0f);
        edgeHighlightStrengthLoaded = true;
    }
    else if (ReadDoubleField(text, "widgetBorderEffectStrength", v) &&
        std::isfinite(v))
    {
        s.widgetEdgeHighlightStrength = std::clamp(
            static_cast<float>(v), 0.0f, 1.0f);
        edgeHighlightStrengthLoaded = true;
    }
    if (ReadDoubleField(text, "gradientEndA", v)) s.gradientEndA = (float)v;
    if (ReadDoubleField(text, "barHeight", v)) s.barHeight = (float)v;
    if (ReadDoubleField(text, "categorizedTabHeight", v))
    {
        s.categorizedTabHeight =
            std::clamp(static_cast<float>(v), 24.0f, 48.0f);
        if (categorizedTabHeightLoaded)
            *categorizedTabHeightLoaded = true;
    }
    else if (ReadDoubleField(text, "categorizedTabFontSize", v))
    {
        // 旧版按字号保存（10–22 cu）。换算为标签条高度：
        // 默认字号 15 对应默认高度 34。
        s.categorizedTabHeight = std::clamp(
            static_cast<float>(v) * 34.0f / 15.0f,
            24.0f, 48.0f);
        if (categorizedTabHeightLoaded)
            *categorizedTabHeightLoaded = true;
    }
    if (ReadDoubleField(text, "backgroundPreset", v))
    {
        s.backgroundPreset = NormalizeAppearancePresetId((int)v);
    }
    if (ReadDoubleField(text, "cornerRadius", v)) s.cornerRadius = (float)v;
    if (ReadDoubleField(text, "contextMenuStyle", v))
        s.contextMenuStyle = std::clamp(static_cast<int>(v), 0, 4);
    bool b = false;
    if (ReadBoolField(text, "glassEnabled", b)) s.glassEnabled = b;
    if (ReadDoubleField(text, "glassBlurRadius", v)) s.glassBlurRadius = (float)v;
    if (ReadDoubleField(text, "contentTheme", v)) s.contentTheme = std::clamp(static_cast<int>(v), 0, 1);
    bool b2 = false;
    if (ReadBoolField(text, "acrylicEnabled", b2)) s.acrylicEnabled = b2;
    bool b3 = false;
    if (ReadBoolField(text, "showCategoryTabCounts", b3))
        s.showCategoryTabCounts = b3;
    // Legacy releases tied edge reflection to glass and used border alpha as
    // its intensity. New edge-highlight fields take priority when present.
    if (!edgeHighlightEnabledLoaded)
        s.widgetEdgeHighlightEnabled = legacyBorderStyleLoaded
            ? legacyBorderStyle == 1 : s.glassEnabled;
    if (!borderWidthLoaded)
        s.widgetBorderWidth = 1.0f;
    if (!edgeHighlightWidthLoaded)
        s.widgetEdgeHighlightWidth = legacyBorderStyleLoaded &&
            legacyBorderStyle == 1 && borderWidthLoaded
            ? s.widgetBorderWidth : kDefaultEdgeHighlightWidth;
    if (!edgeHighlightStrengthLoaded)
        s.widgetEdgeHighlightStrength =
            kDefaultEdgeHighlightStrength;
    if (!edgeHighlightEnabledLoaded && s.glassEnabled)
        s.widgetBorderAlpha = 0.0f;
    // Presets are immutable choices in the UI. Refresh persisted acrylic
    // values so palette refinements and the old placeholder migration are
    // applied without requiring users to reselect the theme.
    if (s.backgroundPreset == kAppearancePresetAcrylicDark ||
        s.backgroundPreset == kAppearancePresetAcrylicLight)
    {
        const float explicitBorderWidth = s.widgetBorderWidth;
        const bool explicitEdgeHighlightEnabled =
            s.widgetEdgeHighlightEnabled;
        const float explicitEdgeHighlightWidth =
            s.widgetEdgeHighlightWidth;
        const float explicitEdgeHighlightStrength =
            s.widgetEdgeHighlightStrength;
        const float cornerRadius = s.cornerRadius;
        const float barHeight = s.barHeight;
        const float categorizedTabHeight =
            s.categorizedTabHeight;
        const bool showCategoryTabCounts =
            s.showCategoryTabCounts;
        const int contextMenuStyle = s.contextMenuStyle;
        s = MakeAppearancePreset(s.backgroundPreset);
        s.cornerRadius = cornerRadius;
        s.barHeight = barHeight;
        s.categorizedTabHeight =
            categorizedTabHeight;
        s.showCategoryTabCounts =
            showCategoryTabCounts;
        s.contextMenuStyle = contextMenuStyle;
        if (borderWidthLoaded)
            s.widgetBorderWidth = explicitBorderWidth;
        if (edgeHighlightEnabledLoaded)
            s.widgetEdgeHighlightEnabled = explicitEdgeHighlightEnabled;
        if (edgeHighlightWidthLoaded)
            s.widgetEdgeHighlightWidth = explicitEdgeHighlightWidth;
        if (edgeHighlightStrengthLoaded)
            s.widgetEdgeHighlightStrength = explicitEdgeHighlightStrength;
    }
    return true;
}

/**
 * @brief 将个性化设置保存为 JSON 文件
 *
 * 将 PersonalizationSettings 结构体中的各字段序列化并写入指定路径的 JSON 文件。
 * 文件以覆盖方式写入（trunc）。
 *
 * @param path 输出 JSON 文件路径
 * @param s    待保存的个性化设置
 * @return true  保存成功
 * @return false 文件打开失败
 */
bool SavePersonalization(const wchar_t* path, const PersonalizationSettings& s)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file << "{\n";
    file << "  \"widgetBgR\": " << s.widgetBgR << ",\n";
    file << "  \"widgetBgG\": " << s.widgetBgG << ",\n";
    file << "  \"widgetBgB\": " << s.widgetBgB << ",\n";
    file << "  \"widgetBorderR\": " << s.widgetBorderR << ",\n";
    file << "  \"widgetBorderG\": " << s.widgetBorderG << ",\n";
    file << "  \"widgetBorderB\": " << s.widgetBorderB << ",\n";
    file << "  \"widgetAlpha\": " << s.widgetAlpha << ",\n";
    file << "  \"widgetBorderAlpha\": " << s.widgetBorderAlpha << ",\n";
    file << "  \"widgetBorderWidth\": "
         << (std::isfinite(s.widgetBorderWidth)
                ? std::clamp(s.widgetBorderWidth,
                    kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth)
                : 1.0f)
         << ",\n";
    file << "  \"widgetEdgeHighlightEnabled\": "
         << (s.widgetEdgeHighlightEnabled ? "true" : "false") << ",\n";
    file << "  \"widgetEdgeHighlightWidth\": "
         << (std::isfinite(s.widgetEdgeHighlightWidth)
                ? std::clamp(s.widgetEdgeHighlightWidth,
                    kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth)
                : kDefaultEdgeHighlightWidth)
         << ",\n";
    file << "  \"widgetEdgeHighlightStrength\": "
         << (std::isfinite(s.widgetEdgeHighlightStrength)
                ? std::clamp(s.widgetEdgeHighlightStrength, 0.0f, 1.0f)
                : kDefaultEdgeHighlightStrength)
         << ",\n";
    file << "  \"gradientEndA\": " << s.gradientEndA << ",\n";
    file << "  \"barHeight\": " << s.barHeight << ",\n";
    file << "  \"categorizedTabHeight\": "
         << std::clamp(
                s.categorizedTabHeight,
                24.0f, 48.0f)
         << ",\n";
    file << "  \"showCategoryTabCounts\": "
         << (s.showCategoryTabCounts ? "true" : "false") << ",\n";
    file << "  \"backgroundPreset\": " << s.backgroundPreset << ",\n";
    file << "  \"cornerRadius\": " << s.cornerRadius << ",\n";
    file << "  \"contextMenuStyle\": "
         << std::clamp(s.contextMenuStyle, 0, 4) << ",\n";
    file << "  \"glassEnabled\": " << (s.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"glassBlurRadius\": " << s.glassBlurRadius << ",\n";
    file << "  \"contentTheme\": " << s.contentTheme << ",\n";
    file << "  \"acrylicEnabled\": " << (s.acrylicEnabled ? "true" : "false") << "\n";
    file << "}\n";
    return true;
}
