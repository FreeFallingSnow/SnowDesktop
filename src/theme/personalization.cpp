#include "data/atomic_file.h"
#include "common/json_value.h"
#include <sstream>
/**
 * @file personalization.cpp
 * @brief 个性化设置持久化实现
 *
 * 提供个性化配置（深色/浅色预设）的加载、保存及路径管理功能。
 * 配置以 JSON 格式存储于 data 目录下的 SnowDesktop.personalization.json 文件中。
 */

#include "personalization.h"
#include "appearance_edge_presets.h"
#include "edge_light_codec.h"
#include "data/data_paths.h"
#include "ui/menu/modern_menu_appearance_rules.h"

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

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
 * @brief 获取个性化配置文件的完整路径
 *
 * 构造 data 目录下的 SnowDesktop.personalization.json 路径。
 *
 * @return std::wstring 配置文件的绝对路径
 */

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
    s.panelGradient = {};
    JsonValue document;
    const bool documentParsed = ParseJson(text, document);
    s.edgeLight = {};
    if (documentParsed)
        if (const auto* light = document.Find("edgeLight"))
            if (!snowdesktop::DecodeEdgeLight(*light, s.edgeLight)) return false;
    if (documentParsed)
        if (const auto* gradient = document.Find("panelGradient"))
            if (!snowdesktop::DecodePanelGradient(*gradient, s.panelGradient)) return false;
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
    if (ReadDoubleField(text, "luaWidgetContentRowHeight", v) &&
        std::isfinite(v))
    {
        s.luaWidgetContentRowHeight = std::clamp(
            static_cast<float>(v), 18.0f, 48.0f);
    }
    else if ((ReadDoubleField(text, "luaWidgetRowHeight", v) ||
              ReadDoubleField(text, "luaWidgetTitleAreaHeight", v)) &&
             std::isfinite(v))
    {
        s.luaWidgetContentRowHeight = std::clamp(
            static_cast<float>(v) * 0.70f, 18.0f, 48.0f);
    }
    const bool backgroundPresetLoaded = ReadDoubleField(text, "backgroundPreset", v);
    if (backgroundPresetLoaded)
    {
        s.backgroundPreset = NormalizeAppearancePresetId((int)v);
    }
    if (ReadDoubleField(text, "cornerRadius", v)) s.cornerRadius = (float)v;
    const PersonalizationSettings pinDefaults;
    s.windowPinBorderEnabled = pinDefaults.windowPinBorderEnabled;
    ReadBoolField(text, "windowPinBorderEnabled", s.windowPinBorderEnabled);
    for (const auto& [key, member] : {
            std::pair{"windowPinBorderR", &PersonalizationSettings::windowPinBorderR},
            std::pair{"windowPinBorderG", &PersonalizationSettings::windowPinBorderG},
            std::pair{"windowPinBorderB", &PersonalizationSettings::windowPinBorderB},
            std::pair{"windowPinBorderWidth", &PersonalizationSettings::windowPinBorderWidth},
            std::pair{"windowPinBorderOpacity", &PersonalizationSettings::windowPinBorderOpacity}})
    {
        s.*member = pinDefaults.*member;
        if (ReadDoubleField(text, key, v) && std::isfinite(v)) s.*member = static_cast<float>(v);
    }
    NormalizeWindowPinBorderAppearance(s);
    // Unversioned follow used blur on Win11 and opaque compatibility on Win10.
    // The marker distinguishes that legacy value from a new explicit opaque
    // follow selection, so later reloads never migrate the user's choice again.
    s.contextMenuStyle = static_cast<int>(snowdesktop::modern_menu::Appearance::OpaqueFollowSystem);
    if (ReadDoubleField(text, "contextMenuStyle", v) && std::isfinite(v))
        s.contextMenuStyle = static_cast<int>(std::clamp(v, 0.0,
            static_cast<double>(snowdesktop::modern_menu::kMaximumAppearance)));
    const auto* menuStyleVersion = documentParsed ? document.Find("contextMenuStyleVersion") : nullptr;
    const bool versionedMenuStyle = menuStyleVersion && menuStyleVersion->IsNumber() && menuStyleVersion->number >= 1;
    if (!versionedMenuStyle && s.contextMenuStyle == 0)
        s.contextMenuStyle = static_cast<int>(
            snowdesktop::modern_menu::appearance_rules::LegacyFollowForCurrentWindows());
    s.scrollableTitleBarOnTop = false;
    ReadBoolField(text, "scrollableTitleBarOnTop", s.scrollableTitleBarOnTop);
    s.widgetTransformCursors = true;
    ReadBoolField(text, "widgetTransformCursors", s.widgetTransformCursors);
    s.groupTabHoverSwitch = false;
    ReadBoolField(text, "groupTabHoverSwitch", s.groupTabHoverSwitch);
    s.categoryTabHoverSwitch = false;
    ReadBoolField(text, "categoryTabHoverSwitch", s.categoryTabHoverSwitch);
    s.tabHoverDelayMs = kDefaultTabHoverDelayMs;
    if (documentParsed)
        if (const auto* delay = document.Find("tabHoverDelayMs"); delay && delay->IsNumber())
            s.tabHoverDelayMs = NormalizeTabHoverDelayMs(delay->number);
    s.popupHoverOpen = false;
    ReadBoolField(text, "popupHoverOpen", s.popupHoverOpen);
    s.popupCloseOnMouseLeave = false;
    ReadBoolField(text, "popupCloseOnMouseLeave", s.popupCloseOnMouseLeave);
    s.popupHoverDelayMs = kDefaultPopupHoverDelayMs;
    if (documentParsed)
        if (const auto* delay = document.Find("popupHoverDelayMs"); delay && delay->IsNumber())
            s.popupHoverDelayMs = NormalizePopupHoverDelayMs(delay->number);
    bool b = false;
    if (ReadBoolField(text, "glassEnabled", b)) s.glassEnabled = b;
    if (ReadDoubleField(text, "glassBlurRadius", v)) s.glassBlurRadius = (float)v;
    if (ReadDoubleField(text, "contentTheme", v)) s.contentTheme = std::clamp(static_cast<int>(v), 0, 1);
    bool b2 = false;
    if (ReadBoolField(text, "acrylicEnabled", b2)) s.acrylicEnabled = b2;
    bool b3 = false;
    if (ReadBoolField(text, "showCategoryTabCounts", b3))
        s.showCategoryTabCounts = b3;
    s.showGroupTabCounts = false;
    if (ReadBoolField(text, "showGroupTabCounts", b3))
        s.showGroupTabCounts = b3;
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
    // A selected built-in theme is a live recipe, not a saved material snapshot.
    // Profiles without a selection and explicit custom profiles retain their values.
    if (backgroundPresetLoaded && s.backgroundPreset != kAppearancePresetCustom)
        ApplyAppearancePreset(s, s.backgroundPreset);
    else if (!backgroundPresetLoaded)
        s.backgroundPreset = kAppearancePresetCustom;
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
    if (!snowdesktop::ValidatePanelGradient(s.panelGradient)) return false;
    std::ostringstream file;
    // Applied theme snapshots must survive a settings save/reload exactly.
    file.precision(std::numeric_limits<float>::max_digits10);
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
    file << "  \"edgeLight\": " << snowdesktop::EncodeEdgeLight(snowdesktop::NormalizeEdgeLight(s.edgeLight)) << ",\n";
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
    file << "  \"panelGradient\": " << snowdesktop::EncodePanelGradient(s.panelGradient) << ",\n";
    file << "  \"barHeight\": " << s.barHeight << ",\n";
    file << "  \"scrollableTitleBarOnTop\": "
         << (s.scrollableTitleBarOnTop ? "true" : "false") << ",\n";
    file << "  \"widgetTransformCursors\": "
         << (s.widgetTransformCursors ? "true" : "false") << ",\n";
    file << "  \"categorizedTabHeight\": "
         << std::clamp(
                s.categorizedTabHeight,
                24.0f, 48.0f)
         << ",\n";
    file << "  \"luaWidgetContentRowHeight\": "
         << std::clamp(s.luaWidgetContentRowHeight, 18.0f, 48.0f)
         << ",\n";
    file << "  \"showCategoryTabCounts\": "
         << (s.showCategoryTabCounts ? "true" : "false") << ",\n";
    file << "  \"showGroupTabCounts\": "
         << (s.showGroupTabCounts ? "true" : "false") << ",\n";
    file << "  \"groupTabHoverSwitch\": "
         << (s.groupTabHoverSwitch ? "true" : "false") << ",\n";
    file << "  \"categoryTabHoverSwitch\": "
         << (s.categoryTabHoverSwitch ? "true" : "false") << ",\n";
    file << "  \"tabHoverDelayMs\": "
         << NormalizeTabHoverDelayMs(s.tabHoverDelayMs) << ",\n";
    file << "  \"popupHoverOpen\": "
         << (s.popupHoverOpen ? "true" : "false") << ",\n";
    file << "  \"popupHoverDelayMs\": "
         << NormalizePopupHoverDelayMs(s.popupHoverDelayMs) << ",\n";
    file << "  \"popupCloseOnMouseLeave\": "
         << (s.popupCloseOnMouseLeave ? "true" : "false") << ",\n";
    file << "  \"backgroundPreset\": " << s.backgroundPreset << ",\n";
    file << "  \"cornerRadius\": " << s.cornerRadius << ",\n";
    auto pinAppearance = s;
    NormalizeWindowPinBorderAppearance(pinAppearance);
    file << "  \"windowPinBorderEnabled\": " << (s.windowPinBorderEnabled ? "true" : "false") << ",\n";
    file << "  \"windowPinBorderR\": " << pinAppearance.windowPinBorderR << ",\n";
    file << "  \"windowPinBorderG\": " << pinAppearance.windowPinBorderG << ",\n";
    file << "  \"windowPinBorderB\": " << pinAppearance.windowPinBorderB << ",\n";
    file << "  \"windowPinBorderWidth\": " << pinAppearance.windowPinBorderWidth << ",\n";
    file << "  \"windowPinBorderOpacity\": " << pinAppearance.windowPinBorderOpacity << ",\n";
    file << "  \"contextMenuStyle\": "
         << std::clamp(s.contextMenuStyle, 0, snowdesktop::modern_menu::kMaximumAppearance) << ",\n";
    file << "  \"contextMenuStyleVersion\": 1,\n";
    file << "  \"glassEnabled\": " << (s.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"glassBlurRadius\": " << s.glassBlurRadius << ",\n";
    file << "  \"contentTheme\": " << s.contentTheme << ",\n";
    file << "  \"acrylicEnabled\": " << (s.acrylicEnabled ? "true" : "false") << "\n";
    file << "}\n";
    return snowdesktop::atomic_file::WriteAll(path, file.str());
}
