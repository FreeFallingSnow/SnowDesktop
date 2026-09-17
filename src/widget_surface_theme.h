#pragma once

#include <string_view>

/**
 * @struct LuaWidgetTheme
 * @brief 小部件主题色定义，控制背景、边框和渐变透明度
 *
 * 当小部件启用自定义样式时，引擎使用此结构中的颜色值替代默认渲染。
 *
 * @note 颜色字段采用 ARGB 格式（0xAARRGGBB），Alpha 通道默认不透明。
 */
struct LuaWidgetTheme
{
    int bg = 0x151A21;          ///< 背景色（ARGB 格式，默认深灰蓝）
    int border = 0xFFFFFF;      ///< 边框色（ARGB 格式，默认白色）
    float alpha = 0.36f;        ///< 背景透明度（0~1，默认 0.36）
    float borderAlpha = 0.40f;  ///< 边框透明度（0~1，默认 0.40）
    float gradientEndA = 0.65f; ///< 渐变末端透明度（0~1，默认 0.65）
    float cornerRadius = 12.0f; ///< 圆角半径（cu）
    int contentTheme = 0;       ///< 文字颜色主题 (0=浅色/白字, 1=深色/黑字)
};

namespace snowdesktop::widget_runtime
{
// Auxiliary surfaces use the host popup palette; desktop and detached previews
// retain their own palette. The host never overwrites a widget's stored theme.
inline LuaWidgetTheme ResolveSurfaceTheme(const LuaWidgetTheme& desktop,
    const LuaWidgetTheme* popup, std::string_view surface) noexcept
{
    return popup && (surface == "panel" || surface == "dialog" || surface == "popover")
        ? *popup : desktop;
}
}
