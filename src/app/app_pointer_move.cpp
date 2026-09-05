#include "app.h"
#include "../desktop_hover_rules.h"
#include "../collection_titleless_rules.h"
#include "../dock_magnification.h"
#include "../widget_composition_layer_rules.h"
#include "../widget_visibility_rules.h"
#include "../widget_scroll_rules.h"
#include "../ole_drag_rules.h"
#include "../page_navigation_rules.h"

// Middle-button behavior and pointer-move drag updates.

void DesktopApp::OnMiddleButtonDown(WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (renameEdit_ != nullptr)
        CommitRename(false);
    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelRequest_.modal)
        return;
    if (mouseDown_ || dragSession_.IsActive() ||
        widgetAction_ != WidgetAction::None)
        return;

    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (quickNavigationOpen_) return;
    if (GetDockContainerAtPoint(pt)) return;
    if (IsCollectionPopupInteractive() &&
        popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt)) return;
        CloseCollectionPopup();
    }

    size_t widgetIndex = static_cast<size_t>(-1);
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        const size_t candidate = n - 1;
        if (desktopIconsHidden_ &&
            !widgets_[candidate].keepWhenDesktopHidden)
            continue;
        bool hit = HitTestStandaloneWidget(candidate, pt) != WidgetHit::None;
        if (!hit && widgets_[candidate].type != DesktopWidgetType::LuaScript)
        {
            for (auto& container : containers_)
            {
                if (desktopIconsHidden_ &&
                    !IsRetainedContainer(container.get()))
                    continue;
                auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
                if (!widgetContainer ||
                    widgetContainer->GetWidgetData() != &widgets_[candidate])
                    continue;
                hit = widgetContainer->HitTestWidget(pt) != WidgetHit::None;
                break;
            }
        }
        if (hit)
        {
            widgetIndex = candidate;
            break;
        }
    }
    if (widgetIndex >= widgets_.size()) return;

    RestoreInteractionInputFocus();
    SelectWidgetOnly(widgetIndex);
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = widgetIndex;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    widgetAction_ = WidgetAction::PendingMove;
    middleButtonWidgetMove_ = true;
    InvalidateDragStaticScene();
    widgetDragOriginalCell_ = widgets_[widgetIndex].gridCell;
    widgetDragOriginalSpan_ = widgets_[widgetIndex].gridSpan;
    widgetPreviewCell_ = widgetDragOriginalCell_;
    widgetPreviewSpan_ = widgetDragOriginalSpan_;
    dragGroupOriginX_ = widgets_[widgetIndex].bounds.left;
    dragGroupOriginY_ = widgets_[widgetIndex].bounds.top;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::OnMiddleButtonUpAt(WPARAM wp, POINT point)
{
    if (!middleButtonWidgetMove_) return;
    middleButtonWidgetMove_ = false;
    OnLeftButtonUpAt(wp, point);
}

void DesktopApp::UpdateWidgetDragPageNavigation(POINT clientPoint)
{
    int navSide = 0;
    int maximumOffset = 0;
    if (!gridPages_.empty() &&
        savedPageIds_.size() > gridPages_.size())
    {
        RECT previousEdge{};
        RECT nextEdge{};
        GetNavHotEdgeRects(previousEdge, nextEdge);
        const auto target = snowdesktop::page_navigation_rules::
            HitTestPointerTarget(
                clientPoint, previousEdge, nextEdge);
        navSide = snowdesktop::page_navigation_rules::
            PointerTargetDirection(target);
        if (navSide != 0)
        {
            maximumOffset = MaxPageOffset();
            const bool directionAvailable =
                (navSide == -1 && pageOffset_ > 0) ||
                (navSide == 1 &&
                    pageOffset_ < maximumOffset);
            if (maximumOffset <= 0 || !directionAvailable)
                navSide = 0;
        }
    }
    SetPageNavHotEdgeHover(navSide);

    const bool navEnabled =
        (navSide == -1 && pageOffset_ > 0) ||
        (navSide == 1 && pageOffset_ < maximumOffset);
    if (navSide == 0 || !navEnabled)
    {
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return;
    }

    const DWORD now = GetTickCount();
    if (navAutoFlipDir_ != navSide)
    {
        navAutoFlipDir_ = navSide;
        navAutoFlipTick_ = now;
        return;
    }
    if (now - navAutoFlipTick_ <=
        snowdesktop::page_navigation_rules::
            kHotEdgeHintDelayMs)
        return;

    const int newOffset =
        NextNonEmptyOffset(pageOffset_, navSide);
    navAutoFlipTick_ = now;
    if (newOffset == pageOffset_)
        return;

    const RECT oldWidgetBounds =
        widgets_[mouseDownWidgetIndex_].bounds;
    pageOffset_ = newOffset;
    ApplyPageMapping();
    LayoutItems();
    RefreshPageNavHotEdgeHoverAt(clientPoint);
    const RECT newWidgetBounds =
        widgets_[mouseDownWidgetIndex_].bounds;
    const int dx =
        newWidgetBounds.left - oldWidgetBounds.left;
    const int dy =
        newWidgetBounds.top - oldWidgetBounds.top;
    dragGroupOriginX_ += dx;
    dragGroupOriginY_ += dy;
    mouseDownPoint_.x += dx;
    mouseDownPoint_.y += dy;
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
    PresentDesktopPointerUpdate();
}

