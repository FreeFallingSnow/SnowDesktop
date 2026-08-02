#pragma once

#include <algorithm>
#include <cmath>

namespace snowdesktop::widget_spacing_rules
{

inline constexpr float kMinimumScale = 0.50f;
inline constexpr float kMaximumScale = 2.00f;
inline constexpr float kMaximumComponentScale = 8.00f;

inline float MaximumComponentScaleForFrame(
    int frameWidth, int frameHeight, float cellScale)
{
    const float scale = std::max(0.1f, cellScale);
    const int maxInset = std::max(1, std::min(
        (std::max(1, frameWidth) - 1) / 4,
        (std::max(1, frameHeight) - 1) / 4));
    return std::clamp(
        static_cast<float>(maxInset) / (4.0f * scale),
        kMinimumScale,
        kMaximumComponentScale);
}

inline float MaximumComponentScaleForPage(
    int cellWidth, int cellHeight, int gridGapX, int gridGapY,
    float cellScale)
{
    return MaximumComponentScaleForFrame(
        std::max(1, cellWidth) + std::max(
            std::max(1, static_cast<int>(std::round(
                2.0f * std::max(0.1f, cellScale)))), gridGapX / 2) * 2,
        std::max(1, cellHeight) + std::max(
            std::max(1, static_cast<int>(std::round(
                2.0f * std::max(0.1f, cellScale)))), gridGapY / 2) * 2,
        cellScale);
}

inline float ClampComponentScale(
    float componentSpacingScale, float maximumScale)
{
    return std::clamp(
        componentSpacingScale,
        kMinimumScale,
        std::clamp(maximumScale, kMinimumScale, kMaximumComponentScale));
}

inline int ScaledComponentInset(
    float cellScale, float componentSpacingScale)
{
    return std::max(1, static_cast<int>(std::round(
        4.0f * std::max(0.1f, cellScale) * std::clamp(
            componentSpacingScale, kMinimumScale, kMaximumComponentScale))));
}

/**
 * @brief 将组件外框上下 inset 的变化均匀分配到集合各行间隙。
 *
 * 第一行随外框顶部移动；最后一行通过该偏移随外框底部对称移动。
 * 放大组件间距时最多把既有行间隙压缩到零，不会压缩包含完整标题的单元格。
 */
inline int CollectionRowOffsetForComponentSpacing(
    int rowIndex, int rowCount, int gridGapY,
    float cellScale, float componentSpacingScale)
{
    const int rows = std::max(1, rowCount);
    const int row = std::clamp(rowIndex, 0, rows - 1);
    if (row == 0 || rows == 1) return 0;

    const int baselineInset = ScaledComponentInset(cellScale, 1.0f);
    const int currentInset = ScaledComponentInset(
        cellScale, componentSpacingScale);
    const int gapCount = rows - 1;
    const int minimumTotalOffset = -std::max(0, gridGapY) * gapCount;
    const int totalOffset = std::max(
        minimumTotalOffset, 2 * (baselineInset - currentInset));
    return static_cast<int>(std::round(
        static_cast<float>(row * totalOffset) /
        static_cast<float>(gapCount)));
}

/**
 * @brief 集合大文件夹模式在不压缩内容单元格时允许的组件间距上限。
 *
 * 单元格自身包含图标、标题间距和完整标题；只有行间隙可以被外框 inset
 * 吸收，因此上限由行数和实际纵向网格间隙共同决定。
 */
inline float MaximumComponentScaleForCollectionRows(
    int rowCount, int gridGapY, float cellScale)
{
    const float scale = std::max(0.1f, cellScale);
    const int gapCount = std::max(0, rowCount - 1);
    const int baselineInset = ScaledComponentInset(scale, 1.0f);
    const int maxInset = baselineInset +
        std::max(0, gridGapY) * gapCount / 2;
    return std::clamp(
        std::max(1.0f,
            static_cast<float>(maxInset) / (4.0f * scale)),
        kMinimumScale,
        kMaximumComponentScale);
}

inline int EffectiveComponentEdgeGap(
    int pageMargin, int gridGap, float cellScale,
    float componentSpacingScale)
{
    const int minimumHalfGap = std::max(1, static_cast<int>(std::round(
        2.0f * std::max(0.1f, cellScale))));
    const int absorbedHalfGap = std::max(minimumHalfGap, gridGap / 2);
    return std::max(0, pageMargin - absorbedHalfGap +
        ScaledComponentInset(cellScale, componentSpacingScale));
}

} // namespace snowdesktop::widget_spacing_rules
