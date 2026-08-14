#include "app.h"
#include "widgets/widget_chrome_rules.h"

// Desktop-item and standalone-widget hit testing.

int DesktopApp::HitTestItem(POINT pt) const
{
    // Backward-compat wrapper: returns items_ index for Shell/COM code
    DesktopIcon* icon = HitTestIcon(pt);
    if (!icon) return -1;
    DesktopItem* di = icon->GetDesktopItem();
    for (size_t j = 0; j < items_.size(); ++j)
        if (&items_[j] == di) return static_cast<int>(j);
    return -1;
}

/**
 * @brief 命中测试：根据点坐标查找桌面图标对象
 * @param pt 客户端坐标点
 * @return 指向 DesktopIcon 的指针，未找到返回 nullptr
 */
DesktopIcon* DesktopApp::HitTestIcon(POINT pt) const
{
    for (int i = static_cast<int>(items_oo_.size()) - 1; i >= 0; --i)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(items_oo_[i].get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (!di || IsRectEmptyRect(di->bounds)) continue;
        if (!di->layoutKey.empty() && collectedKeysCache_.count(ToUpperInvariant(di->layoutKey))) continue;
        RECT selRect = GetItemSelectionRect(di->bounds, di->selected);
        if (PtInRect(&selRect, pt)) return icon;
    }
    return nullptr;
}

/**
 * @brief 判断指定桌面项是否位于任意窗口小部件内
 * @param item 要检查的桌面项
 * @return 若在任意小部件内返回 true
 */
bool DesktopApp::IsItemInAnyWidget(const DesktopItem& item) const
{
    std::wstring key = ToUpperInvariant(item.layoutKey);
    if (key.empty()) return false;
    return collectedKeysCache_.contains(key);
}

/**
 * @brief 获取独立窗口小部件的框架矩形（考虑网格间距）
 * @param widget 桌面小部件引用
 * @return 框架矩形
 */
float DesktopApp::GetWidgetCellScale(const DesktopWidget& widget) const
{
    return widget.cellScale;
}

int DesktopApp::GetComponentEdgeMargin(
    const GridPage& page, bool vertical) const
{
    const float cellScale = CalculateWidgetCellScale(
        page.cellWidth, page.cellHeight);
    return snowdesktop::widget_spacing_rules::EffectiveComponentEdgeGap(
        vertical ? page.marginX : page.marginY,
        vertical ? page.gapX : page.gapY,
        cellScale,
        componentSpacingScale_);
}

RECT DesktopApp::GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const
{
    RECT rect = widget.bounds;
    const float cellScale = GetWidgetCellScale(widget);
    for (const auto& page : gridPages_)
    {
        if (page.id != widget.gridCell.pageId) continue;
        int halfGapX = std::max(ScaleWidgetCu(2.0f, cellScale), page.gapX / 2);
        int halfGapY = std::max(ScaleWidgetCu(2.0f, cellScale), page.gapY / 2);
        rect.left   -= halfGapX;
        rect.top    -= halfGapY;
        rect.right  += halfGapX;
        rect.bottom += halfGapY;
        break;
    }
    const int inset = snowdesktop::widget_spacing_rules::ScaledComponentInset(
        cellScale, componentSpacingScale_);
    if (rect.right - rect.left > inset * 4 && rect.bottom - rect.top > inset * 4)
        InflateRect(&rect, -inset, -inset);
    return rect;
}

RECT DesktopApp::GetLuaWidgetHostActionRect(
    const DesktopWidget& widget) const
{
    const RECT frame = GetStandaloneWidgetFrameRect(widget);
    const float cellScale = GetWidgetCellScale(widget);
    const int horizontalInset = ScaleWidgetCu(14.0f, cellScale);
    const int buttonHeight = ScaleWidgetCu(31.0f, cellScale);
    const int bottomInset = ScaleWidgetCu(14.0f, cellScale);
    const RECT moveHandle = GetStandaloneWidgetMoveHandleRect(widget);
    const LONG contentBottom = std::max<LONG>(frame.top,
        moveHandle.top - ScaleWidgetCu(5.0f, cellScale));
    const LONG bottom = std::max<LONG>(frame.top + buttonHeight,
        contentBottom - bottomInset);
    return {
        frame.left + horizontalInset,
        bottom - buttonHeight,
        frame.right - horizontalInset,
        bottom
    };
}

