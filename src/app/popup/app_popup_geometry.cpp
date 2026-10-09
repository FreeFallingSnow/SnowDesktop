#include "app/app.h"
#include "app/dock/dock_folder_popup_read.h"
#include "widget/runtime/widget_item_layout.h"
#include "categorized_popup_scope.h"

// Collection-popup model lookup, selection adapter, geometry and animation-cache preparation.

bool DesktopApp::UsesCategorizedPopupControls(const DesktopWidget& widget) const
{
    if (IsGroupWidgetType(widget.type)) return true;
    const bool dockFolder = dockFolderPopupOpen_ && &widget == &dockFolderPopupWidget_;
    const size_t sourceCount = widget.type == DesktopWidgetType::FolderMapping
        ? widget.folderEntries.size() : widget.itemKeys.size();
    return popupAnchoredToDock_ &&
        (widget.type == DesktopWidgetType::FileCategories || widget.type == DesktopWidgetType::FolderMapping) &&
        (widget.showSearchBox || widget.showFileCategories) &&
        // Match the known-file reservation used by the opening geometry. Use
        // the full source so filtering to no results keeps search accessible.
        snowdesktop::dock_folder_popup_read::HasCategorizedContent(
            sourceCount, dockFolder && dockFolderPopupLoading_,
            dockFolder ? dockFolderPopupKnownItemCount_ : 0) &&
        !UsesCollectionPopupFan(widget);
}

ScrollingItemWidget* DesktopApp::GetCategorizedPopupView() const
{
    if (auto* group = GetGroupPopupView())
    {
        const auto metrics = GetOpenCollectionPopupLayoutMetrics();
        const RECT frame{popupRect_.left, popupRect_.top + metrics.headerHeight,
            popupRect_.right, popupRect_.bottom - metrics.bottomPadding};
        group->SetPopupFrame(&frame);
        return group;
    }
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !UsesCategorizedPopupControls(*widget)) return nullptr;
    if (dockFolderPopupOpen_) return dockFolderPopupContainer_.get();
    for (const auto& container : containers_)
        if (auto* view = dynamic_cast<FileCategories*>(container.get());
            view && view->GetWidgetData() == widget) return view;
    return nullptr;
}

ScrollingItemWidget* DesktopApp::GetGroupPopupView() const
{
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !IsGroupWidgetType(widget->type)) return nullptr;
    for (const auto& container : containers_)
        if (auto* view = dynamic_cast<ScrollingItemWidget*>(container.get());
            view && view->GetWidgetData() == widget) return view;
    return nullptr;
}

size_t DesktopApp::GetPopupFolderEntryIndex(const DesktopWidget& widget, size_t visibleIndex) const
{
    auto* view = &widget == GetOpenPopupWidget() ?
        dynamic_cast<FolderMapping*>(GetCategorizedPopupView()) : nullptr;
    if (!view) return visibleIndex;
    const auto& visible = view->GetVisibleEntryIndices();
    return visibleIndex < visible.size() ? visible[visibleIndex] : widget.folderEntries.size();
}

RECT DesktopApp::GetCollectionPopupControlsRect(const RECT& popup) const
{
    auto* view = GetCategorizedPopupView();
    if (!view) return {};
    const RECT frame = GetCategorizedPopupFrame(popup);
    if (IsGroupWidgetType(view->GetWidgetData()->type))
    {
        const auto* data = view->GetWidgetData();
        LONG bottom = frame.top + view->Cu(8.0f);
        if (!data->childWidgetIds.empty()) bottom += view->Cu(view->GetCategorizedTabRowPitch());
        if (data->showSearchBox) bottom += view->Cu(view->GetCategorizedSearchBoxHeight() + 5.0f);
        if (data->type == DesktopWidgetType::FileGroup && data->showFileCategories && !data->childWidgetIds.empty())
            bottom += view->Cu(view->GetCategorizedTabRowPitch());
        return {frame.left, frame.top, frame.right, bottom};
    }
    CategorizedPopupScope scope(view, frame);
    const auto* widget = view->GetWidgetData();
    LONG bottom = frame.top;
    const RECT search = view->GetSearchBoxRect();
    if (!IsRectEmptyRect(search)) bottom = search.bottom + view->Cu(4.0f);
    const RECT tabs = view->GetCategorizedTabsRect(widget->showFileCategories);
    if (!IsRectEmptyRect(tabs)) bottom = std::max(bottom, tabs.bottom + view->Cu(8.0f));
    return RECT{frame.left, frame.top, frame.right, bottom};
}

