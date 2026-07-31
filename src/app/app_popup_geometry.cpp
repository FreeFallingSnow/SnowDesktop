#include "app.h"

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

    RECT work = page ? page->workArea : layoutWorkArea_;
    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int cellW = GetCollectionPopupCellWidth();
    const int cellH = GetCollectionPopupCellHeight();
    const int availableWidth = std::max(1, workWidth - 24);
    const int maxWidth = std::min(560, availableWidth);
    const int popupContentWidth = std::max(1, maxWidth - kCollectionPopupPaddingX * 2);
    const int maxColumns = std::max(1,
        (popupContentWidth + kCollectionPopupGapX) /
        std::max(1, cellW + kCollectionPopupGapX));
    const size_t itemCount =
        GetPopupItemCount(widget);
    int columns =
        snowdesktop::collection_popup_layout::
            PreferredColumnCount(
                itemCount, maxColumns);
    int rows =
        snowdesktop::collection_popup_layout::
            RequiredRowCount(
                itemCount, columns);
    const int maxHeight = std::max(1, workHeight - 24);
    auto popupWidthForColumns = [&](int columnCount) {
        return kCollectionPopupPaddingX * 2 + columnCount * cellW +
            std::max(0, columnCount - 1) * kCollectionPopupGapX;
    };
    auto popupHeightForRows = [&](int rowCount) {
        return kCollectionPopupHeaderHeight + rowCount * cellH +
            std::max(0, rowCount - 1) * kCollectionPopupGapY +
            kCollectionPopupBottomPadding;
    };
    int width = popupWidthForColumns(columns);
    int height = popupHeightForRows(rows);
    if (itemCount > 0 &&
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
                top = popupAnchorPoint_.y + 12;
                break;
            case DockPosition::Left:
                left = popupAnchorPoint_.x + 12;
                top = popupAnchorPoint_.y - height / 2;
                break;
            case DockPosition::Right:
                left = popupAnchorPoint_.x - width - 12;
                top = popupAnchorPoint_.y - height / 2;
                break;
            case DockPosition::Bottom:
            default:
                left = popupAnchorPoint_.x - width / 2;
                top = popupAnchorPoint_.y - height - 12;
                break;
            }
        }
        else
        {
            left = popupAnchorPoint_.x + 12;
            top = popupAnchorPoint_.y + 12;
        }
        left = std::clamp(left, static_cast<int>(work.left + 12),
            static_cast<int>(std::max<LONG>(work.left + 12, work.right - width - 12)));
        top = std::clamp(top, static_cast<int>(work.top + 12),
            static_cast<int>(std::max<LONG>(work.top + 12, work.bottom - height - 12)));
    }
    return MakeRect(left, top, left + width, top + height);
}

RECT DesktopApp::GetCollectionPopupContentRect(const RECT& popup) const
{
    return MakeRect(
        popup.left + kCollectionPopupPaddingX,
        popup.top + kCollectionPopupHeaderHeight,
        popup.right - kCollectionPopupPaddingX,
        popup.bottom - kCollectionPopupBottomPadding);
}

RECT DesktopApp::GetDockFolderPopupSortButtonRect(
    const RECT& popup) const
{
    const int width = std::min(
        104,
        std::max(72, static_cast<int>(
            popup.right - popup.left) / 3));
    return MakeRect(
        popup.right - 16 - width,
        popup.top + 11,
        popup.right - 16,
        popup.top + 45);
}

int DesktopApp::GetCollectionPopupColumnCount(const RECT& popup) const
{
    RECT content = GetCollectionPopupContentRect(popup);
    const int cellW = GetCollectionPopupCellWidth();
    return std::max(1,
        (static_cast<int>(content.right - content.left) + kCollectionPopupGapX) /
        std::max(1, cellW + kCollectionPopupGapX));
}

int DesktopApp::GetCollectionPopupCellWidth() const
{
    int cellW = kCellWidth;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_)
        {
            cellW = page.cellWidth;
            break;
        }
    }
    return std::clamp(cellW, 64, kCellWidth);
}

int DesktopApp::GetCollectionPopupCellHeight() const
{
    int cellH = kMinCellHeight;
    for (const auto& page : gridPages_)
    {
        if (page.id == popupPageId_)
        {
            cellH = page.cellHeight;
            break;
        }
    }
    return std::clamp(cellH, 84, kMinCellHeight);
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
    RECT content = GetCollectionPopupContentRect(popup);
    const int cellH = GetCollectionPopupCellHeight();
    const int rows = GetCollectionPopupRowCount(widget, popup);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    const int contentHeight = rows * std::max(1, cellH) +
        std::max(0, rows - 1) * kCollectionPopupGapY;
    return std::max(0, contentHeight - visibleHeight);
}

RECT DesktopApp::GetCollectionPopupItemRect(const RECT& popup, size_t linearIndex) const
{
    RECT content = GetCollectionPopupContentRect(popup);
    const int cellW = GetCollectionPopupCellWidth();
    const int cellH = GetCollectionPopupCellHeight();
    const int columns = GetCollectionPopupColumnCount(popup);
    const int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    const int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    return MakeRect(
        content.left + col * (cellW + kCollectionPopupGapX),
        content.top + row * (cellH + kCollectionPopupGapY) - popupScrollOffset_,
        content.left + col * (cellW + kCollectionPopupGapX) + cellW,
        content.top + row * (cellH + kCollectionPopupGapY) - popupScrollOffset_ + cellH);
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

void DesktopApp::ResetCollectionPopupAnimationCache()
{
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

    // The off-screen draw switches the shared brush cache to its context.
    // Restore lazy creation for the next desktop/floating-Dock frame.
    brushCache_.clear();
    brushCacheContext_ = nullptr;
}
