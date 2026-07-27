#pragma once

#include <algorithm>
#include <cstddef>

namespace snowdesktop::item_layout_rules
{

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
