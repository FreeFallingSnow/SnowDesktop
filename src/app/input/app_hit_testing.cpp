#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "common/utils.h"
#include "settings/animation_settings.h"
#include "widget/view/widget_composition_layer_rules.h"
#include "widgets/widget_chrome_rules.h"
#include "widgets/storage_title_bar_layout.h"
#include "widgets/single_cell_widget_rules.h"

#include <cmath>

// Desktop-item and standalone-widget hit testing.

bool DesktopApp::UpdateWidgetHandleCursor(POINT point)
{
    const bool transformCursors = CurrentPersonalization().widgetTransformCursors;
    const LPCWSTR resizeCursor = transformCursors ? IDC_SIZENWSE : IDC_ARROW;
    const LPCWSTR moveCursor = transformCursors ? IDC_SIZEALL : IDC_ARROW;
    LPCWSTR cursor = nullptr;
    if (widgetAction_ == WidgetAction::Resize) cursor = resizeCursor;
    else if (widgetAction_ == WidgetAction::Move) cursor = moveCursor;
    else if (!dragSession_.IsActive() && !HasActiveContextMenuSession() && !IsPointOccludedByOpenPopup(point))
    {
        WidgetHit hit = WidgetHit::None;
        const auto standalone = HitTestStandaloneWidgetIndex(point);
        if (standalone < widgets_.size()) hit = HitTestStandaloneWidget(standalone, point);
        if (hit == WidgetHit::None)
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                if (desktopIconsHidden_ && !IsRetainedContainer(it->get())) continue;
                const auto* widget = dynamic_cast<WidgetContainer*>(it->get());
                if (!widget) continue;
                hit = widget->HitTestWidget(point);
                if (hit != WidgetHit::None) break;
            }
        if (hit == WidgetHit::ResizeHandle) cursor = resizeCursor;
        else if (hit == WidgetHit::MoveHandle) cursor = moveCursor;
        else if (hit == WidgetHit::CollapseToggleBtn) cursor = IDC_HAND;
    }
    if (!cursor) return false;
    snowdesktop::private_pointer::Cursor(LoadCursorW(nullptr, cursor));
    return true;
}

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
        if (desktopIconsHidden_ && !IsRetainedLargeIcon(*di)) continue;
        RECT selRect = di->largeIcon ? GetLargeIconFrameRect(*di)
            : GetItemSelectionRect(di->bounds, di->selected);
        if (!PtInRect(&selRect, pt)) continue;
        if (!di->layoutKey.empty() &&
            collectedKeysCache_.count(
                ToUpperInvariant(di->layoutKey)))
            continue;
        return icon;
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
    return ResolveLuaWidgetFrameOwner(widget).cellScale;
}

int DesktopApp::GetComponentEdgeMargin(
    const GridPage& page, bool vertical) const
{
    const float pageVisualScale =
        GetPageItemVisualMetrics(page).layoutScale;
    return snowdesktop::layout_spacing_rules::ComponentEdgeMargin(
        vertical ? page.marginX : page.marginY,
        vertical ? page.gapX : page.gapY,
        pageVisualScale, iconSpacingScale_);
}

RECT DesktopApp::GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const
{
    const auto& owner = ResolveLuaWidgetFrameOwner(widget);
    if (&owner != &widget)
        return ResolveLuaWidgetContent(&owner) == &widget ? GetStandaloneWidgetFrameRect(owner) : RECT{};
    if (snowdesktop::single_cell_widget::IsDesktopSingleCell(widget) && widget.singleCellIconMode)
    {
        if (IsRectEmpty(&widget.bounds)) return widget.bounds;
        const RECT title = GetItemTextRect(widget.bounds, widget.selected);
        RECT frame{};
        UnionRect(&frame, &widget.bounds, &title);
        const RECT handle = snowdesktop::single_cell_widget::ArcBounds(
            snowdesktop::single_cell_widget::ResolveArc(GetItemIconRect(widget.bounds), GetWidgetCellScale(widget)), true);
        RECT withHandle{};
        UnionRect(&withHandle, &frame, &handle);
        return withHandle;
    }
    const RECT fullFrame = GetExpandedWidgetFrameRect(widget);
    const float progress = GetWidgetExpansionProgress(widget);
    if (progress >= 1.0f) return fullFrame;
    const float scale = GetWidgetCellScale(widget);
    RECT frame = snowdesktop::storage_title_bar::VisibleFrame(fullFrame, true,
        ScaleWidgetCu(GetCategorizedWidgetTabHeight(), scale), ScaleWidgetCu(2.0f, scale));
    frame.bottom += std::lround(static_cast<double>(fullFrame.bottom - frame.bottom) * progress);
    return frame;
}

