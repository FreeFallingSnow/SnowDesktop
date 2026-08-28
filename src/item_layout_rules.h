#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>

namespace snowdesktop::item_layout_rules
{

/**
 * @brief 判断组件是否应参与桌面网格尺寸变化后的重新安置。
 *
 * 组合内组件由其宿主负责布局；Dock 专属组件使用 Dock 伪页面保存位置，
 * 两者都不属于桌面网格，不能被当作越界组件迁回桌面。
 */
constexpr bool ShouldRelayoutDesktopWidget(
    bool grouped, bool dockExclusive)
{
    return !grouped && !dockExclusive;
}

constexpr int RoundPositive(float value)
{
    return value <= 0.0f
        ? 0
        : static_cast<int>(value + 0.5f);
}

/**
 * @brief 图标与标题之间的缩放间距。
 *
 * 四个逻辑像素能让中文标题与图标保持清晰分隔，同时在缩小的弹窗
 * 单元格中仍至少保留一个设备像素。
 */
constexpr int TitleGap(float layoutScale)
{
    return std::max(
        1, RoundPositive(4.0f * layoutScale));
}

/**
 * @brief 为两行标题保留完整行高和一个像素的抗锯齿余量。
 *
 * 第三行由 VisibleTextLengthForLineLimit 在排版阶段移除，因此这里
 * 可以安全保留中文第二行底部所需的绘制余量。
 */
constexpr int CollapsedTextHeight(float lineHeight)
{
    if (lineHeight <= 0.0f) return 1;
    const float twoLines = lineHeight * 2.0f;
    const int truncated = static_cast<int>(twoLines);
    const int roundedUp = truncated +
        (twoLines > static_cast<float>(
            truncated) ? 1 : 0);
    return std::max(
        1, roundedUp + 1);
}

/**
 * @brief 为给定数量的标题行保留完整行高和抗锯齿余量。
 *
 * 选中态使用实际 DirectWrite 行数调用此函数，因此高度随完整文件名
 * 增长，不再设置固定的最大显示行数。
 */
constexpr int TextHeightForLineCount(
    float lineHeight,
    std::size_t lineCount)
{
    if (lineHeight <= 0.0f) return 1;
    const double height =
        static_cast<double>(lineHeight) *
        static_cast<double>(
            std::max<std::size_t>(1, lineCount));
    if (height >= static_cast<double>(
            std::numeric_limits<int>::max() - 1))
    {
        return std::numeric_limits<int>::max();
    }
    const int truncated = static_cast<int>(height);
    const int roundedUp = truncated +
        (height > static_cast<double>(truncated)
            ? 1 : 0);
    return roundedUp + 1;
}

/**
 * @brief 根据排版后的行度量截取前 maxLines 条视觉行。
 *
 * 最后一条可见行末尾的显式换行符必须移除，否则 DirectWrite 会再
 * 创建一个空白行。LineMetric 需提供 length 与 newlineLength 字段。
 */
template <typename LineMetric>
constexpr std::size_t VisibleTextLengthForLineLimit(
    const LineMetric* lines,
    std::size_t lineCount,
    std::size_t maxLines,
    std::size_t textLength)
{
    if (!lines || maxLines == 0)
        return 0;
    if (lineCount <= maxLines)
        return textLength;

    const std::size_t visibleLines =
        std::min(lineCount, maxLines);
    std::size_t visibleLength = 0;
    for (std::size_t i = 0;
         i < visibleLines; ++i)
    {
        visibleLength +=
            static_cast<std::size_t>(
                lines[i].length);
    }

    const std::size_t trailingNewline =
        static_cast<std::size_t>(
            lines[visibleLines - 1].
                newlineLength);
    visibleLength -= std::min(
        visibleLength, trailingNewline);
    return std::min(
        visibleLength, textLength);
}

constexpr int AvailableIconHeight(
    int cellHeight,
    int topInset,
    int titleGap,
    int textHeight)
{
    return std::max(
        1,
        cellHeight -
            std::max(0, topInset) -
            std::max(0, titleGap) -
            std::max(0, textHeight));
}

} // namespace snowdesktop::item_layout_rules