void DesktopApp::OnMouseMoveAt(
    WPARAM wp, POINT current,
    bool* dragPreviewSynced)
{
    if (dragPreviewSynced)
        *dragPreviewSynced = false;
    (void)wp;
    const POINT tracePoint = current;
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseMoveBegin,
        tracePoint);
    if (!handlingFloatingDockInput_ &&
        !handlingFloatingPopupInput_)
    {
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd_;
        TrackMouseEvent(&mouseTrack);
    }

    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;
    UpdateSystemTaskbarRevealGuard();
    const bool activeWidgetGesture =
        (widgetAction_ == WidgetAction::Move ||
         widgetAction_ == WidgetAction::Resize) &&
        mouseDownWidgetIndex_ < widgets_.size();
    const bool marqueePointerGesture =
        IsMarqueePointerGesturePendingOrActive();
    if (!activeWidgetGesture && !marqueePointerGesture)
    {
        // Once a component owns the captured pointer, Dock previews, Lua
        // hover routing and popup dwell state cannot consume this sample.
        // Keep them out of the per-pixel component drag path; the transition
        // into Move/Resize clears their last visible state once below.
        UpdateDockWindowPreview(current);

        if (widgetEngine_)
        {
            bool interactionHoverRouted = false;
            if (!luaWidgetPanelRequest_.widgetId.empty() &&
                luaWidgetPanelAnimation_.IsInteractive())
            {
                widgetEngine_->ClearInteractionHover("desktop");
                const RECT content = GetLuaWidgetPanelContentRect();
                const std::string& surface =
                    luaWidgetPanelRequest_.surface;
                if (PtInRect(&content, current))
                {
                    widgetEngine_->UpdateInteractionHover(
                        luaWidgetPanelRequest_.widgetId,
                        current.x - content.left,
                        current.y - content.top, surface);
                    interactionHoverRouted = true;
                }
                else
                    widgetEngine_->ClearInteractionHover(surface);
            }
            else
            {
                const size_t interactionWidget =
                    HitTestStandaloneWidgetIndex(current);
                if (interactionWidget < widgets_.size() &&
                    widgets_[interactionWidget].type ==
                        DesktopWidgetType::LuaScript &&
                    HitTestStandaloneWidget(interactionWidget, current) ==
                        WidgetHit::Content)
                {
                    const RECT frame = GetStandaloneWidgetFrameRect(
                        widgets_[interactionWidget]);
                    widgetEngine_->EnsureWidgetLoaded(
                        widgets_[interactionWidget].id,
                        widgets_[interactionWidget].packageId);
                    widgetEngine_->UpdateInteractionHover(
                        widgets_[interactionWidget].id,
                        current.x - frame.left,
                        current.y - frame.top);
                    interactionHoverRouted = true;
                }
            }
            if (!interactionHoverRouted)
                widgetEngine_->ClearInteractionHover();
        }

        if (luaWidgetPanelMouseDown_ &&
            !luaWidgetPanelRequest_.widgetId.empty() &&
            widgetEngine_)
        {
            const RECT content =
                GetLuaWidgetPanelContentRect();
            const int localX =
                current.x - content.left;
            const int localY =
                current.y - content.top;
            const std::string& surface =
                luaWidgetPanelRequest_.surface;
            const bool handled =
                widgetEngine_->HandleHostInputPointerMove(
                    luaWidgetPanelRequest_.widgetId,
                    localX, localY, surface);
            const bool pointerCaptured =
                widgetEngine_->HasInteractionPointerCapture(
                    luaWidgetPanelRequest_.widgetId, surface);
            if (!handled &&
                (PtInRect(&content, current) || pointerCaptured))
            {
                const char* eventName = surface == "dialog"
                    ? "onDialogMouseMove"
                    : (surface == "popover"
                        ? "onPopoverMouseMove"
                        : "onPanelMouseMove");
                widgetEngine_->InvokeMouseEvent(
                    luaWidgetPanelRequest_.widgetId,
                    eventName,
                    localX, localY, 1, 0);
            }
            UpdateHostInputImePosition();
            (void)PresentDesktopForegroundComposition(content);
            return;
        }

        if (!luaWidgetPanelRequest_.widgetId.empty() &&
            luaWidgetPanelRequest_.modal)
        {
            PresentDesktopPointerUpdate();
            return;
        }

    if (popupScrollbarDragging_ && IsCollectionPopupInteractive())
    {
        DesktopWidget* popupWidget = dockFolderPopupOpen_
            ? &dockFolderPopupWidget_
            : (popupWidgetIndex_ < widgets_.size()
                ? &widgets_[popupWidgetIndex_] : nullptr);
        if (popupWidget)
        {
            const RECT popup = GetCollectionPopupRect(*popupWidget);
            const RECT viewport = GetCollectionPopupContentRect(popup);
            const int visible = std::max<int>(
                1, viewport.bottom - viewport.top);
            const int maximum = GetCollectionPopupMaxScrollOffset(
                *popupWidget, popup);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewport.top, viewport.bottom,
                    visible + maximum, visible,
                    popupScrollbarDragStartOffset_);
            popupScrollOffset_ = snowdesktop::widget_scroll_rules::
                ApplyScrollbarThumbDrag(
                    popupScrollbarDragStartOffset_,
                    current.y - popupScrollbarDragStartY_, geometry);
            (void)PresentDesktopForegroundComposition(popup);
        }
        return;
    }

    if (widgetScrollbarDragging_ && widgetScrollbarDragContainer_)
    {
        auto* container = widgetScrollbarDragContainer_;
        auto* data = container->GetWidgetData();
        const int maximum = container->GetMaxScrollOffset();
        const int visible = container->GetVisibleContentHeight();
        const RECT viewport = container->GetContentViewportRect();
        const auto geometry = snowdesktop::widget_scroll_rules::
            ResolveScrollbarAxisGeometry(
                viewport.top, viewport.bottom,
                visible + maximum, visible,
                widgetScrollbarDragStartOffset_,
                container->GetCellScale());
        if (data)
        {
            data->scrollOffset = snowdesktop::widget_scroll_rules::
                ApplyScrollbarThumbDrag(
                    widgetScrollbarDragStartOffset_,
                    current.y - widgetScrollbarDragStartY_, geometry);
            if (auto* group = dynamic_cast<FileGroup*>(container))
                group->InvalidateHostedView();
            else
                container->InvalidateSlots();
            (void)QueueDesktopWidgetComposition(data->id);
        }
        return;
    }

    for (auto& container : containers_)
    {
        auto* searchable =
            dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!searchable ||
            !searchable->IsSearchPointerSelecting())
            continue;
        searchable->UpdateSearchPointerSelection(current);
        UpdateHostInputImePosition();
        if (DesktopWidget* data = searchable->GetWidgetData())
            (void)QueueDesktopWidgetComposition(data->id);
        return;
    }

    if (detailColumnResizeActive_)
    {
        DesktopWidget* resizedWidget =
            detailColumnResizePopup_
            ? GetOpenPopupWidget()
            : (mouseDownWidgetIndex_ < widgets_.size()
                ? &widgets_[mouseDownWidgetIndex_]
                : nullptr);
        if (resizedWidget)
        {
            auto& widget = *resizedWidget;
            const float proposed = static_cast<float>(
                current.x - detailColumnResizeHeaderLeft_) /
                static_cast<float>(detailColumnResizeHeaderWidth_);
            const snowdesktop::list_detail_rules::DividerPositions positions{
                widget.detailModifiedPosition,
                widget.detailTypePosition,
                widget.detailSizePosition };
            const float position = snowdesktop::list_detail_rules::
                ClampDraggedPosition(
                    detailColumnResizeColumn_, proposed,
                    widget.detailShowModified,
                    widget.detailShowType,
                    widget.detailShowSize,
                    positions);
            switch (detailColumnResizeColumn_)
            {
            case snowdesktop::list_detail_rules::Column::Modified:
                widget.detailModifiedPosition = position;
                break;
            case snowdesktop::list_detail_rules::Column::Type:
                widget.detailTypePosition = position;
                break;
            case snowdesktop::list_detail_rules::Column::Size:
                widget.detailSizePosition = position;
                break;
            default:
                break;
            }
            if (detailColumnResizePopup_)
            {
                const RECT popup =
                    GetCollectionPopupRect(widget);
                (void)PresentDesktopForegroundComposition(
                    popup);
            }
            else
            {
                (void)QueueDesktopWidgetComposition(widget.id);
            }
        }
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }

    if (!dragSession_.IsActive() && widgetAction_ == WidgetAction::None &&
        mouseDownWidgetIndex_ < widgets_.size() &&
        widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript &&
        widgetEngine_)
    {
        WidgetHit hit = HitTestStandaloneWidget(mouseDownWidgetIndex_, current);
        RECT frame =
            GetStandaloneWidgetFrameRect(
                widgets_[mouseDownWidgetIndex_]);
        widgetEngine_->EnsureWidgetLoaded(
            widgets_[mouseDownWidgetIndex_].id,
            widgets_[mouseDownWidgetIndex_].packageId);
        const bool hostInputHandled =
            widgetEngine_->HandleHostInputPointerMove(
                widgets_[mouseDownWidgetIndex_].id,
                current.x - frame.left,
                current.y - frame.top);
        if (hostInputHandled)
            UpdateHostInputImePosition();
        const bool pointerCaptured =
            widgetEngine_->HasInteractionPointerCapture(
                widgets_[mouseDownWidgetIndex_].id);
        if (!hostInputHandled &&
            (hit == WidgetHit::Content || pointerCaptured))
        {
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseMove",
                current.x - frame.left, current.y - frame.top,
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0, 0);
        }

        // Lua content owns the pointer interaction. Do not fall through to
        // the desktop marquee-selection state machine: a drag inside a Lua
        // widget has meaning only to the widget or its host input control.
        (void)QueueDesktopWidgetComposition(
            widgets_[mouseDownWidgetIndex_].id);
        return;
    }

    const bool pressedDockEntryWithoutSelection =
        mouseDownHit_ &&
        dynamic_cast<DockEntryItem*>(mouseDownHit_) &&
        dockPressedEntry_ < dockEntries_.size();
    if (!dragSession_.IsActive() && mouseDown_ && mouseDownHit_ &&
        (mouseDownHit_->IsSelected() ||
            pressedDockEntryWithoutSelection))
    {
        if (dynamic_cast<DockFrequentItem*>(mouseDownHit_) ||
            dynamic_cast<DockRunningItem*>(mouseDownHit_))
            return;
        const bool dockItem = dynamic_cast<DockContainer*>(
            mouseDownHit_->GetContainer()) != nullptr;
        const int thresholdX = dockItem
            ? std::max(8, GetSystemMetrics(SM_CXDRAG)) : 3;
        const int thresholdY = dockItem
            ? std::max(8, GetSystemMetrics(SM_CYDRAG)) : 3;
        if (std::abs(current.x - mouseDownPoint_.x) > thresholdX ||
            std::abs(current.y - mouseDownPoint_.y) > thresholdY)
        {
            Container* source = mouseDownHit_->GetContainer();
            std::vector<Item*> sourceItems =
                source ==
                    dockFolderPopupContainer_.get()
                ? GetDockFolderPopupSelectedItems()
                : (source
                    ? source->GetSelectedItems()
                    : std::vector<Item*>{});
            if (sourceItems.empty() &&
                pressedDockEntryWithoutSelection)
            {
                sourceItems.push_back(mouseDownHit_);
            }
            DragSourceList sourceList = BuildDragSourceList(sourceItems, source);
            if (sourceItems.empty())
            {
                return;
            }
            std::vector<RECT> visualItemBounds;
            visualItemBounds.reserve(sourceItems.size());
            for (Item* item : sourceItems)
                visualItemBounds.push_back(
                    item ? item->GetBounds() : RECT{});
            PrepareDockBackdropForDragTransition();
            dragSession_.Begin(source, std::move(sourceItems), std::move(sourceList),
                mouseDownPoint_, current);
            if (hwnd_ && IsWindow(hwnd_))
            {
                SetTimer(
                    hwnd_, kNativeDragHoverRecoveryTimerId,
                    kNativeDragHoverRecoveryIntervalMs,
                    nullptr);
            }
            dragSession_.SetVisualItemBounds(
                std::move(visualItemBounds));
            auto* listSource =
                dynamic_cast<ListContainer*>(source);
            const bool listIconDrag =
                listSource && listSource->SingleColumn() &&
                (dynamic_cast<DesktopIcon*>(mouseDownHit_) ||
                 dynamic_cast<FolderEntryIcon*>(mouseDownHit_));
            if (listIconDrag)
            {
                const RECT pressedBounds =
                    mouseDownHit_->GetBounds();
                const RECT iconBounds =
                    GetItemIconRect(pressedBounds);
                if (!IsRectEmptyRect(iconBounds))
                {
                    dragSession_.AnchorToPointer({
                        iconBounds.left +
                            (iconBounds.right -
                                iconBounds.left) / 2,
                        iconBounds.top +
                            (iconBounds.bottom -
                                iconBounds.top) / 2
                    });
                }
            }
            // From this point the drag session owns the logical interaction.
            // Do not retain the original wrapper pointer across object rebuilds.
            mouseDownHit_ = nullptr;
            pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
            pendingCtrlToggleWidgetItem_ = nullptr;
            marqueeActive_ = false;
            marqueeWidgetIndex_ = static_cast<size_t>(-1);
            marqueeDockFolderPopup_ = false;
            if (source == GetDesktopGrid())
            {
                UpdateDragGroupOrigin();
            }
            else
            {
                RECT groupBounds{};
                bool hasBounds = false;
                for (auto* item : dragSession_.Items())
                {
                    if (!item) continue;
                    RECT bounds = item->GetBounds();
                    if (IsRectEmptyRect(bounds)) continue;
                    groupBounds = hasBounds ? UnionCopy(groupBounds, bounds) : bounds;
                    hasBounds = true;
                }
                if (hasBounds)
                {
                    dragGroupOriginX_ = groupBounds.left;
                    dragGroupOriginY_ = groupBounds.top;
                }
            }
        }
    }

        UpdateCollectionPopupDwell(current);
        UpdateCollectionGroupTabDwell(current);
    }

    if (mouseDown_ && !dragSession_.IsActive()
        && (widgetAction_ == WidgetAction::PendingMove || widgetAction_ == WidgetAction::PendingResize)
        && mouseDownWidgetIndex_ < widgets_.size()
        && (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3))
    {
        if (widgetAction_ == WidgetAction::PendingMove)
            widgetAction_ = WidgetAction::Move;
        else if (widgetAction_ == WidgetAction::PendingResize)
            widgetAction_ = WidgetAction::Resize;
        if (widgetEngine_)
            widgetEngine_->ClearInteractionHover();
        HideDockWindowPreview();
        InvalidateRect(hwnd_, nullptr, FALSE);
        // The transition paint hides the source widget composition and
        // establishes the initial overlay. Later samples can update only the
        // foreground composition without walking the complete desktop scene.
        PresentDesktopPointerUpdate();
    }

    // Widget resize preview
    if (widgetAction_ == WidgetAction::Resize && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        const auto& widget = widgets_[mouseDownWidgetIndex_];
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            int stepX = std::max(1, page->itemPitchWidth);
            int stepY = std::max(1, page->itemPitchHeight);
            int dCol = static_cast<int>(std::round(static_cast<double>(current.x - mouseDownPoint_.x) / static_cast<double>(stepX)));
            int dRow = static_cast<int>(std::round(static_cast<double>(current.y - mouseDownPoint_.y) / static_cast<double>(stepY)));

            GridCell cell = widgetDragOriginalCell_;
            GridSpan span = widgetDragOriginalSpan_;
            span.columns += dCol;
            span.rows += dRow;
            span = ClampWidgetGridSpan(widget, span,
                page->columns - cell.column, page->rows - cell.row);

            widgetPreviewCell_ = cell;
            widgetPreviewSpan_ = span;
        }
        ShowDragHintWindow(current, _LW("core.drag.resize_widget"));
        return;
    }

    // Widget drag preview
    if (widgetAction_ == WidgetAction::Move && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline int SlotFromCell(const std::vector<GridPage>&, const GridCell&);
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);

        const DesktopWidgetType movingType =
            widgets_[mouseDownWidgetIndex_].type;
        const auto movingPayload = snowdesktop::slot_contract::
            PayloadForWidgetType(movingType);
        const bool movingCollection =
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::CollectionGroup,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        const bool movingFileSource =
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::FileGroup,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        const size_t groupTarget = movingCollection
            ? HitTestCollectionGroupIndex(
                current, mouseDownWidgetIndex_)
            : (movingFileSource
                ? HitTestFileGroupIndex(
                    current, mouseDownWidgetIndex_)
                : static_cast<size_t>(-1));
        if (groupTarget < widgets_.size())
        {
            widgetCollectionGroupTargetIndex_ = groupTarget;
            widgetCollectionGroupInsertIndex_ =
                widgets_[groupTarget].childWidgetIds.size();
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (!group ||
                    group->GetWidgetData() != &widgets_[groupTarget])
                    continue;
                Slot* slot = nullptr;
                HitRegion region = group->HitTestDrag(current, slot);
                const bool overTab =
                    widgets_[groupTarget].type ==
                        DesktopWidgetType::CollectionGroup
                        ? !dynamic_cast<CollectionGroup*>(group)->
                            CategoryIdAtPoint(current).empty()
                        : !dynamic_cast<FileGroup*>(group)->
                            SourceIdAtPoint(current).empty();
                if (overTab)
                    widgetCollectionGroupInsertIndex_ =
                        group->GetDropInsertIndex(slot, region);
                break;
            }
            widgetDockTarget_ = false;
            widgetDockTargetContainer_ = nullptr;
            ShowDragHintWindow(current,
                _LW(movingCollection
                    ? "core.drag.move_collection_group"
                    : "core.drag.move_file_group"));
            return;
        }
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);

        DockContainer* dock = GetDockContainerAtPoint(current);
        const bool canDock = dock &&
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Dock,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        if (canDock)
        {
            widgetDockTarget_ = true;
            widgetDockTargetContainer_ = dock;
            widgetDockInsertIndex_ = dock->GetInsertIndexAtPoint(current);
            ShowDragHintWindow(current, _LW("core.drag.move_collection_dock"));
            return;
        }
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;

        UpdateWidgetDragPageNavigation(current);

        POINT adjusted = {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
        GridCell cell = CellFromPointForDrag(adjusted);
        if (!cell.pageId.empty())
        {
            const GridPage* page = FindGridPage(gridPages_, cell.pageId);
            if (page)
            {
                cell = ClampGridCellToFitPage(
                    *page, cell,
                    widgetDragOriginalSpan_);
            }
            widgetPreviewCell_ = cell;
        }
        ShowDragHintWindow(current, _LW("core.drag.move_widget"));
        return;
    }

    if (dragSession_.IsActive() && !dragSession_.Items().empty())
    {
        dragSession_.UpdatePoint(current);
        int currentMods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
        dragSession_.UpdateActionFromMods(currentMods);

        SyncDragPreviewWindow();
        if (dragPreviewSynced)
            *dragPreviewSynced = true;
        bool overExternal = IsExternalDropWindowAt(current);

        if (overExternal)
        {
            DropPayload payload = DropPayload::From(dragSession_.Items());
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                const bool dockFolderPopupSource =
                    dockFolderPopupOpen_ &&
                    dragSession_.Source() ==
                        dockFolderPopupContainer_.get();
                auto* sourceWidget = dynamic_cast<WidgetContainer*>(dragSession_.Source());
                DesktopWidget* sourceWidgetData = sourceWidget ? sourceWidget->GetWidgetData() : nullptr;
                const std::wstring sourceWidgetId =
                    sourceWidgetData ? sourceWidgetData->id : L"";
                const bool sourceFolderMapping =
                    sourceWidgetData &&
                    sourceWidgetData->type ==
                        DesktopWidgetType::FolderMapping;
                const HWND nativeCaptureHwnd = GetCapture();

                HideDragHintWindow();
                // Crossing into another process proves that this is a drag,
                // not a Dock click. Do not carry raw pressed-container
                // pointers through DoDragDrop's rebuilding message loop.
                ClearDockPressedState();
                ReleaseCapturePreservingPointerState();
                mouseDown_ = false;
                mouseDownHit_ = nullptr;
                SetPageNavHotEdgeHover(0);
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;
                ResetDockHandoffDwell();
                CancelCollectionPopupDwell();
                CancelCollectionGroupTabDwell();

                // OLE now owns feedback while the pointer is over another
                // application. Clear both the custom ghost and the last
                // SnowDesktop drop indicator before entering its nested loop;
                // native feedback is restored only after DoDragDrop returns.
                dragSession_.SetVisualVisible(false);
                dragSession_.UpdateTarget(
                    nullptr, nullptr, HitRegion::None);
                PresentOleDragInteractionFrame();

                bool oleUiPumpStarted =
                    hwnd_ && IsWindow(hwnd_) &&
                    SetTimer(
                        hwnd_, kOleDragUiPumpTimerId,
                        kOleDragUiPumpIntervalMs,
                        nullptr) != 0;

                OleDragDropAdapter* oleAdapter =
                    EnsureOleDragDropAdapter();
                DWORD oleEffect = DROPEFFECT_NONE;
                HRESULT hr = E_OUTOFMEMORY;
                bool nativeDragResumed = false;
                for (;;)
                {
                    dragDropController_.BeginSelfDrag();
                    oleEffect = DROPEFFECT_COPY |
                        DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    hr = oleAdapter
                        ? DoDragDrop(dataObj.Get(),
                            static_cast<IDropSource*>(oleAdapter),
                            oleEffect, &oleEffect)
                        : E_OUTOFMEMORY;
                    const bool nativeResumeRequested =
                        dragDropController_.
                            SelfDragNativeResumeRequested();
                    dragDropController_.EndSelfDrag();
                    POINT resumePoint{};
                    const bool overDesktopSurface =
                        nativeResumeRequested &&
                        TryGetNativeDragResumePointFromCursor(
                            resumePoint);
                    const bool primaryButtonDown =
                        nativeResumeRequested &&
                        (GetAsyncKeyState(VK_LBUTTON) &
                            0x8000) != 0;
                    const auto unwindAction =
                        snowdesktop::ole_drag_rules::
                            SelectSelfOleUnwindAction(
                                nativeResumeRequested,
                                overDesktopSurface,
                                primaryButtonDown,
                                dragSession_.IsActive() &&
                                    !dragSession_.Items().empty());
                    if (unwindAction == snowdesktop::
                            ole_drag_rules::SelfOleUnwindAction::
                                RestartOle)
                    {
                        // The pointer can cross the boundary again while OLE
                        // is unwinding. If the initiating button is still
                        // held, immediately give ownership back to a fresh OLE
                        // loop so an external release cannot be lost.
                        continue;
                    }
                    if (unwindAction == snowdesktop::
                            ole_drag_rules::SelfOleUnwindAction::
                                FinishOle)
                        break;

                    // DoDragDrop has fully unwound at this point. Release its
                    // data object and reset the Shell effect cursor before the
                    // custom ghost becomes visible again.
                    if (oleUiPumpStarted && hwnd_ &&
                        IsWindow(hwnd_))
                    {
                        KillTimer(hwnd_, kOleDragUiPumpTimerId);
                        oleUiPumpStarted = false;
                    }
                    dataObj.Reset();
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                    if (unwindAction == snowdesktop::
                            ole_drag_rules::SelfOleUnwindAction::
                                ResumeNativeHeld)
                    {
                        const bool restoreFloatingDockCapture =
                            IsPersistentDockHostWindow(
                                nativeCaptureHwnd) &&
                            IsWindow(nativeCaptureHwnd) &&
                            IsWindowVisible(nativeCaptureHwnd);
                        const bool restoreFloatingPopupCapture =
                            nativeCaptureHwnd ==
                                floatingPopupHwnd_ &&
                            IsWindow(nativeCaptureHwnd) &&
                            IsWindowVisible(nativeCaptureHwnd);
                        HWND restoreCapture =
                            restoreFloatingDockCapture ||
                                restoreFloatingPopupCapture
                            ? nativeCaptureHwnd : hwnd_;
                        SetCapture(restoreCapture);
                        if (GetCapture() != restoreCapture &&
                            restoreCapture != hwnd_)
                        {
                            restoreCapture = hwnd_;
                            SetCapture(restoreCapture);
                        }
                        if (GetCapture() == restoreCapture)
                        {
                            mouseDown_ = true;
                            mouseDownHit_ = nullptr;
                            dragSession_.SetVisualVisible(true);
                            OnMouseMoveAt(0, resumePoint);
                        }
                        else
                        {
                            CancelActiveItemDrag();
                        }
                    }
                    else
                    {
                        // The button may be released in the short interval
                        // between QueryContinueDrag and DoDragDrop returning.
                        // Commit exactly once at the live native point without
                        // reacquiring capture or flashing the custom ghost.
                        OnLeftButtonUpAt(0, resumePoint);
                    }
                    PresentPointerInteractionFrame();
                    nativeDragResumed = true;
                    break;
                }
                if (oleUiPumpStarted && hwnd_ && IsWindow(hwnd_))
                    KillTimer(hwnd_, kOleDragUiPumpTimerId);
                if (nativeDragResumed)
                    return;

                // Ordinary completion, Escape and failure paths do not resume
                // the custom ghost, but they must still release objects that
                // can retain Shell drag UI before any reload or layout work.
                dataObj.Reset();
                POINT finishedOlePoint{};
                if (dragDropController_.SelfDragReturned() ||
                    TryGetNativeDragResumePointFromCursor(
                        finishedOlePoint))
                {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }

                if (hr == DRAGDROP_S_DROP && oleEffect == DROPEFFECT_MOVE
                    && !dragDropController_.SelfDragReturned() &&
                    payload.hasDesktopIcons)
                {
                    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
                    {
                        if (it->selected && !it->desktopIconClsid.empty()) continue;
                        if (!it->selected) continue;
                        wchar_t path[MAX_PATH]{};
                        if (SHGetPathFromIDList(it->absolutePidl.get(), path))
                        {
                            std::vector<snowdesktop::ShellFileOperationStep>
                                steps;
                            steps.push_back({
                                FO_DELETE,
                                { path },
                                {},
                                static_cast<FILEOP_FLAGS>(
                                    FOF_ALLOWUNDO |
                                    FOF_NOCONFIRMATION) });
                            QueueShellFileOperation(
                                std::move(steps),
                                [this](bool succeeded) {
                                    if (succeeded)
                                        ReloadItems(false);
                                });
                        }
                    }
                    SaveLayoutSlots();
                }

                if (!dragDropController_.SelfDragReturned() &&
                    sourceFolderMapping)
                {
                    for (size_t i = 0; i < widgets_.size(); ++i)
                    {
                        if (widgets_[i].id == sourceWidgetId &&
                            widgets_[i].type ==
                                DesktopWidgetType::FolderMapping)
                        {
                            RefreshFolderMappingWidget(i);
                            break;
                        }
                    }
                }

                if (!dragDropController_.SelfDragReturned())
                {
                    ClearSelection();
                    CancelActiveItemDrag();
                    ReloadItems();
                    if (dockFolderPopupSource &&
                        dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                }
                else
                {
                    SaveLayoutSlots();
                    ClearSelection();
                    CancelActiveItemDrag();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return;
            }
        }

        const bool suppressDesktopWidgetTargets =
            SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (suppressDesktopWidgetTargets)
        {
            SetPageNavHotEdgeHover(0);
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
        }
        else if (!UpdateDragPageNavigation(current))
            return;

        // OO hit testing: iterate all containers in reverse (topmost first)
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(current, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            const DragTargetResolution resolved =
                dragDropController_.ResolveInternalTarget(
                    containers_, current,
                    [&](const Container& candidate) {
                        if (desktopIconsHidden_ &&
                            !IsRetainedContainer(&candidate))
                            return false;
                        return !suppressDesktopWidgetTargets ||
                            (!dynamic_cast<const DesktopGrid*>(&candidate) &&
                             !dynamic_cast<const WidgetContainer*>(&candidate));
                    });
            targetContainer = resolved.container;
            targetSlot = resolved.slot;
            targetRegion = resolved.region;
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        std::wstring hint;
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(current);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), currentMods);

        ShowDragHintWindow(current, hint);
        return;
    }

    if (mouseDown_ && !mouseDownHit_ &&
        pendingGuideAction_ == WidgetHit::None)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            const bool startingMarquee = !marqueeActive_;
            if (startingMarquee)
            {
                dragRenderCache_.Reset();
                marqueeFullPresentPending_ = true;
            }
            marqueeActive_ = true;
            UpdateMarqueeSelection(current);
            if (!startingMarquee &&
                marqueeWidgetIndex_ < widgets_.size() &&
                popupWidgetIndex_ != marqueeWidgetIndex_)
            {
                (void)QueueDesktopWidgetComposition(
                    widgets_[marqueeWidgetIndex_].id);
            }
            if (startingMarquee)
                InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    {
        int oldHover = navHoverSide_;
        const bool oldHotEdge = navHotEdgeHover_;
        const bool oldHintVisible = navHotEdgeHintVisible_;
        RefreshPageNavHotEdgeHoverAt(current);
        const bool movingHotEdgeHint =
            navHotEdgeHintVisible_ && oldHintVisible &&
            navHotEdgeHover_ && oldHotEdge &&
            navHoverSide_ == oldHover &&
            oldMouse.y != current.y;
        if (navHoverSide_ != oldHover ||
            navHotEdgeHover_ != oldHotEdge ||
            movingHotEdgeHint)
        {
            RECT prevEdge{};
            RECT nextEdge{};
            GetNavHotEdgeRects(prevEdge, nextEdge);
            RECT dirty{};
            auto addDirty = [&](RECT value) {
                if (IsRectEmptyRect(value)) return;
                if (IsRectEmptyRect(dirty)) dirty = value;
                else UnionRect(&dirty, &dirty, &value);
            };
            if (oldHotEdge)
            {
                addDirty(oldHover < 0 ? prevEdge : nextEdge);
                if (oldHintVisible)
                    addDirty(GetPageNavHotEdgeHintBounds(
                        oldHover, oldMouse));
            }
            if (navHotEdgeHover_)
            {
                addDirty(navHoverSide_ < 0 ? prevEdge : nextEdge);
                if (navHotEdgeHintVisible_)
                    addDirty(GetPageNavHotEdgeHintBounds(
                        navHoverSide_, current));
            }
            InflateRect(&dirty, 4, 4);
            if (!IsRectEmptyRect(dirty))
                (void)PresentDesktopForegroundComposition(dirty);
        }
    }

    if (oldMouse.x != current.x || oldMouse.y != current.y)
    {
        struct MouseHoverVisual
        {
            const void* owner = nullptr;
            const void* target = nullptr;
            int kind = 0;
            size_t index = 0;
            bool continuous = false;
            const DesktopWidget* widget = nullptr;
            RECT bounds{};
            snowdesktop::widget_composition_layer_rules::
                PointerVisualLayer layer =
                    snowdesktop::widget_composition_layer_rules::
                        PointerVisualLayer::None;
        };
        auto sameHoverVisual = [](const MouseHoverVisual& a,
            const MouseHoverVisual& b) {
            return a.owner == b.owner &&
                a.target == b.target &&
                a.kind == b.kind &&
                a.index == b.index;
        };
        auto findHoverVisual = [&](POINT point) -> MouseHoverVisual {
            if (const DesktopWidget* popupWidget =
                    GetOpenPopupWidget();
                IsCollectionPopupInteractive() &&
                (!desktopIconsHidden_ || IsOpenPopupRetained()) &&
                popupWidget &&
                !IsRectEmptyRect(popupRect_) &&
                PtInRect(&popupRect_, point))
            {
                RECT content = GetCollectionPopupContentRect(popupRect_);
                if (PtInRect(&content, point))
                {
                    const size_t popupItemCount =
                        GetPopupItemCount(*popupWidget);
                    for (size_t i = 0;
                         i < popupItemCount; ++i)
                    {
                        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
                        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom)
                            continue;
                        if (PtInRect(&itemRect, point))
                            return {
                                popupWidget, popupWidget, 1, i, false,
                                nullptr, popupRect_,
                                snowdesktop::widget_composition_layer_rules::
                                    PointerVisualLayer::Foreground };
                    }
                }
                return {
                    popupWidget, popupWidget, 2, 0, true,
                    nullptr, popupRect_,
                    snowdesktop::widget_composition_layer_rules::
                        PointerVisualLayer::Foreground };
            }

            if (DockContainer* dock = GetDockContainerAtPoint(point))
            {
                if (dock->ContainsInteractivePoint(point))
                {
                    if (DockEntryItem* entry = dock->EntryAtPoint(point))
                        return {
                            dock, entry, 8, entry->GetEntryIndex(), true,
                            nullptr, {},
                            snowdesktop::widget_composition_layer_rules::
                                PointerVisualLayer::Foreground };
                    if (DockRunningItem* item = dock->RunningItemAtPoint(point))
                        return {
                            dock, item, 12, item->GetRunningIndex(), true,
                            nullptr, {},
                            snowdesktop::widget_composition_layer_rules::
                                PointerVisualLayer::Foreground };
                    if (DockFrequentItem* item = dock->FrequentItemAtPoint(point))
                        return {
                            dock, item, 11, item->GetItemIndex(), true,
                            nullptr, {},
                            snowdesktop::widget_composition_layer_rules::
                                PointerVisualLayer::Foreground };
                    if (dock->IsWindowsButtonPoint(point))
                        return {
                            dock, dock, 13, 0, true, nullptr, {},
                            snowdesktop::widget_composition_layer_rules::
                                PointerVisualLayer::Foreground };
                    if (dock->IsSearchPoint(point))
                        return {
                            dock, dock, 9, 0, true, nullptr, {},
                            snowdesktop::widget_composition_layer_rules::
                                PointerVisualLayer::Foreground };
                    return {
                        dock, dock, 10, 0, true, nullptr, {},
                        snowdesktop::widget_composition_layer_rules::
                            PointerVisualLayer::Foreground };
                }
            }

            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                if (desktopIconsHidden_ &&
                    !IsRetainedContainer(it->get()))
                    continue;
                auto* widget = dynamic_cast<WidgetContainer*>(it->get());
                if (!widget)
                    continue;
                RECT frame = widget->GetFrameRect();
                if (IsRectEmptyRect(frame) || !PtInRect(&frame, point))
                    continue;

                DesktopWidget* widgetData = widget->GetWidgetData();
                const WidgetHit hit = widget->HitTestWidget(point);
                if (hit == WidgetHit::Content)
                {
                    RECT content = widget->GetContentViewportRect();
                    if (!IsRectEmptyRect(content) && PtInRect(&content, point))
                    {
                        const auto& slots = widget->GetSlots();
                        for (const auto& slot : slots)
                        {
                            if (!slot)
                                continue;
                            RECT slotRect = slot->GetBounds();
                            if (IsRectEmptyRect(slotRect) || !PtInRect(&slotRect, point))
                                continue;
                            RECT visible{};
                            if (!IntersectRect(&visible, &slotRect, &content))
                                continue;
                            Item* item = slot->GetItem();
                            const bool selectedNeedsHover =
                                widgetData &&
                                widgetData->type ==
                                    DesktopWidgetType::Collection &&
                                snowdesktop::collection_titleless_rules::
                                    IsActive(
                                        widgetData->largeFolderTitleless,
                                        widgetData->scrollContainerMode,
                                        widgetData->gridSpan.columns,
                                        widgetData->gridSpan.rows);
                            if (item &&
                                (!item->IsSelected() ||
                                    selectedNeedsHover))
                                return {
                                    widgetData, slot.get(), 3,
                                    slot->GetIndex(), false,
                                    widgetData, frame,
                                    snowdesktop::widget_composition_layer_rules::
                                        PointerVisualLayer::Widget };
                            break;
                        }
                    }
                    return {
                        widgetData, widgetData, 4, 0, false,
                        widgetData, frame,
                        snowdesktop::widget_composition_layer_rules::
                            PointerVisualLayer::Widget };
                }

                return {
                    widgetData,
                    widgetData,
                    5,
                    static_cast<size_t>(hit),
                    hit != WidgetHit::None,
                    widgetData,
                    frame,
                    snowdesktop::widget_composition_layer_rules::
                        PointerVisualLayer::Widget
                };
            }

            const size_t standalone = HitTestStandaloneWidgetIndex(point);
            if (standalone < widgets_.size())
            {
                const WidgetHit hit = HitTestStandaloneWidget(standalone, point);
                return {
                    &widgets_[standalone],
                    &widgets_[standalone],
                    6,
                    static_cast<size_t>(hit),
                    hit != WidgetHit::Content && hit != WidgetHit::None,
                    &widgets_[standalone],
                    GetStandaloneWidgetFrameRect(widgets_[standalone]),
                    snowdesktop::widget_composition_layer_rules::
                        PointerVisualLayer::Widget
                };
            }

            for (int i = static_cast<int>(items_oo_.size()) - 1; i >= 0; --i)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(items_oo_[i].get());
                if (!icon)
                    continue;
                DesktopItem* item = icon->GetDesktopItem();
                if (!item || item->selected || IsRectEmptyRect(item->bounds))
                    continue;
                if (!item->layoutKey.empty() &&
                    collectedKeysCache_.count(ToUpperInvariant(item->layoutKey)))
                    continue;
                if (PtInRect(&item->bounds, point))
                    return {
                        item, item, 7, 0, false, nullptr, item->bounds,
                        snowdesktop::widget_composition_layer_rules::
                            PointerVisualLayer::Background };
            }

            return {};
        };

        const MouseHoverVisual oldVisual = findHoverVisual(oldMouse);
        const MouseHoverVisual newVisual = findHoverVisual(current);
        bool dockHoverPresentationTracked = false;
        for (const auto& container : containers_)
        {
            auto* dock = dynamic_cast<DockContainer*>(
                container.get());
            if (!dock ||
                (desktopIconsHidden_ &&
                    !IsRetainedContainer(dock)))
            {
                continue;
            }
            if (snowdesktop::dock_magnification::
                    ShouldTrackHoverPresentation(
                        dock->GetInteractiveBounds(),
                        oldMouse, current))
            {
                dockHoverPresentationTracked = true;
                break;
            }
        }
        const bool hoverChanged = !sameHoverVisual(oldVisual, newVisual);
        const bool needsContinuousHoverPaint =
            (oldVisual.owner && oldVisual.continuous) ||
            (newVisual.owner && newVisual.continuous) ||
            dockHoverPresentationTracked;
        const bool invalidateDesktopHover =
            snowdesktop::floating_dock_rules::
                ShouldInvalidateDesktopHover(
                    handlingFloatingDockInput_ ||
                    handlingFloatingPopupInput_);
        const bool dockHoverActive =
            (oldVisual.kind >= 8 &&
                oldVisual.kind <= 13) ||
            (newVisual.kind >= 8 &&
                newVisual.kind <= 13) ||
            dockHoverPresentationTracked;
        if (invalidateDesktopHover &&
            (marqueeActive_ || hoverChanged ||
                needsContinuousHoverPaint))
        {
            auto refreshWidgetVisual = [&](
                    const MouseHoverVisual& visual) {
                using namespace
                    snowdesktop::widget_composition_layer_rules;
                if (!NeedsWidgetSurfaceRefresh(visual.layer) ||
                    !visual.widget)
                {
                    return;
                }
                (void)QueueDesktopWidgetComposition(visual.widget->id);
            };
            refreshWidgetVisual(oldVisual);
            if (newVisual.widget != oldVisual.widget)
                refreshWidgetVisual(newVisual);

            RECT backgroundDirty{};
            RECT foregroundDirty{};
            auto includeDesktopVisual = [&](
                    const MouseHoverVisual& visual) {
                using namespace
                    snowdesktop::widget_composition_layer_rules;
                RECT* dirty = nullptr;
                if (NeedsBackgroundPaint(visual.layer))
                    dirty = &backgroundDirty;
                else if (NeedsForegroundPaint(visual.layer))
                    dirty = &foregroundDirty;
                if (!dirty || IsRectEmptyRect(visual.bounds))
                {
                    return;
                }
                if (IsRectEmptyRect(*dirty))
                    *dirty = visual.bounds;
                else
                    UnionRect(dirty, dirty, &visual.bounds);
            };
            includeDesktopVisual(oldVisual);
            includeDesktopVisual(newVisual);
            if (dockHoverActive && !marqueeActive_)
            {
                for (const auto& container : containers_)
                {
                    auto* dock = dynamic_cast<DockContainer*>(
                        container.get());
                    if (!dock)
                        continue;
                    // Dock hit-testing updates a hysteresis-backed focus
                    // rect. Reconstructing the old title/panel after resolving
                    // the current point can therefore miss pixels from the
                    // last presented frame. Redraw one bounded, focus-neutral
                    // envelope that covers every magnified icon and every
                    // possible title position.
                    const RECT hoverEnvelope =
                        snowdesktop::floating_dock_rules::
                            ExpandHostForTitleLayer(
                                dock->GetInteractiveBounds(),
                                dockSettings_.position);
                    if (IsRectEmptyRect(foregroundDirty))
                        foregroundDirty = hoverEnvelope;
                    else
                        UnionRect(
                            &foregroundDirty,
                            &foregroundDirty,
                            &hoverEnvelope);
                }
                if (!IsRectEmptyRect(foregroundDirty))
                    InflateRect(&foregroundDirty, 4, 4);
            }
            using namespace
                snowdesktop::widget_composition_layer_rules;
            const bool needsBackgroundPaint = marqueeActive_ ||
                NeedsBackgroundPaint(oldVisual.layer) ||
                NeedsBackgroundPaint(newVisual.layer);
            const bool needsForegroundPaint =
                NeedsForegroundPaint(oldVisual.layer) ||
                NeedsForegroundPaint(newVisual.layer) ||
                dockHoverActive;
            if (needsForegroundPaint && !marqueeActive_)
            {
                if (!IsRectEmptyRect(foregroundDirty))
                {
                    (void)PresentDesktopForegroundComposition(
                        foregroundDirty);
                }
                else
                {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
            if (needsBackgroundPaint)
            {
                InvalidateRect(
                    hwnd_,
                    IsRectEmptyRect(backgroundDirty)
                        ? nullptr : &backgroundDirty,
                    FALSE);
            }
            if (needsBackgroundPaint && !marqueeActive_ &&
                snowdesktop::desktop_hover_rules::
                    ShouldPresentSynchronously(
                        hoverChanged,
                        dockHoverActive))
            {
                // Hover state is pointer feedback, not an animation frame.
                // WM_PAINT has lower queue priority than pointer and animated
                // window traffic, so a plain invalidation can remain pending
                // until Quick Navigation finishes and then replay stale
                // transitions. Present each target change in this input
                // message; Dock additionally needs continuous movement.
                PresentDesktopPointerUpdate();
            }
        }

        if (invalidateDesktopHover)
        {
            bool hoverOnlyVisibilityChanged = false;
            for (size_t widgetIndex = 0;
                 widgetIndex < widgets_.size();
                 ++widgetIndex)
            {
                const auto& w = widgets_[widgetIndex];
                if (!w.showOnHoverOnly)
                    continue;
                const RECT frame =
                    GetStandaloneWidgetFrameRect(w);
                const bool pointerWasInside =
                    PtInRect(&frame, oldMouse) != FALSE;
                const bool pointerIsInside =
                    PtInRect(&frame, current) != FALSE;
                const bool interactionRetained =
                    popupWidgetIndex_ == widgetIndex ||
                    (!interactionPinnedWidgetId_.empty() &&
                        interactionPinnedWidgetId_ == w.id) ||
                    snowdesktop::widget_visibility_rules::
                        ShouldRetainForKeyboardNavigation(
                            keyboardNavVisualFocus_,
                            keyboardNavInsideWidget_,
                            keyboardNavWidgetIndex_,
                            widgetIndex);
                const auto shouldRender = [&](bool pointerInside) {
                    return snowdesktop::widget_visibility_rules::
                        ShouldRenderWidget(
                            w.showOnHoverOnly,
                            dragSession_.IsActive(),
                            dragDropController_.IsExternalDragActive(),
                            widgetAction_ == WidgetAction::Move,
                            w.selected,
                            HasSelectedFilesInWidget(widgetIndex),
                            interactionRetained,
                            pointerInside);
                };
                if (shouldRender(pointerWasInside) ==
                    shouldRender(pointerIsInside))
                    continue;
                hoverOnlyVisibilityChanged = true;
                break;
            }
            if (hoverOnlyVisibilityChanged)
                PresentPassiveHoverVisualChange();
        }
    }
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseMoveEnd,
        current);
}
