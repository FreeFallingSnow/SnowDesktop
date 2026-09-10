#include "app.h"
#include "../widget_item_layout.h"

// Collection-popup model lookup, selection adapter, geometry and animation-cache preparation.

std::vector<Item*> DesktopApp::GetDockFolderPopupSelectedItems()
{
    dockFolderPopupDragItems_.clear();
    std::vector<Item*> selected;
    if (!dockFolderPopupOpen_ || !dockFolderPopupContainer_)
        return selected;

    const RECT popup = GetCollectionPopupRect(dockFolderPopupWidget_);
    for (size_t i = 0; i < dockFolderPopupWidget_.folderEntries.size(); ++i)
    {
        FolderEntry& entry = dockFolderPopupWidget_.folderEntries[i];
        if (!entry.selected)
            continue;

        auto item = std::make_unique<FolderEntryIcon>(
            &entry, dockFolderPopupContainer_.get(), this);
        item->SetBounds(GetCollectionPopupItemRect(popup, i));
        selected.push_back(item.get());
        dockFolderPopupDragItems_.push_back(std::move(item));
    }
    return selected;
}

std::vector<std::wstring> DesktopApp::GetPopupItemKeys(const DesktopWidget& widget) const
{
    if (widget.type == DesktopWidgetType::Collection)
        return widget.itemKeys;
    return {};
}

DesktopWidget* DesktopApp::GetOpenPopupWidget()
{
    if (dockFolderPopupOpen_)
        return &dockFolderPopupWidget_;
    return popupWidgetIndex_ < widgets_.size()
        ? &widgets_[popupWidgetIndex_] : nullptr;
}

const DesktopWidget* DesktopApp::GetOpenPopupWidget() const
{
    if (dockFolderPopupOpen_)
        return &dockFolderPopupWidget_;
    return popupWidgetIndex_ < widgets_.size()
        ? &widgets_[popupWidgetIndex_] : nullptr;
}

size_t DesktopApp::GetPopupItemCount(
    const DesktopWidget& widget) const
{
    if (widget.type == DesktopWidgetType::FolderMapping)
        return widget.folderEntries.size();
    return widget.type == DesktopWidgetType::Collection ? widget.itemKeys.size() : 0;
}

bool DesktopApp::CollectionPopupFanRootAbove() const
{
    if (popupAnchoredToDock_)
        return popupDockPosition_ == DockPosition::Top;
    const auto* widget = GetOpenPopupWidget();
    const auto* page = widget ? ResolveCollectionPopupPage(*widget) : nullptr;
    const RECT work = page ? page->workArea : layoutWorkArea_;
    return popupHasAnchor_ && popupAnchorPoint_.y < (work.top + work.bottom) / 2;
}

RECT DesktopApp::GetCollectionPopupFanWorkArea(const DesktopWidget& widget) const
{
    const auto* page = ResolveCollectionPopupPage(widget);
    RECT work = page ? page->workArea : layoutWorkArea_;
    const auto metrics = GetCollectionPopupLayoutMetrics(widget);
    InflateRect(&work, -metrics.edgeMargin, -metrics.edgeMargin);
    if (popupHasAnchor_)
    {
        if (CollectionPopupFanRootAbove())
            work.top = std::max(work.top, popupAnchorPoint_.y + metrics.anchorGap);
        else
            work.bottom = std::min(work.bottom, popupAnchorPoint_.y - metrics.anchorGap);
    }
    return work;
}

bool DesktopApp::UsesCollectionPopupFan(const DesktopWidget& widget) const
{
    namespace layout = snowdesktop::collection_popup_layout;
    if (!widget.fanPopup || popupFanShowAll_ || GetPopupItemCount(widget) == 0)
        return false;
    // A vertical side Dock has no native fan counterpart. Use the existing
    // grid there, and when there is insufficient space for an anchored fan.
    if (popupAnchoredToDock_ &&
        (popupDockPosition_ == DockPosition::Left || popupDockPosition_ == DockPosition::Right))
        return false;
    const auto metrics = GetCollectionPopupLayoutMetrics(widget);
    const auto work = GetCollectionPopupFanWorkArea(widget);
    return work.right - work.left >= layout::ScaleDimension(400, metrics.scale) &&
        work.bottom - work.top >= layout::FanFrameHeight(metrics, 2);
}

bool DesktopApp::UsesCollectionPopupList(const DesktopWidget& widget) const
{
    return widget.listMode && !widget.fanPopup;
}