RECT DesktopApp::GetCategorizedPopupFrame(const RECT& popup) const
{
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    RECT frame{popup.left, popup.top + metrics.headerHeight, popup.right, popup.bottom};
    if (const auto* widget = GetOpenPopupWidget(); widget && IsGroupWidgetType(widget->type))
    {
        frame.bottom -= metrics.bottomPadding;
        return frame;
    }
    const auto* view = GetCategorizedPopupView();
    if (!view) return frame;
    const LONG left = popup.left + metrics.paddingX;
    const LONG right = popup.right - metrics.paddingX;
    const LONG inset = view->Cu(10.0f);
    const auto* widget = GetOpenPopupWidget();
    if (widget && UsesCollectionPopupList(*widget))
    {
        frame.left = left - inset;
        frame.right = right + inset;
        return frame;
    }
    const int columns = std::max(1, (static_cast<int>(right - left) + metrics.gapX) /
        std::max(1, metrics.cellWidth + metrics.gapX));
    // Search and category controls share the outer grid labels' horizontal edges.
    const RECT firstTitle = GetItemTextRect(
        RECT{left, 0, left + metrics.cellWidth, metrics.cellHeight}, false);
    frame.left = firstTitle.left - inset;
    frame.right = firstTitle.right + (columns - 1) * (metrics.cellWidth + metrics.gapX) + inset;
    return frame;
}

