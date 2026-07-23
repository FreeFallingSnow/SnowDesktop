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
    s.widgetAlpha = 0.34f; s.widgetBorderAlpha = 0.32f;
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
    s.widgetAlpha = 0.12f; s.widgetBorderAlpha = 0.20f;
    s.gradientEndA = 0.12f;
    s.backgroundPreset = 1;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassDarkPreset()
{
    PersonalizationSettings s = DarkPreset();
    s.widgetBgR = 0.05f; s.widgetBgG = 0.07f; s.widgetBgB = 0.10f;
    s.widgetBorderR = 0.78f; s.widgetBorderG = 0.88f; s.widgetBorderB = 1.0f;
    s.widgetAlpha = 0.22f; s.widgetBorderAlpha = 0.24f;
    s.backgroundPreset = 6;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.glassBlurRadius = 24.0f;
    return s;
}

PersonalizationSettings PersonalizationSettings::GlassLightPreset()
{
    PersonalizationSettings s = LightPreset();
    s.widgetBgR = 0.92f; s.widgetBgG = 0.96f; s.widgetBgB = 1.0f;
    s.widgetBorderR = 1.0f; s.widgetBorderG = 1.0f; s.widgetBorderB = 1.0f;
    s.backgroundPreset = 7;
    s.widgetAlpha = 0.08f; s.widgetBorderAlpha = 0.28f;
    s.gradientEndA = 0.0f;
    s.glassEnabled = true;
    s.glassBlurRadius = 22.0f;
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
    switch (NormalizeAppearancePresetId(presetId))
    {
    case kAppearancePresetLight:
        s = PersonalizationSettings::LightPreset();
        s.widgetBgR = 0.965f; s.widgetBgG = 0.973f; s.widgetBgB = 0.988f;
        s.widgetBorderR = 0.706f; s.widgetBorderG = 0.745f; s.widgetBorderB = 0.784f;
        s.widgetAlpha = 0.96f; s.widgetBorderAlpha = 0.70f;
        s.glassEnabled = false;
        break;
    case kAppearancePresetGlassDark:
        s = PersonalizationSettings::GlassDarkPreset();
        s.widgetBgR = 0.055f; s.widgetBgG = 0.071f; s.widgetBgB = 0.102f;
        s.widgetBorderR = 0.549f; s.widgetBorderG = 0.620f; s.widgetBorderB = 0.741f;
        s.widgetAlpha = 0.68f; s.widgetBorderAlpha = 0.72f;
        s.glassEnabled = true;
        s.glassBlurRadius = 28.0f;
        break;
    case kAppearancePresetGlassLight:
        s = PersonalizationSettings::GlassLightPreset();
        s.widgetBgR = 0.930f; s.widgetBgG = 0.950f; s.widgetBgB = 0.980f;
        s.widgetBorderR = 0.700f; s.widgetBorderG = 0.760f; s.widgetBorderB = 0.840f;
        s.widgetAlpha = 0.74f; s.widgetBorderAlpha = 0.76f;
        s.glassEnabled = true;
        s.glassBlurRadius = 24.0f;
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
bool LoadPersonalization(const wchar_t* path, PersonalizationSettings& s)
{
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
    if (ReadDoubleField(text, "gradientEndA", v)) s.gradientEndA = (float)v;
    if (ReadDoubleField(text, "barHeight", v)) s.barHeight = (float)v;
    if (ReadDoubleField(text, "backgroundPreset", v))
    {
        s.backgroundPreset = NormalizeAppearancePresetId((int)v);
    }
    if (ReadDoubleField(text, "cornerRadius", v)) s.cornerRadius = (float)v;
    bool b = false;
    if (ReadBoolField(text, "glassEnabled", b)) s.glassEnabled = b;
    if (ReadDoubleField(text, "glassBlurRadius", v)) s.glassBlurRadius = (float)v;
    if (ReadDoubleField(text, "contentTheme", v)) s.contentTheme = std::clamp(static_cast<int>(v), 0, 1);
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
    file << "  \"gradientEndA\": " << s.gradientEndA << ",\n";
    file << "  \"barHeight\": " << s.barHeight << ",\n";
    file << "  \"backgroundPreset\": " << s.backgroundPreset << ",\n";
    file << "  \"cornerRadius\": " << s.cornerRadius << ",\n";
    file << "  \"glassEnabled\": " << (s.glassEnabled ? "true" : "false") << ",\n";
    file << "  \"glassBlurRadius\": " << s.glassBlurRadius << ",\n";
    file << "  \"contentTheme\": " << s.contentTheme << "\n";
    file << "}\n";
    return true;
}