size_t DesktopApp::GetCollectionPopupFanVisibleCount(const RECT& popup) const
{
    const auto* widget = GetOpenPopupWidget();
    return widget ? snowdesktop::collection_popup_layout::FanVisibleItemCount(
        GetCollectionPopupLayoutMetrics(*widget), popup.bottom - popup.top,
        GetPopupItemCount(*widget)) : 0;
}

std::wstring DesktopApp::GetCollectionPopupFanLabel(size_t index) const
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || index >= GetPopupItemCount(*widget))
        return _LW("app.interact.popup_show_all");
    if (widget->type == DesktopWidgetType::FolderMapping)
        return widget->folderEntries[index].name;
    const auto itemIndex = FindItemIndexByKey(widget->itemKeys[index]);
    if (itemIndex >= items_.size()) return {};
    const auto& item = items_[itemIndex];
    return ShouldUseDemoCollectionIdentity(widget)
        ? GetDemoCollectionIdentityTitle(*widget,
            item.layoutKey.empty() ? item.parsingName : item.layoutKey)
        : item.name;
}

snowdesktop::collection_popup_layout::FanItem
DesktopApp::GetCollectionPopupFanItem(const RECT& popup, size_t index) const
{
    namespace layout = snowdesktop::collection_popup_layout;
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    const bool mirrored = popupHasAnchor_ &&
        popupAnchorPoint_.x < (popup.left + popup.right) / 2;
    const auto* widget = GetOpenPopupWidget();
    const double position = widget && index >= GetPopupItemCount(*widget)
        ? static_cast<double>(GetCollectionPopupFanVisibleCount(popup))
        : static_cast<double>(index) - GetCollectionPopupFanScrollOffset(popup);
    auto result = layout::ResolveFanItem(metrics, popup, position,
        CollectionPopupFanRootAbove(), mirrored);
    const std::wstring text = GetCollectionPopupFanLabel(index);
    ComPtr<IDWriteTextLayout> textLayout;
    if (dwriteFactory_ && itemTextFormat_ && SUCCEEDED(dwriteFactory_->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()), itemTextFormat_.Get(),
            10000.0f, 1000.0f, &textLayout)))
    {
        textLayout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS textMetrics{};
        if (SUCCEEDED(textLayout->GetMetrics(&textMetrics)))
        {
            const int width = std::clamp(static_cast<int>(std::ceil(textMetrics.width)) +
                layout::ScaleDimension(16, metrics.scale),
                layout::ScaleDimension(36, metrics.scale),
                static_cast<int>(result.label.right - result.label.left));
            if (mirrored) result.label.right = result.label.left + width;
            else result.label.left = result.label.right - width;
        }
    }
    return result;
}

bool DesktopApp::HitTestCollectionPopupItem(const RECT& popup, size_t index, POINT point) const
{
    const auto* widget = GetOpenPopupWidget();
    if (widget && UsesCollectionPopupFan(*widget))
        return IsCollectionPopupFanItemVisible(popup, index) &&
            snowdesktop::collection_popup_layout::FanItemContains(
                GetCollectionPopupFanItem(popup, index), point);
    const RECT item = GetCollectionPopupItemRect(popup, index);
    const RECT content = GetCollectionPopupContentRect(popup);
    return PtInRect(&item, point) && PtInRect(&content, point);
}

double DesktopApp::GetCollectionPopupFanScrollOffset(const RECT& popup) const
{
    const auto* widget = GetOpenPopupWidget();
    return widget ? std::clamp(popupFanScroll_.position, 0.0,
        snowdesktop::collection_popup_layout::FanMaximumScroll(
            GetPopupItemCount(*widget), GetCollectionPopupFanVisibleCount(popup))) : 0.0;
}

bool DesktopApp::IsCollectionPopupFanItemVisible(const RECT& popup, size_t index) const
{
    const auto* widget = GetOpenPopupWidget();
    return widget && index < GetPopupItemCount(*widget) &&
        snowdesktop::collection_popup_layout::FanItemOpacity(
            static_cast<double>(index) - GetCollectionPopupFanScrollOffset(popup),
            GetCollectionPopupFanVisibleCount(popup)) > 0.001f;
}

void DesktopApp::ResetCollectionPopupFanScroll()
{
    if (popupFanScrollFrameToken_) uiAnimationScheduler_.Cancel(popupFanScrollFrameToken_);
    popupFanScrollFrameToken_ = 0;
    popupFanScroll_ = {};
}