std::vector<Item*> DesktopApp::GetDockFolderPopupSelectedItems()
{
    dockFolderPopupDragItems_.clear();
    std::vector<Item*> selected;
    if (!dockFolderPopupOpen_ || !dockFolderPopupContainer_)
        return selected;

    const RECT popup = GetCollectionPopupRect(dockFolderPopupWidget_);
    for (size_t i = 0; i < GetPopupItemCount(dockFolderPopupWidget_); ++i)
    {
        FolderEntry& entry = dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(dockFolderPopupWidget_, i)];
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
    if (&widget == GetOpenPopupWidget())
        if (auto* categories = dynamic_cast<FileCategories*>(GetCategorizedPopupView()))
            return categories->GetSearchResultKeys();
    if (widget.type == DesktopWidgetType::Collection || widget.type == DesktopWidgetType::FileCategories)
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
    {
        if (&widget == GetOpenPopupWidget())
            if (auto* view = dynamic_cast<FolderMapping*>(GetCategorizedPopupView()))
                return view->GetVisibleEntryIndices().size();
        return widget.folderEntries.size();
    }
    if (widget.type == DesktopWidgetType::FileCategories)
        return GetPopupItemKeys(widget).size();
    return (widget.type == DesktopWidgetType::Collection || widget.type == DesktopWidgetType::FileCategories) ? widget.itemKeys.size() : 0;
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
    if (IsGroupWidgetType(widget.type)) return false;
    namespace layout = snowdesktop::collection_popup_layout;
    // The fan always previews the full source. Search/category preferences
    // apply to its Show all view and must not change the chosen presentation.
    const size_t sourceCount = widget.type == DesktopWidgetType::FolderMapping
        ? widget.folderEntries.size() : widget.itemKeys.size();
    // Loading/empty directories retain the chosen presentation. Entry arrivals
    // must not turn a rectangular opening snapshot into a fan at completion.
    if (layout::ResolveView(popupAnchoredToDock_, widget.fanPopup,
            widget.listMode, popupFanShowAll_) != layout::View::Fan ||
        !snowdesktop::dock_folder_popup_read::HasFanContent(
            widget.type == DesktopWidgetType::FolderMapping, sourceCount))
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
    if (IsGroupWidgetType(widget.type)) return widget.listMode;
    namespace layout = snowdesktop::collection_popup_layout;
    return layout::ResolveView(popupAnchoredToDock_, widget.fanPopup,
        widget.listMode, popupFanShowAll_) == layout::View::List;
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
    if (widget && dockFolderPopupOpen_ && GetPopupItemCount(*widget) == 0)
        return dockFolderPopupLoading_ ? _LW("widget.folder_mapping.loading") :
            dockFolderPopupAvailable_ ? _LW("widget.folder_mapping.empty") :
            _LW("widget.folder_mapping.unavailable");
    if (!widget || index >= GetPopupItemCount(*widget))
        return _LW("app.interact.popup_show_all");
    if (widget->type == DesktopWidgetType::FolderMapping)
        return widget->folderEntries[GetPopupFolderEntryIndex(*widget, index)].name;
    const auto keys = GetPopupItemKeys(*widget);
    const auto itemIndex = FindItemIndexByKey(keys[index]);
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
    if (GetGroupPopupView()) return;
    const auto* widget = GetOpenPopupWidget();
    if (!widget || !UsesCollectionPopupFan(*widget) || GetPopupItemCount(*widget) == 0) return;
    // This is a change of the same popup, not an outside click. Retire queued
    // hook notifications against the old fan before its hit region shrinks.
    AdvanceFloatingPopupContentGeneration();
    if (handlingFloatingPopupInput_)
    {
        // The Dock's timer also samples physical presses. Consume this handled
        // press before publishing the full view, including the since-last-read bit.
        const SHORT state = GetAsyncKeyState(VK_LBUTTON);
        constexpr UINT leftButtonBit = 1u << 0;
        floatingDockPointerButtonsDown_ =
            (floatingDockPointerButtonsDown_ & ~leftButtonBit) |
            ((state & 0x8000) ? leftButtonBit : 0);
    }
    ResetCollectionPopupAnimationCache();
    ResetCollectionPopupFanScroll();
    popupFanShowAll_ = true;
    if (auto* view = GetCategorizedPopupView())
    {
        view->ClearSearchText();
        view->GetWidgetData()->activeCategoryId = L"all";
        view->InvalidateSlots();
        if (dockFolderPopupOpen_)
        {
            const size_t source = FindWidgetIndexById(dockFolderPopupMappingWidgetId_);
            if (source < widgets_.size()) widgets_[source].activeCategoryId = L"all";
        }
    }
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
    if (popupAnimationCompositorDriven_ && popupAnimationOverlay_.active &&
        &widget == GetOpenPopupWidget() && !IsRectEmptyRect(popupAnimationCacheRect_))
    {
        RECT bounds = popupAnimationCacheRect_;
        InflateRect(&bounds, -4, -4);
        return bounds;
    }
    const GridPage* page = ResolveCollectionPopupPage(widget);
    const auto metrics = GetCollectionPopupLayoutMetrics(widget);
    const bool dockFolder = dockFolderPopupOpen_ && &widget == &dockFolderPopupWidget_;
    // Keep the outer frame based on All. Filtering changes only the content
    // and scroll range, so choosing a small/empty category cannot move the
    // Dock popup or shrink its search and tab controls.
    size_t allItemCount = widget.type == DesktopWidgetType::FolderMapping
        ? widget.folderEntries.size() : widget.itemKeys.size();
    if (IsGroupWidgetType(widget.type))
        for (const auto& id : widget.childWidgetIds)
        {
            const size_t child = FindWidgetIndexById(id);
            if (child < widgets_.size())
                allItemCount = std::max(allItemCount,
                    widgets_[child].type == DesktopWidgetType::FolderMapping
                        ? widgets_[child].folderEntries.size() : widgets_[child].itemKeys.size());
        }
    if (&widget == GetOpenPopupWidget())
        if (auto* categories = dynamic_cast<FileCategories*>(GetCategorizedPopupView()))
            allItemCount = categories->CachedCategoryKeys(L"all").size();
    const size_t itemCount = snowdesktop::collection_popup_layout::LayoutItemCount(
        dockFolder && dockFolderPopupLoading_, allItemCount,
        dockFolder ? dockFolderPopupKnownItemCount_ : 0);
    if (UsesCollectionPopupFan(widget))
    {
        namespace layout = snowdesktop::collection_popup_layout;
        const RECT available = GetCollectionPopupFanWorkArea(widget);
        const int width = layout::ScaleDimension(400, metrics.scale);
        const int maximum = std::min(metrics.maximumHeight,
            static_cast<int>(available.bottom - available.top));
        const auto visible = layout::FanVisibleItemCount(metrics, maximum, itemCount);
        const int height = layout::FanFrameHeight(metrics, visible + 1);
        const int anchorX = popupHasAnchor_ ? popupAnchorPoint_.x : (available.left + available.right) / 2;
        const bool mirrored = anchorX - layout::FanRootX(metrics, width, false) < available.left;
        const int left = std::clamp(anchorX - layout::FanRootX(metrics, width, mirrored),
            static_cast<int>(available.left), static_cast<int>(available.right) - width);
        int top = CollectionPopupFanRootAbove() ? available.top : available.bottom - height;
        if (popupAnchoredToDock_)
        {
            const RECT work = page ? page->workArea : layoutWorkArea_;
            const int maxTop = static_cast<int>(std::max<LONG>(
                work.top + metrics.edgeMargin,
                work.bottom - metrics.edgeMargin - height));
            top += std::min(layout::ScaleDimension(6, metrics.scale),
                std::max(0, maxTop - top));
        }
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
        if (UsesCategorizedPopupControls(widget))
            return std::min(maxWidth, std::max(metrics.paddingX * 2 + columnCount * cellW +
                std::max(0, columnCount - 1) * metrics.gapX,
                snowdesktop::collection_popup_layout::ScaleDimension(320, metrics.scale)));
        return metrics.paddingX * 2 + columnCount * cellW +
            std::max(0, columnCount - 1) * metrics.gapX;
    };
    auto popupHeightForRows = [&](int rowCount) {
        const RECT controlProbe = GetCollectionPopupControlsRect(RECT{0, 0, maxWidth, maxHeight});
        const int controlsHeight = controlProbe.bottom - controlProbe.top;
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
            return metrics.headerHeight + controlsHeight + detailsHeader +
                snowdesktop::widget_item_layout::ContentHeight(
                    layout, static_cast<size_t>(rowCount)) +
                metrics.bottomPadding;
        }
        return metrics.headerHeight + controlsHeight + rowCount * cellH +
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
        if (popupAnchoredToDock_) top += snowdesktop::collection_popup_layout::ScaleDimension(10, metrics.scale);
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
    if (auto* group = GetGroupPopupView())
    {
        const RECT frame = GetCategorizedPopupFrame(popup);
        group->SetPopupFrame(&frame);
        return group->GetContentViewportRect();
    }
    const auto metrics = GetOpenCollectionPopupLayoutMetrics();
    if (const auto* widget = GetOpenPopupWidget(); widget && UsesCollectionPopupFan(*widget))
        return popup;
    int top = popup.top + metrics.headerHeight;
    const RECT controls = GetCollectionPopupControlsRect(popup);
    if (!IsRectEmptyRect(controls)) top = controls.bottom;
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
        content.top - snowdesktop::collection_popup_layout::ResolveDetailsHeaderHeight(metrics),
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
    const auto keys = GetPopupItemKeys(*widget);
    for (size_t i = 0; i < GetPopupItemCount(*widget); ++i)
    {
        const bool matches = widget->type == DesktopWidgetType::FolderMapping
            ? folder && folder->GetFolderEntry() == &widget->folderEntries[GetPopupFolderEntryIndex(*widget, i)]
            : desktop && FindItemIndexByKey(keys[i]) < items_.size() &&
                desktop->GetDesktopItem() == &items_[FindItemIndexByKey(keys[i])];
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
    popupAnimationContentPending_ = false;
    popupAnimationContentRevision_ = 0;
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
    if (!DrawCollectionPopupAnimationCache())
        popupAnimationCacheRect_ = {};
    else
        (void)PrepareCompositionAnimationOverlay(
            popupAnimationOverlay_, popupAnimationRenderCache_,
            popupAnimationCacheRect_, UiCompositionAnimationHost::FloatingPopup);
}

bool DesktopApp::DrawCollectionPopupAnimationCache()
{
    if (!d2dDevice_ || !GetOpenPopupWidget() || IsRectEmptyRect(popupAnimationCacheRect_))
        return false;
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
            ++popupAnimationContentRevision_,
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
    // The off-screen draw switches the shared brush cache to its context.
    // Restore lazy creation for the next desktop/floating-Dock frame.
    brushCache_.clear();
    brushCacheContext_ = nullptr;
    return ready;
}

void DesktopApp::RefreshCollectionPopupAnimationContent()
{
    if (!std::exchange(popupAnimationContentPending_, false) ||
        !popupAnimationCompositorDriven_ || !popupAnimationOverlay_.active)
        return;
    const double started = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
    // Update the same surface. Its scale/opacity curves and completion token
    // remain installed; a failed refresh is superseded by live content at the
    // normal completion handoff, without switching to UI-driven animation.
    const bool updated = DrawCollectionPopupAnimationCache() &&
        UpdateCompositionAnimationOverlayContent(popupAnimationOverlay_, popupAnimationRenderCache_);
    if (updated) CommitCompositionAnimationFrame();
    wchar_t message[240]{};
    swprintf_s(message,
        L"Popup animation content: driver=compositor updated=%d elapsedMs=%.2f revision=%llu",
        updated ? 1 : 0, snowdesktop::UiAnimationScheduler::MonotonicMilliseconds() - started,
        static_cast<unsigned long long>(popupAnimationContentRevision_));
    WriteDiagnosticLogEntry(message);
}

void DesktopApp::InvalidateCollectionPopupContent()
{
    if (!popupAnimationOverlay_.active && IsRectEmptyRect(popupAnimationCacheRect_))
        return;
    using namespace snowdesktop::popup_animation_rules;
    const auto action = RefreshContent(popupAnimation_, static_cast<std::uint64_t>(
        snowdesktop::UiAnimationScheduler::MonotonicMilliseconds()),
        popupAnimationCompositorDriven_,
        [this] {
            if (!std::exchange(popupAnimationContentPending_, true))
                InvalidateFloatingPopupWindow(false);
        },
        [this] {
            const RECT dirty = popupAnimationCacheRect_;
            // DrawAt must not reuse the obsolete loading bitmap while the
            // live surface is prepared underneath the still-attached visual.
            popupAnimationRenderCache_.Reset();
            popupAnimationCacheRect_ = {};
            UpdateFloatingPopupWindowBounds(false);
            PrepareCompositionAnimationOverlayRetirement(popupAnimationOverlay_, dirty);
        },
        [this] { ResetCollectionPopupAnimationCache(); });
    if (action == ContentRefreshAction::ContinueCompositor)
        return;
    if (action == ContentRefreshAction::FinalizeClose)
    {
        FinalizeCloseCollectionPopup();
        return;
    }
    if (action == ContentRefreshAction::ContinueAnimation)
        EnsureUiAnimationFrame();
    // The UI fallback has no independent native track to preserve.
    ApplyCollectionPopupBackdropAnimationFrame();
    InvalidateCollectionPopupAnimation(true);
}
