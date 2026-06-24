/**
 * @file slot.cpp
 * @brief 插槽(Slot)类的实现
 *
 * Slot 表示容器(Container)中的一个单元格位置，负责管理该位置内
 * 项目的持有、命中测试、拖放操作以及拖放指示器的绘制。
 * 每个 Slot 都隶属于一个父 Container，拥有固定的边界矩形和索引位置。
 *
 * @author FreeFalling_Snow
 * @date 2026-06-07
 */

#include "slot.h"
#include "container.h"
#include "item.h"
#include "constants.h"
#include <wrl/client.h>
#include <algorithm>

using Microsoft::WRL::ComPtr;

/**
 * @brief 构造函数
 * @param parent 所属的父容器指针
 * @param bounds 插槽的边界矩形（像素坐标）
 * @param index  插槽在父容器中的索引位置
 *
 * 初始化插槽所属容器、边界矩形和索引，此时插槽内尚无项目(item_ 为 nullptr)。
 */
Slot::Slot(Container* parent, RECT bounds, size_t index)
    : parent_(parent), bounds_(bounds), index_(index) {}

/**
 * @brief 获取所属父容器
 * @return Container* 父容器指针
 */
Container* Slot::GetParent() const { return parent_; }

/**
 * @brief 获取插槽的边界矩形
 * @return RECT 边界矩形（像素坐标）
 */
RECT Slot::GetBounds() const { return bounds_; }

/**
 * @brief 获取插槽中存放的项目指针
 * @return Item* 项目指针；若插槽为空则返回 nullptr
 */
Item* Slot::GetItem() const { return item_; }

/**
 * @brief 设置插槽中存放的项目
 * @param item 待设置的项目指针
 */
void Slot::SetItem(Item* item) { item_ = item; }

/**
 * @brief 获取插槽在父容器中的索引
 * @return size_t 索引值
 */
size_t Slot::GetIndex() const { return index_; }

/**
 * @brief 判断插槽是否为空
 * @return true  插槽内无项目
 * @return false 插槽内已有项目
 */
bool Slot::IsEmpty() const { return item_ == nullptr; }

/**
 * @brief 计算项目图标的绘制区域
 *
 * 根据插槽的尺寸决定图标大小和位置：
 * - 当插槽高度小于 50px 时（紧凑布局），图标居中于插槽左侧，最大 32px；
 * - 否则（常规布局），图标居中于插槽顶部，尺寸根据可用空间自适应，
 *   同时为下方的文字区域(kTextHeight)预留空间。
 *
 * @return RECT 图标在屏幕坐标系中的矩形区域
 */
RECT Slot::GetIconRect() const
{
    const int cellW = bounds_.right - bounds_.left;
    const int cellH = bounds_.bottom - bounds_.top;
    if (cellH < 50)
    {
        const int iconSz = std::min(32, cellH - 4);
        return RECT{
            bounds_.left + 4,
            bounds_.top + (cellH - iconSz) / 2,
            bounds_.left + 4 + iconSz,
            bounds_.top + (cellH + iconSz) / 2
        };
    }
    const int maxIconW = std::max(16, cellW - 8);
    const int maxIconH = std::max(16, cellH - kTextHeight - 8);
    const int iconSz = std::min(maxIconW, maxIconH);
    const int iconX = bounds_.left + (cellW - iconSz) / 2;
    const int iconY = bounds_.top + 2;
    return RECT{ iconX, iconY, iconX + iconSz, iconY + iconSz };
}

/**
 * @brief 命中测试：判断鼠标点击位置落在插槽的哪个区域
 *
 * 根据点击位置相对于插槽边界和内部项目的分布判定 HitRegion：
 * - 不在边界内         → None
 * - 插槽为空           → Empty
 * - 落在图标区域(Handoff) → 判定为 Handoff，表示触发"交给"操作
 * - 否则根据父容器的排布方向(BarStyle)区分"插入之前/之后"
 *
 * @param pt 鼠标点击的屏幕坐标点
 * @return HitRegion 命中区域类型枚举值
 */
HitRegion Slot::HitTest(POINT pt) const
{
    if (!PtInRect(&bounds_, pt)) return HitRegion::None;

    if (IsEmpty()) return HitRegion::Empty;

    // Handoff: hit on the icon area
    RECT iconRect = GetIconRect();
    RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                         iconRect.right + 4, iconRect.bottom + 4 };
    if (PtInRect(&handoffRect, pt)) return HitRegion::Handoff;

    // Sort: which half of the slot?
    const int cellW = bounds_.right - bounds_.left;
    const int cellH = bounds_.bottom - bounds_.top;

    BarStyle style = parent_ ? parent_->GetInsertionStyle() : BarStyle::HBar;
    if (style == BarStyle::VBar)
        return (pt.x < bounds_.left + cellW / 2) ? HitRegion::SortBefore : HitRegion::SortAfter;
    return (pt.y < bounds_.top + cellH / 2) ? HitRegion::SortBefore : HitRegion::SortAfter;
}