void DesktopApp::ScrollCollectionPopupFan(double amount)
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !UsesCollectionPopupFan(*widget)) return;
    const auto maximum = snowdesktop::collection_popup_layout::FanMaximumScroll(
        GetPopupItemCount(*widget), GetCollectionPopupFanVisibleCount(popupRect_));
    popupFanScroll_.MoveTo(popupFanScroll_.target + amount, maximum,
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
        snowdesktop::animation::RuntimeAnimationsEnabled() ? 130.0 : 0.0);
    popupFanActionFocused_ = false;
    EnsureUiAnimationFrame();
    InvalidateCollectionPopupAnimation(true);
}

void DesktopApp::EnsureCollectionPopupFanItemVisible(size_t index)
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !UsesCollectionPopupFan(*widget) || index >= GetPopupItemCount(*widget)) return;
    const size_t visible = GetCollectionPopupFanVisibleCount(popupRect_);
    const double current = GetCollectionPopupFanScrollOffset(popupRect_);
    const double position = static_cast<double>(index);
    double target = current;
    if (position < current) target = position;
    else if (position > current + static_cast<double>(visible) - 1)
        target = position - static_cast<double>(visible) + 1;
    popupFanScroll_.MoveTo(target, snowdesktop::collection_popup_layout::FanMaximumScroll(
        GetPopupItemCount(*widget), visible),
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(), 0);
}

void DesktopApp::ShowAllCollectionPopupItems()
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !UsesCollectionPopupFan(*widget)) return;
    // This is a change of the same popup, not an outside click. Retire queued
    // hook notifications against the old fan before its hit region shrinks.
    AdvanceFloatingPopupContentGeneration();
    if (handlingFloatingPopupInput_)
    {
        // The Dock's timer also samples physical presses. Consume this handled
        // press before publishing the grid, including the since-last-read bit.
        const SHORT state = GetAsyncKeyState(VK_LBUTTON);
        constexpr UINT leftButtonBit = 1u << 0;
        floatingDockPointerButtonsDown_ =
            (floatingDockPointerButtonsDown_ & ~leftButtonBit) |
            ((state & 0x8000) ? leftButtonBit : 0);
    }
    ResetCollectionPopupAnimationCache();
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = true;
    popupFanActionFocused_ = false;
    popupScrollOffset_ = 0;
    popupScrollbarDragging_ = false;
    popupAnimation_.Configure(
        snowdesktop::animation::RuntimePopupEffect() == snowdesktop::animation::Fade,
        snowdesktop::animation::RuntimeDurationScale());
    popupAnimation_.ShowImmediately();
    ClearPopupDragTarget();
    RefreshOpenCollectionPopupGeometry();
}

