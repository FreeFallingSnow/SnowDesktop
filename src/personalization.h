/**
 * @file personalization.h
 * @brief 个性化外观设置
 * @details 定义组件背景色、边框色、透明度等外观参数的存储结构与序列化接口，
 *          支持暗色/亮色预设方案，通过 JSON 文件持久化用户偏好。
 */

#pragma once

#include <d2d1_1.h>
#include <string>

/**
 * @brief 个性化设置结构体
 * @details 存储桌面组件的颜色与透明度外观参数，包含预设工厂方法。
 *          默认值为暗色预设，字段以 RGB 分量 + 独立 Alpha 值组织。
 */
struct PersonalizationSettings
{
    /**
     * @name 组件背景色 (RGB)
     * @brief 组件背景填充色的 RGB 分量，取值范围 [0.0f, 1.0f]
     */
    //@{
    float widgetBgR = 0.08f; /**< 背景红色分量 */
    float widgetBgG = 0.10f; /**< 背景绿色分量 */
    float widgetBgB = 0.13f; /**< 背景蓝色分量 */
    //@}

    /**
     * @name 组件边框色 (RGB)
     * @brief 组件边框绘制色的 RGB 分量，取值范围 [0.0f, 1.0f]
     */
    //@{
    float widgetBorderR = 1.0f; /**< 边框红色分量 */
    float widgetBorderG = 1.0f; /**< 边框绿色分量 */
    float widgetBorderB = 1.0f; /**< 边框蓝色分量 */
    //@}

    /**
     * @brief 统一透明度
     * @details 同时控制背景填充、边框以及渐变起始端的 Alpha 通道值，
     *          取值范围 [0.0f, 1.0f]，0.0 完全透明，1.0 完全不透明。
     */
    float widgetAlpha = 0.36f;

    /**
     * @brief 渐变底部末端 Alpha
     * @details 组件底部渐变结束端的 Alpha 通道值，与 widgetAlpha 配合
     *          形成从上到下的渐变透明效果，取值范围 [0.0f, 1.0f]。
     */
    float gradientEndA = 0.65f;

    float barHeight = 24.0f;

    /**
     * @brief 获取暗色预设
     * @return 暗色主题的 PersonalizationSettings 实例
     */
    static PersonalizationSettings DarkPreset();

    /**
     * @brief 获取亮色预设
     * @return 亮色主题的 PersonalizationSettings 实例
     */
    static PersonalizationSettings LightPreset();
};

/**
 * @brief 从 JSON 文件加载个性化设置
 * @param path   JSON 配置文件路径（UTF-16 宽字符）
 * @param s      [out] 接收加载结果的结构体引用
 * @return true  加载成功
 * @return false 加载失败（文件不存在或解析出错）
 */
bool LoadPersonalization(const wchar_t* path, PersonalizationSettings& s);

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
