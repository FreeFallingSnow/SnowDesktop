#pragma once

namespace snowdesktop::item_render_layer_rules
{

struct TitleLayerPlan
{
    bool drawWithItem = true;
    bool drawInForeground = false;
};

/**
 * @brief 将展开的选中标题安排到图标之后的前景绘制阶段。
 *
 * 普通标题仍与所属图标一起绘制。选中标题可能向下跨越多个单元格，
 * 必须等所有图标本体完成后再绘制，避免被后续图标遮挡。
 */
constexpr TitleLayerPlan ResolveTitleLayerPlan(
    bool selected)
{
    return selected
        ? TitleLayerPlan{ false, true }
        : TitleLayerPlan{ true, false };
}

} // namespace snowdesktop::item_render_layer_rules