RECT DesktopApp::GetCollectionPopupRect(const DesktopWidget& widget) const
{
    const GridPage* page = ResolveCollectionPopupPage(widget);
    const auto metrics = GetCollectionPopupLayoutMetrics(widget);
    if (UsesCollectionPopupFan(widget))
    {
        namespace layout = snowdesktop::collection_popup_layout;
        const RECT available = GetCollectionPopupFanWorkArea(widget);
        const int width = layout::ScaleDimension(400, metrics.scale);
        const int maximum = std::min(metrics.maximumHeight,
            static_cast<int>(available.bottom - available.top));
        const auto visible = layout::FanVisibleItemCount(metrics, maximum, GetPopupItemCount(widget));
        const int height = layout::FanFrameHeight(metrics, visible + 1);
        const int anchorX = popupHasAnchor_ ? popupAnchorPoint_.x : (available.left + available.right) / 2;
        const bool mirrored = anchorX - layout::FanRootX(metrics, width, false) < available.left;
        const int left = std::clamp(anchorX - layout::FanRootX(metrics, width, mirrored),
            static_cast<int>(available.left), static_cast<int>(available.right) - width);
        const int top = CollectionPopupFanRootAbove() ? available.top : available.bottom - height;
        return MakeRect(left, top, left + width, top + height);
    }

    RECT work = page ? page->workArea : layoutWorkArea_;
    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int cellW = metrics.cellWidth;
    const int cellH = metrics.cellHeight;
    const int availableWidth = std::max(
        1, workWidth - metrics.edgeMargin * 2);
    const int maxWidth = std::min(
        metrics.maximumWidth, availableWidth);
    const int popupContentWidth = std::max(
        1, maxWidth - metrics.paddingX * 2);
    const int maxColumns = std::max(1,
        (popupContentWidth + metrics.gapX) /
        std::max(1, cellW + metrics.gapX));
    const size_t itemCount =
        GetPopupItemCount(widget);
    const bool listMode = UsesCollectionPopupList(widget);
    int columns = listMode
        ? 1
        : snowdesktop::collection_popup_layout::
            PreferredColumnCount(
                itemCount, maxColumns);
    int rows =
        listMode
        ? snowdesktop::collection_popup_layout::
            RequiredListRowCount(itemCount)
        : snowdesktop::collection_popup_layout::
            RequiredRowCount(itemCount, columns);
    const int maxHeight =
        snowdesktop::collection_popup_layout::
            ResolveMaximumHeight(
                metrics, workHeight);
    auto popupWidthForColumns = [&](int columnCount) {
        if (listMode)
            return maxWidth;
        return metrics.paddingX * 2 + columnCount * cellW +
            std::max(0, columnCount - 1) * metrics.gapX;
    };
    auto popupHeightForRows = [&](int rowCount) {
        if (listMode)
        {
            const RECT viewport{
                0, 0, std::max(1, maxWidth - metrics.paddingX * 2),
                std::numeric_limits<LONG>::max() / 4 };
            const auto layout = snowdesktop::widget_item_layout::
                ResolveList(
                    viewport,
                    snowdesktop::collection_popup_layout::
                        ResolveListRowHeight(
                            metrics, listItemFontSizeCu_),
                    GetLayoutSpacingScale());
            const int detailsHeader =
                snowdesktop::collection_popup_layout::
                    DetailsVisible(
                        UsesCollectionPopupList(widget),
                        widget.detailShowModified,
                        widget.detailShowType,
                        widget.detailShowSize)
                ? snowdesktop::collection_popup_layout::
                    ResolveDetailsHeaderHeight(metrics)
                : 0;
            return metrics.headerHeight + detailsHeader +
                snowdesktop::widget_item_layout::ContentHeight(
                    layout, static_cast<size_t>(rowCount)) +
                metrics.bottomPadding;
        }
        return metrics.headerHeight + rowCount * cellH +
            std::max(0, rowCount - 1) * metrics.gapY +
            metrics.bottomPadding;
    };
    int width = popupWidthForColumns(columns);
    int height = popupHeightForRows(rows);
    if (!listMode && itemCount > 0 &&
        height > maxHeight &&
        columns < maxColumns)
    {
        columns = maxColumns;
        rows =
            snowdesktop::collection_popup_layout::
                RequiredRowCount(
                    itemCount, columns);
        width = popupWidthForColumns(columns);
        height = popupHeightForRows(rows);
    }
    width = std::min(width, availableWidth);
    height = std::min(height, maxHeight);

    int left = work.left + (workWidth - width) / 2;
    int top = work.top + (workHeight - height) / 2;
    if (popupHasAnchor_)
    {
        if (popupAnchoredToDock_)
        {
            switch (popupDockPosition_)
            {
            case DockPosition::Top:
                left = popupAnchorPoint_.x - width / 2;
                top = popupAnchorPoint_.y + metrics.anchorGap;
                break;
            case DockPosition::Left:
                left = popupAnchorPoint_.x + metrics.anchorGap;
                top = popupAnchorPoint_.y - height / 2;
                break;
            case DockPosition::Right:
                left = popupAnchorPoint_.x - width - metrics.anchorGap;
                top = popupAnchorPoint_.y - height / 2;
                break;
            case DockPosition::Bottom:
            default:
                left = popupAnchorPoint_.x - width / 2;
                top = popupAnchorPoint_.y - height - metrics.anchorGap;
                break;
            }
        }
        else
        {
            left = popupAnchorPoint_.x + metrics.anchorGap;
            top = popupAnchorPoint_.y + metrics.anchorGap;
        }
        left = std::clamp(
            left,
            static_cast<int>(work.left + metrics.edgeMargin),
            static_cast<int>(std::max<LONG>(
                work.left + metrics.edgeMargin,
                work.right - width - metrics.edgeMargin)));
        top = std::clamp(
            top,
            static_cast<int>(work.top + metrics.edgeMargin),
            static_cast<int>(std::max<LONG>(
                work.top + metrics.edgeMargin,
                work.bottom - height - metrics.edgeMargin)));
    }
    return MakeRect(left, top, left + width, top + height);
}