/**
 * @brief 获取拖放操作时的提示文字
 *
 * 根据命中区域类型返回对应的中文提示字符串：
 * - Empty      → "移动到此空位"
 * - SortBefore/SortAfter → "重新排序"
 * - Handoff    → "交给「项目名」处理" 或 "交给此项目处理"
 * - None       → 空字符串
 *
 * @param region      命中区域类型
 * @param sourceItems 拖拽来源的项目列表（当前未使用，仅用于接口兼容）
 * @return std::wstring 提示文字（宽字符串）
 */
std::wstring Slot::GetDropHint(HitRegion region, const std::vector<Item*>& sourceItems) const
{
    switch (region)
    {
    case HitRegion::Empty:
        return L"移动到此空位";
    case HitRegion::SortBefore:
    case HitRegion::SortAfter:
        return L"重新排序";
    case HitRegion::Handoff:
        if (item_) return L"交给「" + item_->GetTitle() + L"」处理";
        return L"交给此项目处理";
    default:
        return L"";
    }
    (void)sourceItems;
}

/**
 * @brief 执行拖放操作
 *
 * 将拖放事件委托给父容器处理。父容器负责实际的插入、排序或"交给"逻辑。
 *
 * @param region      命中区域类型，决定操作语义
 * @param sourceItems 被拖拽的项目列表
 * @param origin      拖拽操作的来源容器
 * @param mods        键盘修饰键状态（如 Ctrl、Shift 等）
 */
void Slot::ExecuteDrop(HitRegion region, const std::vector<Item*>& sourceItems, Container* origin, int mods)
{
    if (parent_)
        parent_->OnItemsDropped(sourceItems, origin, this, region, mods);
}

/**
 * @brief 绘制拖放指示器（蓝色高亮条/框）
 *
 * 根据命中区域类型在插槽上绘制视觉反馈：
 * - Empty   → 绘制半透明蓝色填充矩形 + 蓝色边框，表示可放置在此空位
 * - SortBefore/SortAfter：
 *   - 竖直排布(VBar)：在插槽左侧或右侧绘制一条竖线
 *   - 水平排布(HBar)：在插槽上方或下方绘制一条横线
 * - None / Handoff → 不绘制任何内容
 *
 * @param ctx    Direct2D 设备上下文指针
 * @param region 命中区域类型
 */
void Slot::DrawDropIndicator(ID2D1DeviceContext* ctx, HitRegion region, float itemPad) const
{
    if (!ctx || !parent_ || region == HitRegion::None || region == HitRegion::Handoff)
        return;

    BarStyle style = parent_->GetInsertionStyle();
    const float lineWidth = 3.0f;
    const D2D1_COLOR_F blue = D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.92f);

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(ctx->CreateSolidColorBrush(blue, &brush)) || !brush)
        return;

    if (region == HitRegion::Empty)
    {
        RECT r = bounds_;
        InflateRect(&r, -4, -4);
        D2D1_RECT_F rf = D2D1::RectF(
            static_cast<float>(r.left), static_cast<float>(r.top),
            static_cast<float>(r.right), static_cast<float>(r.bottom));

        ComPtr<ID2D1SolidColorBrush> fillBrush;
        ctx->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.10f), &fillBrush);
        if (fillBrush)
            ctx->FillRectangle(rf, fillBrush.Get());
        ctx->DrawRectangle(rf, brush.Get(), 2.0f);
    }
    else if (style == BarStyle::VBar)
    {
        float x;
        if (region == HitRegion::SortBefore)
            x = static_cast<float>(bounds_.left) - itemPad - lineWidth / 2.0f;
        else
            x = static_cast<float>(bounds_.right) + itemPad - lineWidth / 2.0f;

        D2D1_RECT_F rf = D2D1::RectF(
            x, static_cast<float>(bounds_.top) + 2.0f,
            x + lineWidth, static_cast<float>(bounds_.bottom) - 2.0f);
        ctx->FillRectangle(rf, brush.Get());
    }
    else // HBar
    {
        float y;
        if (region == HitRegion::SortBefore)
            y = static_cast<float>(bounds_.top) - itemPad - lineWidth / 2.0f;
        else
            y = static_cast<float>(bounds_.bottom) + itemPad - lineWidth / 2.0f;

        D2D1_RECT_F rf = D2D1::RectF(
            static_cast<float>(bounds_.left) + 4.0f, y,
            static_cast<float>(bounds_.right) - 4.0f, y + lineWidth);
        ctx->FillRectangle(rf, brush.Get());
    }
}
