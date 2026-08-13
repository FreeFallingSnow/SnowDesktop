#include "app.h"
#include "../desktop_hover_rules.h"
#include "../widget_visibility_rules.h"

// Middle-button behavior and pointer-move drag updates.

void DesktopApp::OnMiddleButtonDown(WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (renameEdit_ != nullptr || mouseDown_ || dragSession_.IsActive() ||
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

void DesktopApp::OnMiddleButtonUp(WPARAM wp, LPARAM lp)
{
    if (!middleButtonWidgetMove_) return;
    middleButtonWidgetMove_ = false;
    OnLeftButtonUp(wp, lp);
}

void DesktopApp::OnMouseMove(WPARAM wp, LPARAM lp)
{
    (void)wp;
    const POINT tracePoint{
        GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseMoveBegin,
        tracePoint);
    if (!handlingFloatingDockInput_)
    {
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd_;
        TrackMouseEvent(&mouseTrack);
    }

    POINT current{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;
    UpdateSystemTaskbarRevealGuard();
    UpdateDockWindowPreview(current);

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
        const bool handled =
            widgetEngine_->HandleHostInputPointerMove(
                luaWidgetPanelRequest_.widgetId,
                localX, localY);
        if (!handled &&
            PtInRect(&content, current))
        {
            widgetEngine_->InvokeMouseEvent(
                luaWidgetPanelRequest_.widgetId,
                "onPanelMouseMove",
                localX, localY, 1, 0);
        }
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        PresentDesktopPointerUpdate();
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
        InvalidateRect(hwnd_, nullptr, FALSE);
        PresentDesktopPointerUpdate();
        return;
    }

    if (detailColumnResizeActive_)
    {
        if (mouseDownWidgetIndex_ < widgets_.size())
        {
            auto& widget = widgets_[mouseDownWidgetIndex_];
            const float scale = std::max(0.01f, widget.cellScale);
            const float delta = static_cast<float>(
                mouseDownPoint_.x - current.x) / scale;
            const float width = snowdesktop::list_detail_rules::
                ClampPreferredWidth(
                    detailColumnResizeStartWidth_ + delta);
            switch (detailColumnResizeColumn_)
            {
            case snowdesktop::list_detail_rules::Column::Modified:
                widget.detailModifiedWidth = width;
                break;
            case snowdesktop::list_detail_rules::Column::Type:
                widget.detailTypeWidth = width;
                break;
            case snowdesktop::list_detail_rules::Column::Size:
                widget.detailSizeWidth = width;
                break;
            default:
                break;
            }
            InvalidateRect(hwnd_, &widget.bounds, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        PresentDesktopPointerUpdate();
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
        if (!hostInputHandled && hit == WidgetHit::Content)
        {
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseMove",
                current.x - frame.left, current.y - frame.top,
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0, 0);
        }

        // Lua content owns the pointer interaction. Do not fall through to
        // the desktop marquee-selection state machine: a drag inside a Lua
        // widget has meaning only to the widget or its host input control.
        InvalidateRect(hwnd_, nullptr, FALSE);
        PresentDesktopPointerUpdate();
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
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Widget resize preview
    if (widgetAction_ == WidgetAction::Resize && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        const auto& widget = widgets_[mouseDownWidgetIndex_];
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            int stepX = std::max(1, page->cellWidth + page->gapX);
            int stepY = std::max(1, page->cellHeight + page->gapY);
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
        InvalidateRect(hwnd_, nullptr, TRUE);
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
            InvalidateRect(hwnd_, nullptr, FALSE);
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
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;

        // ── 跨页翻页：检测导航按钮悬停 + 自动翻页 ──
        if (MaxPageOffset() > 0)
        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);

            int navSide = 0;
            if (PtInRect(&prevRect, current)) navSide = -1;
            else if (PtInRect(&nextRect, current)) navSide = 1;
            navHoverSide_ = navSide;

            const bool hasPrev = pageOffset_ > 0;
            const bool hasNext = pageOffset_ < MaxPageOffset();
            const bool navEnabled = (navSide == -1 && hasPrev) || (navSide == 1 && hasNext);

            if (navSide != 0 && navEnabled)
            {
                const DWORD now = GetTickCount();
                if (navAutoFlipDir_ != navSide)
                {
                    navAutoFlipDir_ = navSide;
                    navAutoFlipTick_ = now;
                }
                else if (now - navAutoFlipTick_ > 500)
                {
                    // 触发翻页
                    int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
                    if (newOffset != pageOffset_)
                    {
                        // 保存迁移前组件实际 bounds（含页面渲染尺寸差异）
                        RECT oldWidgetBounds = widgets_[mouseDownWidgetIndex_].bounds;
                        pageOffset_ = newOffset;
                        ApplyPageMapping();
                        LayoutItems();
                        // 用实际 bounds 差值补偿 group origin + mouseDown，保持视觉连续性
                        RECT newWidgetBounds = widgets_[mouseDownWidgetIndex_].bounds;
                        const int dx = newWidgetBounds.left - oldWidgetBounds.left;
                        const int dy = newWidgetBounds.top  - oldWidgetBounds.top;
                        dragGroupOriginX_ += dx;
                        dragGroupOriginY_ += dy;
                        mouseDownPoint_.x += dx;
                        mouseDownPoint_.y += dy;
                        InvalidateDragStaticScene();
                        InvalidateRect(hwnd_, nullptr, TRUE);
                    }
                    navAutoFlipTick_ = now;
                }
            }
            else
            {
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;
            }
        }
        else
        {
            navHoverSide_ = 0;
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
        }

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
                cell.column = std::clamp(cell.column, 0, page->columns - widgetDragOriginalSpan_.columns);
                cell.row    = std::clamp(cell.row,    0, page->rows    - widgetDragOriginalSpan_.rows);
            }
            widgetPreviewCell_ = cell;
        }
        ShowDragHintWindow(current, _LW("core.drag.move_widget"));
        InvalidateRect(hwnd_, nullptr, TRUE);
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

        POINT screenPt = current;
        ClientToScreen(hwnd_, &screenPt);
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

                HideDragHintWindow();
                ReleaseCapture();
                mouseDown_ = false;
                mouseDownHit_ = nullptr;
                navHoverSide_ = 0;
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;

                dragDropController_.BeginSelfDrag();

                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);
                FlushPendingCompositionCommit();

                const bool oleUiPumpStarted =
                    hwnd_ && IsWindow(hwnd_) &&
                    SetTimer(
                        hwnd_, kOleDragUiPumpTimerId,
                        kOleDragUiPumpIntervalMs,
                        nullptr) != 0;

                DWORD oleEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                OleDragDropAdapter* oleAdapter =
                    EnsureOleDragDropAdapter();
                HRESULT hr = oleAdapter
                    ? DoDragDrop(dataObj.Get(),
                        static_cast<IDropSource*>(oleAdapter),
                        oleEffect, &oleEffect)
                    : E_OUTOFMEMORY;
                if (oleUiPumpStarted && hwnd_ && IsWindow(hwnd_))
                    KillTimer(hwnd_, kOleDragUiPumpTimerId);
                dragDropController_.EndSelfDrag();

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

                if (!dragDropController_.SelfDragReturned() && sourceWidgetData
                    && sourceWidgetData->type == DesktopWidgetType::FolderMapping)
                {
                    for (size_t i = 0; i < widgets_.size(); ++i)
                    {
                        if (&widgets_[i] == sourceWidgetData)
                        {
                            RefreshFolderMappingWidget(i);
                            break;
                        }
                    }
                }

                if (!dragDropController_.SelfDragReturned())
                {
                    ClearSelection();
                    EndDragSession();
                    ReloadItems();
                    if (dockFolderPopupSource &&
                        dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                }
                else
                {
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
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
            navHoverSide_ = 0;
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
        }
        else if (!UpdateDragPageNavigation(current))
            return;

        // OO hit testing: iterate all containers in reverse (topmost first)
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        popupDragTargetSlot_.reset();

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
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (mouseDown_ && !mouseDownHit_ &&
        pendingGuideAction_ == WidgetHit::None)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            if (!marqueeActive_)
                dragRenderCache_.Reset();
            marqueeActive_ = true;
            UpdateMarqueeSelection(current);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    {
        int oldHover = navHoverSide_;
        navHoverSide_ = 0;
        if (MaxPageOffset() > 0 || pageOffset_ > 0)
        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);
            if (pageOffset_ > 0 && PtInRect(&prevRect, current)) navHoverSide_ = -1;
            else if (pageOffset_ < MaxPageOffset() && PtInRect(&nextRect, current)) navHoverSide_ = 1;
        }
        if (navHoverSide_ != oldHover)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            PresentDesktopPointerUpdate();
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
                            return { popupWidget, popupWidget, 1, i, false };
                    }
                }
                return { popupWidget, popupWidget, 2, 0, true };
            }

            if (DockContainer* dock = GetDockContainerAtPoint(point))
            {
                if (dock->ContainsInteractivePoint(point))
                {
                    if (DockEntryItem* entry = dock->EntryAtPoint(point))
                        return { dock, entry, 8, entry->GetEntryIndex(), true };
                    if (DockRunningItem* item = dock->RunningItemAtPoint(point))
                        return { dock, item, 12, item->GetRunningIndex(), true };
                    if (DockFrequentItem* item = dock->FrequentItemAtPoint(point))
                        return { dock, item, 11, item->GetItemIndex(), true };
                    if (dock->IsWindowsButtonPoint(point))
                        return { dock, dock, 13, 0, true };
                    if (dock->IsSearchPoint(point))
                        return { dock, dock, 9, 0, true };
                    return { dock, dock, 10, 0, true };
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
                            if (item && !item->IsSelected())
                                return { widgetData, slot.get(), 3, slot->GetIndex(), false };
                            break;
                        }
                    }
                    return { widgetData, widgetData, 4, 0, false };
                }

                return {
                    widgetData,
                    widgetData,
                    5,
                    static_cast<size_t>(hit),
                    hit != WidgetHit::None
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
                    hit != WidgetHit::Content && hit != WidgetHit::None
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
                    return { item, item, 7, 0, false };
            }

            return {};
        };

        const MouseHoverVisual oldVisual = findHoverVisual(oldMouse);
        const MouseHoverVisual newVisual = findHoverVisual(current);
        const bool hoverChanged = !sameHoverVisual(oldVisual, newVisual);
        const bool needsContinuousHoverPaint =
            (oldVisual.owner && oldVisual.continuous) ||
            (newVisual.owner && newVisual.continuous);
        const bool invalidateDesktopHover =
            snowdesktop::floating_dock_rules::
                ShouldInvalidateDesktopHover(
                    handlingFloatingDockInput_);
        const bool dockHoverActive =
            (oldVisual.kind >= 8 &&
                oldVisual.kind <= 13) ||
            (newVisual.kind >= 8 &&
                newVisual.kind <= 13);
        if (invalidateDesktopHover &&
            (marqueeActive_ || hoverChanged ||
                needsContinuousHoverPaint))
        {
            RECT dirty{};
            if (dockHoverActive && !marqueeActive_)
            {
                for (const auto& container : containers_)
                {
                    auto* dock = dynamic_cast<DockContainer*>(
                        container.get());
                    if (!dock)
                        continue;
                    const RECT oldPanel =
                        dock->GetVisualPanelBounds(oldMouse);
                    const RECT newPanel =
                        dock->GetVisualPanelBounds(current);
                    const RECT oldTitle =
                        dock->GetHoveredTitleBounds(oldMouse);
                    const RECT newTitle =
                        dock->GetHoveredTitleBounds(current);
                    for (const RECT candidate : {
                            oldPanel, newPanel,
                            oldTitle, newTitle })
                    {
                        if (IsRectEmptyRect(candidate))
                            continue;
                        if (IsRectEmptyRect(dirty))
                            dirty = candidate;
                        else
                            UnionRect(&dirty, &dirty, &candidate);
                    }
                }
                if (!IsRectEmptyRect(dirty))
                    InflateRect(&dirty, 4, 4);
            }
            InvalidateRect(
                hwnd_,
                IsRectEmptyRect(dirty) ? nullptr : &dirty,
                FALSE);
            if (!marqueeActive_ &&
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
                        interactionPinnedWidgetId_ == w.id);
                const auto shouldRender = [&](bool pointerInside) {
                    return snowdesktop::widget_visibility_rules::
                        ShouldRenderWidget(
                            w.showOnHoverOnly,
                            dragSession_.IsActive(),
                            dragDropController_.IsExternalDragActive(),
                            widgetAction_ == WidgetAction::Move,
                            w.selected,
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