const GridPage* DesktopApp::ResolveCollectionPopupPage(
    const DesktopWidget& widget) const
{
    const std::wstring& targetPageId = popupPageId_.empty()
        ? widget.gridCell.pageId : popupPageId_;
    const GridPage* page = nullptr;
    for (const auto& p : gridPages_)
    {
        if (p.id == targetPageId)
        {
            page = &p;
            break;
        }
    }
    if (!page && popupHasAnchor_)
    {
        for (const auto& p : gridPages_)
        {
            if (PtInRect(&p.bounds, popupAnchorPoint_))
            {
                page = &p;
                break;
            }
        }
    }
    if (!page && !gridPages_.empty())
        page = &gridPages_.front();
    return page;
}

snowdesktop::collection_popup_layout::Metrics
DesktopApp::GetCollectionPopupLayoutMetrics(
    const DesktopWidget& widget) const
{
    if (const GridPage* page =
            ResolveCollectionPopupPage(widget))
    {
        const auto visualMetrics =
            GetPageItemVisualMetrics(*page);
        return snowdesktop::collection_popup_layout::
            ResolveMetrics(
                page->cellWidth,
                page->cellHeight,
                visualMetrics.minimumGridWidth,
                visualMetrics.minimumGridHeight,
                visualMetrics.layoutScale,
                visualMetrics.minimumListHeight);
    }
    return snowdesktop::collection_popup_layout::
        ResolveMetrics(
            kCellWidth,
            kMinCellHeight,
            kCellWidth,
            kMinCellHeight,
            1.0f);
}

snowdesktop::collection_popup_layout::Metrics
DesktopApp::GetOpenCollectionPopupLayoutMetrics() const
{
    if (const DesktopWidget* widget = GetOpenPopupWidget())
        return GetCollectionPopupLayoutMetrics(*widget);
    return snowdesktop::collection_popup_layout::
        ResolveMetrics(
            kCellWidth,
            kMinCellHeight,
            kCellWidth,
            kMinCellHeight,
            1.0f);
}

RECT DesktopApp::GetCollectionPopupContentRect(const RECT& popup) const
{
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    if (const auto* widget = GetOpenPopupWidget(); widget && UsesCollectionPopupFan(*widget))
        return popup;
    int top = popup.top + metrics.headerHeight;
    if (const DesktopWidget* widget = GetOpenPopupWidget();
        widget &&
        !UsesCollectionPopupFan(*widget) &&
        snowdesktop::collection_popup_layout::DetailsVisible(
            UsesCollectionPopupList(*widget),
            widget->detailShowModified,
            widget->detailShowType,
            widget->detailShowSize))
    {
        top += snowdesktop::collection_popup_layout::
            ResolveDetailsHeaderHeight(metrics);
    }
    return MakeRect(
        popup.left + metrics.paddingX,
        top,
        popup.right - metrics.paddingX,
        popup.bottom - metrics.bottomPadding);
}

RECT DesktopApp::GetCollectionPopupDetailsHeaderRect(
    const RECT& popup) const
{
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (!widget ||
        UsesCollectionPopupFan(*widget) ||
        !snowdesktop::collection_popup_layout::DetailsVisible(
            UsesCollectionPopupList(*widget),
            widget->detailShowModified,
            widget->detailShowType,
            widget->detailShowSize))
    {
        return {};
    }

    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    const RECT content = GetCollectionPopupContentRect(popup);
    return MakeRect(
        content.left,
        popup.top + metrics.headerHeight,
        content.right,
        content.top);
}

snowdesktop::list_detail_rules::Column
DesktopApp::HitTestCollectionPopupDetailsDivider(
    POINT point, const RECT& popup) const
{
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (!widget) return snowdesktop::list_detail_rules::Column::None;

    const RECT header = GetCollectionPopupDetailsHeaderRect(popup);
    if (IsRectEmptyRect(header) || !PtInRect(&header, point))
        return snowdesktop::list_detail_rules::Column::None;

    const int width = std::max<int>(1, header.right - header.left);
    const auto columns = snowdesktop::list_detail_rules::BuildColumns(
        width,
        widget->detailShowModified,
        widget->detailShowType,
        widget->detailShowSize,
        widget->detailModifiedPosition,
        widget->detailTypePosition,
        widget->detailSizePosition);
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    return snowdesktop::list_detail_rules::HitDivider(
        columns,
        point.x - header.left,
        snowdesktop::collection_popup_layout::ScaleDimension(
            4, metrics.scale));
}

