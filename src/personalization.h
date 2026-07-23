/**
 * @file personalization.h
 * @brief 个性化外观设置
 * @details 定义组件背景色、边框色、透明度等外观参数的存储结构与序列化接口，
 *          支持暗色/亮色预设方案，通过 JSON 文件持久化用户偏好。
 */

#pragma once

#include <d2d1_1.h>
#include <string>

constexpr int kAppearancePresetDark = 0;
constexpr int kAppearancePresetLight = 1;
constexpr int kAppearancePresetGlassDark = 6;
constexpr int kAppearancePresetGlassLight = 7;
constexpr int kAppearancePresetCustom = 9;

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
     * @brief 组件背景透明度
     * @details 控制背景填充以及渐变起始端的 Alpha 通道值，
     *          取值范围 [0.0f, 1.0f]，0.0 完全透明，1.0 完全不透明。
     */
    float widgetAlpha = 0.36f;

    /**
     * @brief 组件边框透明度
     * @details 独立控制边框描边的 Alpha 通道值。
     */
    float widgetBorderAlpha = 0.40f;

    /**
     * @brief 渐变底部末端 Alpha
     * @details 组件底部渐变结束端的 Alpha 通道值，与 widgetAlpha 配合
     *          形成从上到下的渐变透明效果，取值范围 [0.0f, 1.0f]。
     */
    float gradientEndA = 0.65f;

    /** @brief 独立的组件底栏高度，不属于主题预设。 */
    float barHeight = 24.0f;

    int backgroundPreset = 0;
    /** @brief 独立的组件圆角半径，不属于主题预设。 */
    float cornerRadius = 12.0f;

    /**
     * @brief 毛玻璃背景开关（苹果 Dock 效果）
     * @details 开启后由 DWM 原生合成器模糊面板背后的桌面内容，
     *          填充色作为半透明色调叠加，边框切换为玻璃边缘光渐变描边。
     */
    bool glassEnabled = false;

    /**
     * @brief 毛玻璃模糊半径（像素），取值约 [4.0f, 48.0f]
     */
    float glassBlurRadius = 24.0f;

    /**
     * @brief 文字颜色主题 (0=浅色/白字, 1=深色/黑字)
     * @details 影响任务栏文字图标、Dock组件标题和右下角图标、Lua组件文字颜色。
     *          默认浅色（白字），与现有主题预设一致。
     */
    int contentTheme = 0;

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
    static PersonalizationSettings GlassDarkPreset();
    static PersonalizationSettings GlassLightPreset();
};

/** @brief 将旧版或无效预设 ID 映射到现有四个主题。 */
int NormalizeAppearancePresetId(int presetId);

/** @brief 根据预设 ID 创建深色、浅色或对应毛玻璃主题。 */
PersonalizationSettings MakeAppearancePreset(int presetId);

/** @brief 根据预设 ID 创建针对快捷搜索可读性优化的外观主题。 */
PersonalizationSettings MakeQuickNavigationAppearancePreset(int presetId);

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