bool DesktopApp::IsWidgetCollapsed(const DesktopWidget& widget) const
{
    return GetWidgetExpansionProgress(widget) <= 0.0f;
}

bool DesktopApp::CanFoldWidget(const DesktopWidget& widget) const
{
    return snowdesktop::storage_title_bar::IsCollapsed(widget,
        CurrentPersonalization().scrollableTitleBarOnTop,
        // Keep the target expanded through synchronous drop submission too.
        dragSession_.HasContext(), dragDropController_.IsExternalDragActive(),
        widgetAction_ == WidgetAction::Move);
}

float DesktopApp::GetWidgetExpansionProgress(const DesktopWidget& widget) const
{
    if (!CanFoldWidget(widget)) return 1.0f;
    if (!widget.titleBarExpandOnHover) return 0.0f;
    const auto expansion = widgetHoverExpansion_.find(widget.id);
    if (expansion == widgetHoverExpansion_.end()) return 0.0f;
    if (generalSettings_.widgetHoverExpandEffect != snowdesktop::animation::Roll)
        return expansion->second.IsInteractive() ? 1.0f : 0.0f;
    return expansion->second.GetVisual().progress;
}

bool DesktopApp::IsWidgetHoverExpansionAnimating(const DesktopWidget& widget) const
{
    if (generalSettings_.widgetHoverExpandEffect != snowdesktop::animation::Roll ||
        !widget.titleBarExpandOnHover || !CanFoldWidget(widget))
        return false;
    const auto expansion = widgetHoverExpansion_.find(widget.id);
    return expansion != widgetHoverExpansion_.end() && expansion->second.IsAnimating();
}