RECT DesktopApp::GetDockFolderPopupSortButtonRect(
    const RECT& popup) const
{
    if (const auto* widget = GetOpenPopupWidget(); widget && UsesCollectionPopupFan(*widget))
        return {};
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    const auto headerBounds =
        snowdesktop::collection_popup_layout::
            ResolveHeaderVerticalBounds(metrics.scale);
    const int width = std::min(
        snowdesktop::collection_popup_layout::ScaleDimension(
            104, metrics.scale),
        std::max(
            snowdesktop::collection_popup_layout::ScaleDimension(
                72, metrics.scale),
            static_cast<int>(
            popup.right - popup.left) / 3));
    return MakeRect(
        popup.right - snowdesktop::collection_popup_layout::
            ScaleDimension(16, metrics.scale) - width,
        popup.top + headerBounds.sortButtonTop,
        popup.right - snowdesktop::collection_popup_layout::
            ScaleDimension(16, metrics.scale),
        popup.top + headerBounds.sortButtonBottom);
}

int DesktopApp::GetCollectionPopupColumnCount(const RECT& popup) const
{
    if (const DesktopWidget* widget = GetOpenPopupWidget();
        widget && (UsesCollectionPopupFan(*widget) || UsesCollectionPopupList(*widget)))
        return 1;
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    RECT content = GetCollectionPopupContentRect(popup);
    const int cellW = metrics.cellWidth;
    return std::max(1,
        (static_cast<int>(content.right - content.left) + metrics.gapX) /
        std::max(1, cellW + metrics.gapX));
}

int DesktopApp::GetCollectionPopupRowCount(const DesktopWidget& widget, const RECT& popup) const
{
    const int columns = GetCollectionPopupColumnCount(popup);
    const int itemCount = std::max(
        1, static_cast<int>(GetPopupItemCount(widget)));
    return (itemCount + columns - 1) / columns;
}

int DesktopApp::GetCollectionPopupMaxScrollOffset(const DesktopWidget& widget, const RECT& popup) const
{
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    RECT content = GetCollectionPopupContentRect(popup);
    if (UsesCollectionPopupFan(widget)) return 0;
    if (UsesCollectionPopupList(widget))
    {
        const auto layout = snowdesktop::widget_item_layout::
            ResolveList(
                content,
                snowdesktop::collection_popup_layout::
                    ResolveListRowHeight(
                        metrics, listItemFontSizeCu_),
                GetLayoutSpacingScale());
        return std::max(
            0,
            snowdesktop::widget_item_layout::ContentHeight(
                layout, GetPopupItemCount(widget)) -
                std::max(1, static_cast<int>(
                    content.bottom - content.top)));
    }
    const int cellH = metrics.cellHeight;
    const int rows = GetCollectionPopupRowCount(widget, popup);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    const int contentHeight = rows * std::max(1, cellH) +
        std::max(0, rows - 1) * metrics.gapY;
    return std::max(0, contentHeight - visibleHeight);
}

RECT DesktopApp::GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const
{
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    RECT content = GetCollectionPopupContentRect(popup);
    if (const DesktopWidget* widget = GetOpenPopupWidget(); widget && UsesCollectionPopupFan(*widget))
    {
        if (!IsCollectionPopupFanItemVisible(popup, linearIndex)) return {};
        return snowdesktop::collection_popup_layout::FanItemBounds(
            GetCollectionPopupFanItem(popup, linearIndex));
    }
    if (const DesktopWidget* widget = GetOpenPopupWidget();
        widget && UsesCollectionPopupList(*widget))
    {
        const auto layout = snowdesktop::widget_item_layout::
            ResolveList(
                content,
                snowdesktop::collection_popup_layout::
                    ResolveListRowHeight(
                        metrics, listItemFontSizeCu_),
                GetLayoutSpacingScale());
        return snowdesktop::widget_item_layout::ItemRect(
            layout, linearIndex, popupScrollOffset_);
    }
    const int cellW = metrics.cellWidth;
    const int cellH = metrics.cellHeight;
    const int columns = GetCollectionPopupColumnCount(popup);
    const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    return MakeRect(
        content.left + col * (cellW + metrics.gapX),
        content.top + row * (cellH + metrics.gapY) - popupScrollOffset_,
        content.left + col * (cellW + metrics.gapX) + cellW,
        content.top + row * (cellH + metrics.gapY) - popupScrollOffset_ + cellH);
}

