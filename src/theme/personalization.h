#pragma once
#include "personalization_types.h"
#include "panel_gradient.h"
#include "edge_light_settings.h"
#include <string>

float NormalizePopupHoverDelayMs(double delay);
float NormalizeTabHoverDelayMs(double delay);
void NormalizeWindowPinBorderAppearance(PersonalizationSettings& settings);

/** @brief 将旧版或无效预设 ID 映射到现有主题。 */
int NormalizeAppearancePresetId(int presetId);

/** @brief 根据预设 ID 创建纯色、毛玻璃、亚克力或自定义主题。 */
PersonalizationSettings MakeAppearancePreset(int presetId);

/** Apply current material defaults without changing independent layout/interaction preferences. */
void ApplyAppearancePreset(PersonalizationSettings& settings, int presetId);

/** @brief 根据预设 ID 创建针对快捷搜索可读性优化的外观主题。 */
PersonalizationSettings MakeQuickNavigationAppearancePreset(int presetId);

/** @brief 创建集合弹窗主题，保留弹窗可读性配色并拆分普通边框与边缘高光。 */
PersonalizationSettings MakeCollectionPopupAppearancePreset(int presetId);

/**
 * @brief 从 JSON 文件加载个性化设置
 * @param path   JSON 配置文件路径（UTF-16 宽字符）
 * @param s      [out] 接收加载结果的结构体引用
 * @return true  加载成功
 * @return false 加载失败（文件不存在或解析出错）
 */
bool LoadPersonalization(
    const wchar_t* path,
    PersonalizationSettings& s,
    bool* categorizedTabHeightLoaded = nullptr);

/**
 * @brief 将个性化设置保存到 JSON 文件
 * @param path JSON 配置文件路径（UTF-16 宽字符）
 * @param s    待保存的设置结构体常量引用
 * @return true  保存成功
 * @return false 保存失败（写入错误）
 */
bool SavePersonalization(const wchar_t* path, const PersonalizationSettings& s);

/**
 * @brief 获取个性化设置文件的默认路径
 * @return 包含完整路径的宽字符串，路径格式由上层调用方约定
 * @details 通常在用户数据目录下生成 "personalization.json" 文件名
 */
std::wstring GetPersonalizationPath();
