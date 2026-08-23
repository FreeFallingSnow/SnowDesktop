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
    return GetPopupItemKeys(widget).size();
}

RECT DesktopApp::GetCollectionPopupRect(const DesktopWidget& widget) const
{
    const GridPage* page = ResolveCollectionPopupPage(widget);
    const auto metrics = GetCollectionPopupLayoutMetrics(widget);

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
    const bool listMode = widget.listMode;
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
    const int maxHeight = std::max(
        1, workHeight - metrics.edgeMargin * 2);
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
                        widget.listMode,
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
    int top = popup.top + metrics.headerHeight;
    if (const DesktopWidget* widget = GetOpenPopupWidget();
        widget &&
        snowdesktop::collection_popup_layout::DetailsVisible(
            widget->listMode,
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
        !snowdesktop::collection_popup_layout::DetailsVisible(
            widget->listMode,
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
        widget && widget->listMode)
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
    if (widget.listMode)
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
    if (const DesktopWidget* widget = GetOpenPopupWidget();
        widget && widget->listMode)
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

RECT DesktopApp::GetCollectionPopupItemIconRect(
    const RECT& itemRect) const
{
    const DesktopWidget* widget = GetOpenPopupWidget();
    if (!widget || !widget->listMode)
        return GetItemIconRect(itemRect);

    RECT nameCell = itemRect;
    if (snowdesktop::collection_popup_layout::DetailsVisible(
            widget->listMode,
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
    if (!widget || !widget->listMode)
        return GetItemTextRect(itemRect, true);

    RECT nameCell = itemRect;
    if (snowdesktop::collection_popup_layout::DetailsVisible(
            widget->listMode,
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
    if (!d2dDevice_ || !openWidget)
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