RECT DesktopApp::GetCollectionPopupFanDragBounds(const Item* item) const
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !item || !UsesCollectionPopupFan(*widget)) return {};
    const auto* container = dynamic_cast<const WidgetContainer*>(item->GetContainer());
    if (!container || container->GetWidgetData() != widget) return {};
    const auto* desktop = dynamic_cast<const DesktopIcon*>(item);
    const auto* folder = dynamic_cast<const FolderEntryIcon*>(item);
    const RECT popup = GetCollectionPopupRect(*widget);
    for (size_t i = 0; i < GetPopupItemCount(*widget); ++i)
    {
        const bool matches = widget->type == DesktopWidgetType::FolderMapping
            ? folder && folder->GetFolderEntry() == &widget->folderEntries[i]
            : desktop && FindItemIndexByKey(widget->itemKeys[i]) < items_.size() &&
                desktop->GetDesktopItem() == &items_[FindItemIndexByKey(widget->itemKeys[i])];
        if (matches && IsCollectionPopupFanItemVisible(popup, i))
            return GetCollectionPopupFanItem(popup, i).icon;
    }
    return {};
}

bool DesktopApp::HitTestCollectionPopupHandoff(const RECT& popup, size_t index, POINT point) const
{
    const auto* widget = GetOpenPopupWidget();
    if (widget && UsesCollectionPopupFan(*widget))
    {
        if (!IsCollectionPopupFanItemVisible(popup, index)) return false;
        const auto pose = GetCollectionPopupFanItem(popup, index);
        return snowdesktop::collection_popup_layout::FanRectContains(pose,
            snowdesktop::collection_popup_layout::FanHandoffBounds(
                pose, GetOpenCollectionPopupLayoutMetrics()), point);
    }
    const RECT bounds = snowdesktop::popup_drag_rules::HandoffActivationBounds(
        GetCollectionPopupItemIconRect(GetCollectionPopupItemRect(popup, index)));
    return PtInRect(&bounds, point) != FALSE;
}

RECT DesktopApp::GetCollectionPopupItemIconRect(
    const RECT& itemRect) const
{
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (widget && UsesCollectionPopupFan(*widget))
    {
        const RECT popup = GetCollectionPopupRect(*widget);
        const auto range = snowdesktop::collection_popup_layout::FanVisibleRange(
            GetCollectionPopupFanScrollOffset(popup), GetCollectionPopupFanVisibleCount(popup),
            GetPopupItemCount(*widget));
        for (size_t i = range.first; i < range.end; ++i)
        {
            const auto item = GetCollectionPopupFanItem(popup, i);
            const RECT bounds = snowdesktop::collection_popup_layout::FanItemBounds(item);
            if (EqualRect(&bounds, &itemRect))
                return snowdesktop::collection_popup_layout::FanRotatedBounds(item.icon, item);
        }
        return {};
    }
    if (!widget || !UsesCollectionPopupList(*widget))
        return GetItemIconRect(itemRect);

    RECT nameCell = itemRect;
    if (snowdesktop::collection_popup_layout::DetailsVisible(
            UsesCollectionPopupList(*widget),
            widget->detailShowModified,
            widget->detailShowType,
            widget->detailShowSize))
    {
        const auto columns = snowdesktop::list_detail_rules::
            BuildColumns(
                std::max(1, static_cast<int>(
                    itemRect.right - itemRect.left)),
                widget->detailShowModified,
                widget->detailShowType,
                widget->detailShowSize,
                widget->detailModifiedPosition,
                widget->detailTypePosition,
                widget->detailSizePosition);
        nameCell.right = std::min<LONG>(
            nameCell.right,
            nameCell.left + columns.nameWidth);
    }
    const auto metrics = GetItemVisualMetrics(itemRect);
    return snowdesktop::ResolveListItemIconRect(
        nameCell,
        nameCell.left + snowdesktop::collection_popup_layout::
            ScaleDimension(
                4,
                GetOpenCollectionPopupLayoutMetrics().scale),
        metrics);
}