void DesktopApp::UpdateWidgetHoverExpansion(POINT point)
{
    namespace motion = snowdesktop::animation;
    const bool pointerAvailable = !HasActiveContextMenuSession() &&
        !IsPointOccludedByOpenPopup(point);
    bool changed = false;
    bool suppressionStillInside = false;
    for (size_t index = 0; index < widgets_.size(); ++index)
    {
        const auto& widget = widgets_[index];
        const bool eligible = widget.titleBarCollapsed && widget.titleBarExpandOnHover &&
            snowdesktop::storage_title_bar::UsesTop(widget,
                CurrentPersonalization().scrollableTitleBarOnTop) &&
            customDesktopVisible_ && !desktopPassthroughActive_ &&
            (!desktopIconsHidden_ || widget.keepWhenDesktopHidden) &&
            !IsRectEmptyRect(widget.bounds) && !IsGroupedWidget(widget);
        if (!eligible)
        {
            changed |= widgetHoverExpansion_.erase(widget.id) != 0;
            continue;
        }

        const RECT frame = GetExpandedWidgetFrameRect(widget);
        const bool inFrame = PtInRect(&frame, point) != FALSE;
        const bool suppressed = hoverExpansionSuppressedWidgetId_ == widget.id && inFrame;
        suppressionStillInside |= suppressed;
        const auto existing = widgetHoverExpansion_.find(widget.id);
        const bool wasExpanded = existing != widgetHoverExpansion_.end() &&
            existing->second.IsInteractive();
        bool retained = interactionPinnedWidgetId_ == widget.id ||
            popupWidgetIndex_ == index ||
            (mouseDown_ && mouseDownWidgetIndex_ == index) ||
            snowdesktop::widget_visibility_rules::ShouldRetainForKeyboardNavigation(
                keyboardNavVisualFocus_, keyboardNavInsideWidget_,
                keyboardNavWidgetIndex_, index);
        if (wasExpanded && !retained)
        {
            for (const auto& container : containers_)
            {
                const auto* searchable = dynamic_cast<ScrollingItemWidget*>(container.get());
                if (searchable && searchable->GetWidgetData() == &widget &&
                    searchable->IsSearchFocused())
                {
                    retained = true;
                    break;
                }
            }
        }
        const bool expand = snowdesktop::storage_title_bar::ExpandOnHover(eligible, wasExpanded,
            suppressed, pointerAvailable && inFrame, widget.selected,
            HasSelectedFilesInWidget(index), retained);
        if (expand == wasExpanded) continue;
        changed = true;

        auto& expansion = widgetHoverExpansion_[widget.id];
        if (hwnd_ && generalSettings_.widgetHoverExpandEffect == motion::Roll &&
            CanFoldWidget(widget) && motion::RuntimeAnimationsEnabled())
        {
            expansion.Configure(false, motion::RuntimeDurationScale(), true);
            const auto tick = static_cast<std::uint64_t>(
                snowdesktop::UiAnimationScheduler::MonotonicMilliseconds());
            if (expand) expansion.Open(tick);
            else expansion.Close(tick);
        }
        else if (expand) expansion.ShowImmediately();
        else expansion.ResetHidden();
    }
    if (!suppressionStillInside) hoverExpansionSuppressedWidgetId_.clear();
    changed |= std::erase_if(widgetHoverExpansion_, [this](const auto& entry) {
        return entry.second.IsHidden() || FindWidgetIndexById(entry.first) >= widgets_.size();
    }) != 0;
    if (!changed) return;

    if (!widgetHoverExpansionFrameToken_ &&
        std::ranges::any_of(widgetHoverExpansion_, [](const auto& entry) {
            return entry.second.IsAnimating();
        }))
    {
        widgetHoverExpansionFrameToken_ = uiAnimationScheduler_.StartAnimation(
            snowdesktop::UiAnimationSurface::Desktop,
            [this](double now) { return AdvanceWidgetHoverExpansion(now); });
        if (!widgetHoverExpansionFrameToken_)
            AdvanceWidgetHoverExpansion(0.0, true);
    }
    InvalidateDragStaticScene();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DesktopApp::AdvanceWidgetHoverExpansion(double nowMilliseconds, bool finish)
{
    namespace layers = snowdesktop::widget_composition_layer_rules;
    const bool animate = !finish &&
        generalSettings_.widgetHoverExpandEffect == snowdesktop::animation::Roll &&
        snowdesktop::animation::RuntimeAnimationsEnabled();
    const auto tick = static_cast<std::uint64_t>(nowMilliseconds);
    const bool canPresentChild = animate && !desktopBackdropFullCollectionPending_ &&
        !widgetGroupTransition_.ShouldDeferPaint() &&
        !layers::ShouldDeferWidgetSurfaceDraw(compositionPaintInProgress_,
            IsAnyPersistentDockHostPainting(), floatingPopupCompositionPaintInProgress_,
            desktopWidgetPointerBatchActive_);
    bool active = false;
    bool redraw = false;
    for (auto& [id, expansion] : widgetHoverExpansion_)
    {
        if (!expansion.IsAnimating()) continue;
        const auto index = FindWidgetIndexById(id);
        if (index >= widgets_.size())
        {
            expansion.ResetHidden();
            continue;
        }

        const auto& widget = widgets_[index];
        if (animate && widget.titleBarExpandOnHover && CanFoldWidget(widget)) expansion.Advance(tick);
        else if (expansion.IsInteractive()) expansion.ShowImmediately();
        else expansion.ResetHidden();
        active |= expansion.IsAnimating();

        // Intermediate heights only change the widget child and its glass.
        // Keep desktop paint for first/final visibility and recovery.
        if (canPresentChild && expansion.IsAnimating())
        {
            const auto child = desktopWidgetCompositionItems_.find(id);
            if (child != desktopWidgetCompositionItems_.end() &&
                child->second.visible && child->second.surface)
            {
                const RECT bounds = GetStandaloneWidgetFrameRect(widget);
                if (EqualRect(&child->second.bounds, &bounds) ||
                    SyncDesktopWidgetOcclusion(widget, bounds) ||
                    QueueDesktopWidgetComposition(id))
                    continue;
            }
        }

        RECT dirty = GetExpandedWidgetFrameRect(widget);
        if (IsRectEmpty(&dirty)) continue;
        InflateRect(&dirty, layers::kWidgetSurfaceBorderOverdraw,
            layers::kWidgetSurfaceBorderOverdraw);
        InvalidateRect(hwnd_, &dirty, FALSE);
        redraw = true;
    }
    std::erase_if(widgetHoverExpansion_, [](const auto& entry) { return entry.second.IsHidden(); });
    if (redraw && !finish)
        uiAnimationScheduler_.RequestFramePresentation(&widgetHoverExpansion_, [this] {
            if (hwnd_ && !compositionPaintInProgress_ && !desktopWidgetPointerBatchActive_)
                UpdateWindow(hwnd_);
        });
    if (!active) widgetHoverExpansionFrameToken_ = 0;
    return active;
}

RECT DesktopApp::GetExpandedWidgetFrameRect(const DesktopWidget& widget) const
{
    RECT frame = widget.bounds;
    for (const auto& page : gridPages_)
    {
        if (page.id != widget.gridCell.pageId) continue;
        const float pageVisualScale =
            GetPageItemVisualMetrics(page).layoutScale;
        const int outsetX = snowdesktop::layout_spacing_rules::
            ComponentFrameOutset(
                page.gapX, pageVisualScale, iconSpacingScale_);
        const int outsetY = snowdesktop::layout_spacing_rules::
            ComponentFrameOutset(
                page.gapY, pageVisualScale, iconSpacingScale_);
        InflateRect(&frame, outsetX, outsetY);
        break;
    }
    return frame;
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
    const LONG contentBottom = snowdesktop::widget_chrome_rules::
        HostActionContentBottom(
            widget.showTitle, frame.top, frame.bottom,
            moveHandle.top, ScaleWidgetCu(5.0f, cellScale));
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
    const auto& personalization = CurrentPersonalization();
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
 * @brief 获取无底栏独立窗口小部件的紧凑移动手柄矩形
 * @param widget 桌面小部件引用
 * @return 左下角移动手柄矩形
 */
RECT DesktopApp::GetStandaloneWidgetCompactMoveHandleRect(
    const DesktopWidget& widget) const
{
    RECT handle = GetStandaloneWidgetMoveHandleRect(widget);
    const int preferredWidth = ScaleWidgetCu(
        CurrentPersonalization().barHeight,
        GetWidgetCellScale(widget));
    const int handleWidth = snowdesktop::widget_chrome_rules::
        CompactEdgeHandleWidth(
            handle.right - handle.left, preferredWidth);
    return {
        handle.left,
        handle.top,
        handle.left + handleWidth,
        handle.bottom
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
    const float barHeight = CurrentPersonalization().barHeight;
    const int preferredWidth = ScaleWidgetCu(
        barHeight, GetWidgetCellScale(widget));
    const auto* content = ResolveLuaWidgetContent(&widget);
    const int handleWidth = (content ? content->showTitle : widget.showTitle)
        ? preferredWidth
        : snowdesktop::widget_chrome_rules::CompactEdgeHandleWidth(
            handle.right - handle.left, preferredWidth);
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
    if (!IsLuaDesktopSurface(widget) || IsGroupedWidget(widget)) return WidgetHit::None;

    RECT frame = GetStandaloneWidgetFrameRect(widget);
    if (!PtInRect(&frame, pt)) return WidgetHit::None;
    RECT resize = GetStandaloneWidgetResizeHandleRect(widget);
    if (PtInRect(&resize, pt) && (widget.type != DesktopWidgetType::LuaStack ||
        widget.minGridSpan.columns != widget.maxGridSpan.columns || widget.minGridSpan.rows != widget.maxGridSpan.rows))
        return WidgetHit::ResizeHandle;
    const auto* content = ResolveLuaWidgetContent(&widget);
    if (!snowdesktop::widget_chrome_rules::HasBottomBar(content ? content->showTitle : widget.showTitle))
    {
        RECT move = GetStandaloneWidgetCompactMoveHandleRect(widget);
        if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;
        return WidgetHit::Content;
    }
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
        const auto& widget = widgets_[i];
        if (IsGroupedWidget(widget) || (desktopIconsHidden_ && !widget.keepWhenDesktopHidden)) continue;
        if (!IsLuaDesktopSurface(widget))
        {
            RECT frame = widget.bounds;
            for (const auto& container : containers_)
            {
                const auto* native = dynamic_cast<const WidgetContainer*>(container.get());
                if (native && native->GetWidgetData() == &widget)
                {
                    frame = native->GetFrameRect();
                    break;
                }
            }
            // A foreground native card also blocks Lua hover/cursor hits.
            if (!IsRectEmpty(&frame) && PtInRect(&frame, pt)) return static_cast<size_t>(-1);
            continue;
        }
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