/**
 * @brief 获取独立窗口小部件的移动手柄矩形
 * @param widget 桌面小部件引用
 * @return 移动手柄矩形
 */
RECT DesktopApp::GetStandaloneWidgetMoveHandleRect(const DesktopWidget& widget) const
{
    RECT frame = GetStandaloneWidgetFrameRect(widget);
    const float cellScale = GetWidgetCellScale(widget);
    const auto personalization = settingsWindow_
        ? settingsWindow_->GetPersonalization()
        : PersonalizationSettings{};
    const float barHeight = personalization.barHeight;
    const int handleHeight = ScaleWidgetCu(barHeight, cellScale);
    const int sideInset = snowdesktop::widget_chrome_rules::BottomBarSideInset(
        ScaleWidgetCu(personalization.cornerRadius, cellScale),
        handleHeight,
        ScaleWidgetCu(4.0f, cellScale),
        ScaleWidgetCu(2.0f, cellScale));
    const int maxSideInset = std::max<int>(
        0, (frame.right - frame.left - 1) / 2);
    const int clampedSideInset = std::min(sideInset, maxSideInset);
    return {
        frame.left + clampedSideInset,
        std::max<LONG>(frame.top,
            frame.bottom - handleHeight - ScaleWidgetCu(2.0f, cellScale)),
        frame.right - clampedSideInset,
        frame.bottom - ScaleWidgetCu(2.0f, cellScale)
    };
}

/**
 * @brief 获取独立窗口小部件的调整大小手柄矩形
 * @param widget 桌面小部件引用
 * @return 调整大小手柄矩形
 */
RECT DesktopApp::GetStandaloneWidgetResizeHandleRect(const DesktopWidget& widget) const
{
    RECT handle = GetStandaloneWidgetMoveHandleRect(widget);
    const float barHeight = settingsWindow_ ? settingsWindow_->GetPersonalization().barHeight : 24.0f;
    const int handleWidth = ScaleWidgetCu(barHeight, GetWidgetCellScale(widget));
    return {
        std::max<LONG>(handle.left, handle.right - handleWidth),
        handle.top,
        handle.right,
        handle.bottom
    };
}

/**
 * @brief 对独立窗口小部件进行命中测试
 * @param widgetIndex 小部件索引
 * @param pt 客户端坐标点
 * @return 命中类型（无/移动手柄/调整大小手柄/内容区域）
 */
WidgetHit DesktopApp::HitTestStandaloneWidget(size_t widgetIndex, POINT pt) const
{
    if (widgetIndex >= widgets_.size()) return WidgetHit::None;
    const DesktopWidget& widget = widgets_[widgetIndex];
    if (desktopIconsHidden_ && !widget.keepWhenDesktopHidden)
        return WidgetHit::None;
    if (widget.type != DesktopWidgetType::LuaScript) return WidgetHit::None;

    RECT frame = GetStandaloneWidgetFrameRect(widget);
    if (!PtInRect(&frame, pt)) return WidgetHit::None;
    RECT resize = GetStandaloneWidgetResizeHandleRect(widget);
    if (PtInRect(&resize, pt)) return WidgetHit::ResizeHandle;
    RECT move = GetStandaloneWidgetMoveHandleRect(widget);
    if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;
    return WidgetHit::Content;
}

/**
 * @brief 命中测试：查找鼠标点所在的独立小部件索引
 * @param pt 客户端坐标点
 * @return 小部件索引，未找到返回 (size_t)-1
 */
size_t DesktopApp::HitTestStandaloneWidgetIndex(POINT pt) const
{
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t i = n - 1;
        if (HitTestStandaloneWidget(i, pt) != WidgetHit::None)
            return i;
    }
    return static_cast<size_t>(-1);
}

/**
 * @brief 将宽字符串转换为 UTF-8 编码（用于 Lua 交互）
 * @param value 输入的宽字符串
 * @return UTF-8 编码的字符串
 */