RECT DesktopApp::GetCollectionPopupItemTextRect(
    const RECT& itemRect) const
{
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (widget && UsesCollectionPopupFan(*widget))
    {
        const RECT popup = GetCollectionPopupRect(*widget);
        const auto range = snowdesktop::collection_popup_layout::FanVisibleRange(
            GetCollectionPopupFanScrollOffset(popup), GetCollectionPopupFanVisibleCount(popup),
            GetPopupItemCount(*widget));
        for (size_t i = range.first; i < range.end; ++i)
        {
            const auto item = GetCollectionPopupFanItem(popup, i);
            const RECT bounds = snowdesktop::collection_popup_layout::FanItemBounds(item);
            if (EqualRect(&bounds, &itemRect))
                return snowdesktop::collection_popup_layout::FanRotatedBounds(item.label, item);
        }
        return {};
    }
    if (!widget || !UsesCollectionPopupList(*widget))
        return GetItemTextRect(itemRect, true);

    RECT nameCell = itemRect;
    if (snowdesktop::collection_popup_layout::DetailsVisible(
            UsesCollectionPopupList(*widget),
            widget->detailShowModified,
            widget->detailShowType,
            widget->detailShowSize))
    {
        const auto columns = snowdesktop::list_detail_rules::
            BuildColumns(
                std::max(1, static_cast<int>(
                    itemRect.right - itemRect.left)),
                widget->detailShowModified,
                widget->detailShowType,
                widget->detailShowSize,
                widget->detailModifiedPosition,
                widget->detailTypePosition,
                widget->detailSizePosition);
        nameCell.right = std::min<LONG>(
            nameCell.right,
            nameCell.left + columns.nameWidth);
    }
    const auto popupMetrics =
        GetOpenCollectionPopupLayoutMetrics();
    const RECT iconRect =
        GetCollectionPopupItemIconRect(itemRect);
    return MakeRect(
        iconRect.right + snowdesktop::collection_popup_layout::
            ScaleDimension(6, popupMetrics.scale),
        itemRect.top + snowdesktop::collection_popup_layout::
            ScaleDimension(2, popupMetrics.scale),
        nameCell.right - snowdesktop::collection_popup_layout::
            ScaleDimension(6, popupMetrics.scale),
        itemRect.bottom - snowdesktop::collection_popup_layout::
            ScaleDimension(2, popupMetrics.scale));
}

bool DesktopApp::IsPointInsideOpenPopup(POINT point) const
{
    if (!IsCollectionPopupInteractive())
        return false;
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (!widget) return false;
    RECT popup = GetCollectionPopupRect(*widget);
    return PtInRect(&popup, point) != FALSE;
}

bool DesktopApp::IsPointOccludedByOpenPopup(POINT point) const
{
    if (popupAnimation_.IsHidden())
        return false;
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (!widget) return false;
    RECT popup = GetCollectionPopupRect(*widget);
    return PtInRect(&popup, point) != FALSE;
}

void DesktopApp::ResetCollectionPopupAnimationCache()
{
    if (popupAnimationCompletionToken_)
        uiAnimationScheduler_.Cancel(
            popupAnimationCompletionToken_);
    popupAnimationCompletionToken_ = 0;
    popupAnimationCompositorDriven_ = false;
    ResetCompositionAnimationOverlay(
        popupAnimationOverlay_);
    popupAnimationRenderCache_.Reset();
    popupAnimationCacheRect_ = {};
}

void DesktopApp::PrepareCollectionPopupAnimationCache()
{
    ResetCollectionPopupAnimationCache();
    const DesktopWidget* openWidget =
        GetOpenPopupWidget();
    if (!d2dDevice_ || !openWidget || UsesCollectionPopupFan(*openWidget))
        return;

    popupRect_ = GetCollectionPopupRect(*openWidget);
    popupAnimationCacheRect_ = popupRect_;
    InflateRect(&popupAnimationCacheRect_, 4, 4);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(
            1,
            popupAnimationCacheRect_.right -
                popupAnimationCacheRect_.left));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(
            1,
            popupAnimationCacheRect_.bottom -
                popupAnimationCacheRect_.top));

    const bool ready =
        popupAnimationRenderCache_.Ensure(
            d2dDevice_.Get(),
            D2D1::SizeU(width, height),
            1,
            [&](ID2D1DeviceContext* cacheContext) {
                cacheContext->SetTransform(
                    D2D1::Matrix3x2F::Translation(
                        static_cast<float>(
                            -popupAnimationCacheRect_.left),
                        static_cast<float>(
                            -popupAnimationCacheRect_.top)));
                DrawCollectionPopup(
                    cacheContext, false);
            });
    if (!ready)
        popupAnimationCacheRect_ = {};
    else
    {
        (void)PrepareCompositionAnimationOverlay(
            popupAnimationOverlay_,
            popupAnimationRenderCache_,
            popupAnimationCacheRect_,
            UiCompositionAnimationHost::FloatingPopup);
    }

    // The off-screen draw switches the shared brush cache to its context.
    // Restore lazy creation for the next desktop/floating-Dock frame.
    brushCache_.clear();
    brushCacheContext_ = nullptr;
}
