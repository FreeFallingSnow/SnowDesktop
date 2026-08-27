#include "app.h"
#include "../quick_navigation_rules.h"
#include "../widget_scroll_rules.h"

// Primary-button press handling and drag-source initialization.

void DesktopApp::OnLeftButtonDown(WPARAM wp, LPARAM lp)
{
    dockPressedClosedCollectionPopup_ = false;
    if (middleButtonWidgetMove_) return;
    if (renameEdit_ != nullptr) return;
    keyboardNavVisualFocus_ = false;
    ClearPopupMouseDownItem();
    ClearPopupDragTarget();
    pendingGuideAction_ = WidgetHit::None;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelAnimation_.IsInteractive())
    {
        const RECT panel = GetLuaWidgetPanelRect();
        if (!PtInRect(&panel, pt))
        {
            if (luaWidgetPanelRequest_.dismissOnOutside)
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "outside");
            if (luaWidgetPanelRequest_.modal)
                return;
        }
        else
        {
            RestoreInteractionInputFocus();
            const RECT closeRect =
                GetLuaWidgetPanelCloseRect();
            if (PtInRect(&closeRect, pt))
            {
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "close");
                return;
            }
            const RECT content =
                GetLuaWidgetPanelContentRect();
            if (!PtInRect(&content, pt))
                return;
            const int localX =
                pt.x - content.left;
            const int localY =
                pt.y - content.top;
            const std::string& surface =
                luaWidgetPanelRequest_.surface;
            if (widgetEngine_ &&
                widgetEngine_->HasFocusedHostInput() &&
                !widgetEngine_->IsFocusedHostInputAt(
                    luaWidgetPanelRequest_.widgetId,
                    localX, localY, surface))
            {
                widgetEngine_->BlurHostInput(false);
            }
            mouseDown_ = true;
            mouseDownPoint_ = pt;
            luaWidgetPanelMouseDown_ = true;
            const HWND panelCaptureHost =
                handlingFloatingPopupInput_ &&
                    floatingPopupHwnd_ &&
                    IsWindow(floatingPopupHwnd_)
                ? floatingPopupHwnd_
                : hwnd_;
            luaWidgetPanelCaptureHwnd_ = nullptr;
            SetCapture(panelCaptureHost);
            if (GetCapture() == panelCaptureHost)
                luaWidgetPanelCaptureHwnd_ = panelCaptureHost;
            if (!widgetEngine_ ||
                !widgetEngine_->HandleHostUiPointer(
                    luaWidgetPanelRequest_.widgetId,
                    localX, localY, 0, false, surface))
            {
                if (widgetEngine_)
                {
                    const char* eventName = surface == "dialog"
                        ? "onDialogMouseDown"
                        : (surface == "popover"
                            ? "onPopoverMouseDown"
                            : "onPanelMouseDown");
                    widgetEngine_->InvokeMouseEvent(
                        luaWidgetPanelRequest_.widgetId,
                        eventName,
                        localX, localY, 1, 0);
                }
            }
            UpdateHostInputImePosition();
            InvalidateRect(
                hwnd_, &panel, FALSE);
            return;
        }
    }
    DockContainer* pointDock = GetDockContainerAtPoint(pt);
    PersistentDockHost* pointDockHost =
        FindPersistentDockHost(pointDock);
    const bool popupOwnedByPointDock =
        collectionPopupDockHost_ == pointDockHost;
    const bool pointInDock = pointDock != nullptr;
    size_t pressedDockCollectionWidgetIndex =
        static_cast<size_t>(-1);
    bool pressedOpenPopupFolderToggle = false;
    if (pointDock)
    {
        if (DockEntryItem* pressedDockItem =
                pointDock->EntryAtPoint(pt);
            pressedDockItem)
        {
            if (pressedDockItem->GetEntryType() ==
                    DockEntryType::Collection)
            {
                pressedDockCollectionWidgetIndex =
                    FindWidgetIndexById(
                        pressedDockItem->GetReference());
            }
            if (pressedDockItem->GetEntryIndex() <
                    dockEntries_.size() &&
                IsFolderDockEntry(dockEntries_[
                    pressedDockItem->GetEntryIndex()]))
            {
                const DockEntry& entry = dockEntries_[
                    pressedDockItem->GetEntryIndex()];
                const std::wstring sourceId =
                    std::to_wstring(
                        static_cast<int>(entry.type)) +
                    L":" + ToUpperInvariant(entry.reference);
                pressedOpenPopupFolderToggle =
                    IsCollectionPopupInteractive() &&
                    dockFolderPopupOpen_ &&
                    dockFolderPopupSourceId_ == sourceId &&
                    popupOwnedByPointDock;
            }
        }
    }
    const bool pressedOpenPopupDockToggle =
        IsCollectionPopupInteractive() &&
        snowdesktop::floating_dock_rules::
            ShouldCloseCollectionPopup(
                popupWidgetIndex_,
                pressedDockCollectionWidgetIndex,
                popupOwnedByPointDock);
    bool collectionPopupClosedByPointerDown = false;
    HWND interactionCaptureHwnd = hwnd_;
    if (handlingFloatingDockInput_ &&
        handlingPersistentDockHost_ &&
        handlingPersistentDockHost_->hwnd &&
        IsWindow(handlingPersistentDockHost_->hwnd))
        interactionCaptureHwnd =
            handlingPersistentDockHost_->hwnd;
    else if (handlingFloatingPopupInput_ &&
        floatingPopupHwnd_ &&
        IsWindow(floatingPopupHwnd_))
        interactionCaptureHwnd =
            floatingPopupHwnd_;
    dockPressedContainer_ = pointDock;
    // Dock is an app switcher: do not move focus away from the current app before
    // deciding whether this click should minimize or restore it. The decision
    // itself is captured from the indicator state during hit testing below.
    if (!pointInDock)
        RestoreInteractionInputFocus();
    if (widgetEngine_ && widgetEngine_->HasFocusedHostInput())
    {
        bool keepFocusedInput = false;
        for (size_t n = widgets_.size(); n > 0; --n)
        {
            const size_t wi = n - 1;
            if (widgets_[wi].type != DesktopWidgetType::LuaScript ||
                HitTestStandaloneWidget(wi, pt) != WidgetHit::Content)
                continue;
            const RECT frame =
                GetStandaloneWidgetFrameRect(widgets_[wi]);
            keepFocusedInput =
                widgetEngine_->IsFocusedHostInputAt(
                    widgets_[wi].id,
                    pt.x - frame.left,
                    pt.y - frame.top);
            break;
        }
        if (!keepFocusedInput)
        {
            widgetEngine_->BlurHostInput(false);
            UpdateHostInputImePosition();
        }
    }
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    dockFolderPopupMarqueeInitialSelection_.clear();
    marqueeAnchorPoint_ = pt;
    marqueeInitialScrollOffset_ = 0;
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    marqueeRect_ = MakeRect(pt.x, pt.y, pt.x, pt.y);

    // 外部点击先关闭集合弹窗，但保留本次按下事件，继续命中弹窗下方的真实目标。
    if (IsCollectionPopupInteractive() &&
        popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (snowdesktop::floating_dock_rules::
                ShouldCloseCollectionPopupOnPointerDown(
                    popupWidgetIndex_,
                    pressedDockCollectionWidgetIndex,
                    PtInRect(&popup, pt) != FALSE,
                    popupOwnedByPointDock))
        {
            CloseCollectionPopup();
            collectionPopupClosedByPointerDown = true;
        }
    }
    else if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_)
    {
        const RECT popup =
            GetCollectionPopupRect(
                dockFolderPopupWidget_);
        if (!PtInRect(&popup, pt) &&
            !pressedOpenPopupFolderToggle)
            CloseCollectionPopup();
    }

    const bool quickNavigationWasOpen =
        quickNavigationOpen_;
    if (HandleQuickNavigationClick(pt))
    {
        mouseDown_ = false;
        return;
    }
    const bool quickNavigationDismissedByThisPress =
        quickNavigationWasOpen &&
        !quickNavigationOpen_;

    if (HandlePageNavClick(pt)) return;

    bool ctrl = (wp & MK_CONTROL) != 0;

    if (IsCollectionPopupInteractive())
    {
        DesktopWidget* popupWidget = nullptr;
        bool pressedPopupToggle = false;
        if (dockFolderPopupOpen_)
        {
            popupWidget = &dockFolderPopupWidget_;
            pressedPopupToggle = pressedOpenPopupFolderToggle;
        }
        else if (popupWidgetIndex_ < widgets_.size())
        {
            popupWidget = &widgets_[popupWidgetIndex_];
            pressedPopupToggle = pressedOpenPopupDockToggle;
        }
        if (popupWidget && !pressedPopupToggle)
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
                    popupScrollOffset_);
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, pt.y, pt.x, viewport.right))
            {
                popupScrollbarDragging_ = true;
                popupScrollbarDragStartY_ = pt.y;
                popupScrollbarDragStartOffset_ = popupScrollOffset_;
                mouseDownHit_ = nullptr;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, &popup, FALSE);
                return;
            }
        }
    }

    if (IsCollectionPopupInteractive() &&
        dockFolderPopupOpen_ &&
        !pressedOpenPopupFolderToggle)
    {
        const RECT popup = GetCollectionPopupRect(
            dockFolderPopupWidget_);
        if (PtInRect(&popup, pt))
        {
            const RECT sortButton =
                GetDockFolderPopupSortButtonRect(
                    popup);
            if (PtInRect(&sortButton, pt))
            {
                mouseDown_ = false;
                mouseDownHit_ = nullptr;
                marqueeActive_ = false;
                marqueeDockFolderPopup_ = false;
                POINT screenPoint = pt;
                ClientToScreen(
                    hwnd_, &screenPoint);
                ShowDockFolderPopupSortMenu(
                    screenPoint);
                return;
            }

            const RECT content =
                GetCollectionPopupContentRect(popup);
            const RECT detailsHeader =
                GetCollectionPopupDetailsHeaderRect(popup);
            if (!IsRectEmptyRect(detailsHeader) &&
                PtInRect(&detailsHeader, pt))
            {
                mouseDownHit_ = nullptr;
                const auto divider =
                    HitTestCollectionPopupDetailsDivider(
                        pt, popup);
                if (divider != snowdesktop::list_detail_rules::
                        Column::None)
                {
                    detailColumnResizeActive_ = true;
                    detailColumnResizePopup_ = true;
                    detailColumnResizeColumn_ = divider;
                    detailColumnResizeHeaderLeft_ =
                        detailsHeader.left;
                    detailColumnResizeHeaderWidth_ =
                        std::max<int>(
                            1, detailsHeader.right -
                                detailsHeader.left);
                    mouseDownWidgetIndex_ =
                        static_cast<size_t>(-1);
                    SetCapture(interactionCaptureHwnd);
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                }
                else
                {
                    mouseDown_ = false;
                }
                return;
            }
            bool clickedPopupItem = false;
            for (size_t i = 0;
                 i < dockFolderPopupWidget_.
                    folderEntries.size(); ++i)
            {
                RECT itemRect =
                    GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top =
                    std::max(clipped.top, content.top);
                clipped.bottom =
                    std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top ||
                    !PtInRect(&clipped, pt))
                    continue;

                auto& entries =
                    dockFolderPopupWidget_.folderEntries;
                ClearSelection();
                if (ctrl)
                {
                    entries[i].selected =
                        !entries[i].selected;
                }
                else if (!entries[i].selected)
                {
                    for (auto& entry : entries)
                        entry.selected = false;
                    entries[i].selected = true;
                }
                popupMouseDownItem_ =
                    std::make_unique<FolderEntryIcon>(
                        &entries[i],
                        dockFolderPopupContainer_.get(),
                        this);
                popupMouseDownItem_->SetBounds(itemRect);
                mouseDownHit_ =
                    popupMouseDownItem_.get();
                clickedPopupItem = true;
                break;
            }
            const bool startPopupMarquee =
                snowdesktop::
                    collection_popup_layout::
                        AllowsMarqueeStart(
                            true,
                            clickedPopupItem,
                            false);
            if (!clickedPopupItem && !ctrl)
                for (auto& entry :
                     dockFolderPopupWidget_.folderEntries)
                    entry.selected = false;
            dockFolderPopupMarqueeInitialSelection_.
                clear();
            if (startPopupMarquee)
            {
                dockFolderPopupMarqueeInitialSelection_.
                    reserve(
                        dockFolderPopupWidget_.
                            folderEntries.size());
                for (const auto& entry :
                     dockFolderPopupWidget_.
                        folderEntries)
                {
                    dockFolderPopupMarqueeInitialSelection_.
                        push_back(entry.selected);
                }
                mouseDownHit_ = nullptr;
            }
            marqueeWidgetIndex_ =
                static_cast<size_t>(-1);
            marqueeDockFolderPopup_ =
                startPopupMarquee;
            marqueeInitialScrollOffset_ =
                popupScrollOffset_;
            mouseDownWidgetIndex_ =
                static_cast<size_t>(-1);
            SetCapture(interactionCaptureHwnd);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    if (IsCollectionPopupInteractive() &&
        popupWidgetIndex_ < widgets_.size() &&
        !pressedOpenPopupDockToggle)
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);

        std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
        RECT content = GetCollectionPopupContentRect(popup);
        const RECT detailsHeader =
            GetCollectionPopupDetailsHeaderRect(popup);
        if (!IsRectEmptyRect(detailsHeader) &&
            PtInRect(&detailsHeader, pt))
        {
            mouseDownHit_ = nullptr;
            const auto divider =
                HitTestCollectionPopupDetailsDivider(
                    pt, popup);
            if (divider != snowdesktop::list_detail_rules::
                    Column::None)
            {
                detailColumnResizeActive_ = true;
                detailColumnResizePopup_ = true;
                detailColumnResizeColumn_ = divider;
                detailColumnResizeHeaderLeft_ =
                    detailsHeader.left;
                detailColumnResizeHeaderWidth_ =
                    std::max<int>(
                        1, detailsHeader.right -
                            detailsHeader.left);
                mouseDownWidgetIndex_ =
                    static_cast<size_t>(-1);
                SetCapture(interactionCaptureHwnd);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            }
            else
            {
                mouseDown_ = false;
            }
            return;
        }
        bool clickedPopupItem = false;
        for (size_t i = 0; i < popupKeys.size(); ++i)
        {
            RECT itemRect = GetCollectionPopupItemRect(popup, i);
            RECT clipped = itemRect;
            clipped.top = std::max(clipped.top, content.top);
            clipped.bottom = std::min(clipped.bottom, content.bottom);
            if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex != static_cast<size_t>(-1))
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                    ToggleSelection(static_cast<int>(itemIndex));
                }
                else if (!items_[itemIndex].selected)
                {
                    SelectOnly(static_cast<int>(itemIndex));
                }
                else
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                }
                WidgetContainer* wc = nullptr;
                for (auto& c : containers_)
                {
                    wc = dynamic_cast<WidgetContainer*>(c.get());
                    if (wc && wc->GetWidgetData() == &widgets_[popupWidgetIndex_]) break;
                    wc = nullptr;
                }
                popupMouseDownItem_ = std::make_unique<DesktopIcon>(&items_[itemIndex], wc, this);
                popupMouseDownItem_->SetBounds(itemRect);
                mouseDownHit_ = popupMouseDownItem_.get();
                clickedPopupItem = true;
            }
            break;
        }
        if (!clickedPopupItem && !ctrl)
            ClearSelection();

        if (!clickedPopupItem)
            mouseDownHit_ = nullptr;
        marqueeWidgetIndex_ = popupWidgetIndex_;
        marqueeInitialScrollOffset_ = popupScrollOffset_;
        mouseDownWidgetIndex_ = popupWidgetIndex_;
        SetCapture(interactionCaptureHwnd);
        InvalidateRect(hwnd_, nullptr, FALSE);
        SyncKeyboardNavFromSelection();
        return;
    }

    // ── Dock hit-test（位于普通组件和桌面网格之上）──────────
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    if (DockContainer* dock = pointDock)
    {
        if (dock->ContainsInteractivePoint(pt))
        {
            const auto primeDockMinimizeSnapshot =
                [this, dock]() {
                    if (IsDockContainerPromoted(dock) ||
                        dockPressedWindowAction_ !=
                            snowdesktop::dock_window_rules::
                                DockClickAction::Minimize ||
                        !dockPressedTargetWindow_ ||
                        !dockWindowTransition_)
                        return;

                    // Paint the pressed state before snapshot capture blocks
                    // this UI thread, then overlap capture with the natural
                    // button-down/button-up interval.
                    UpdateWindow(hwnd_);
                    dockWindowTransition_->
                        PrimeMinimizeSnapshot(
                            dockPressedTargetWindow_);
                };
            if (dock->IsWindowsButtonPoint(pt))
            {
                mouseDown_ = false;
                CloseFloatingDockThen(
                    [this]() {
                        ToggleWindowsStartMenu();
                    },
                    FloatingDockCloseFocusPolicy::PreserveCurrent);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (dock->IsSearchPoint(pt))
            {
                mouseDown_ = false;
                if (snowdesktop::quick_navigation_rules::
                        ShouldOpenFromDockSearchPress(
                            quickNavigationDismissedByThisPress))
                {
                    OpenQuickNavigation(
                        QuickNavigationInvocationSource::DockSearch);
                }
                return;
            }
            if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                dockItem->SetSelected(true);
                dockPressedEntry_ = dockItem->GetEntryIndex();
                if (dockPressedEntry_ < dockEntries_.size() &&
                    dockEntries_[dockPressedEntry_].type ==
                        DockEntryType::Collection)
                {
                    dockPressedClosedCollectionPopup_ =
                        collectionPopupClosedByPointerDown;
                }
                if (dockPressedEntry_ < dockEntries_.size() &&
                    dockEntries_[dockPressedEntry_].type ==
                        DockEntryType::DesktopItem)
                {
                    const size_t itemIndex = FindItemIndexByKey(
                        dockEntries_[dockPressedEntry_].reference);
                    if (itemIndex < items_.size())
                    {
                        const DockWindowVisualState state =
                            GetDockWindowVisualState(itemIndex);
                        dockPressedWindowAction_ =
                            snowdesktop::dock_window_rules::
                                ResolveDockClickAction(
                                    state != DockWindowVisualState::Closed,
                                    state == DockWindowVisualState::Minimized,
                                    state == DockWindowVisualState::Foreground);
                        const auto running = dockRunningWindows_.find(
                            DockItemWindowKey(items_[itemIndex]));
                        if (running != dockRunningWindows_.end() &&
                            IsWindow(running->second.window))
                            dockPressedTargetWindow_ =
                                running->second.window;
                    }
                }
                mouseDownHit_ = dockItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                primeDockMinimizeSnapshot();
                return;
            }
            if (DockRunningItem* runningItem = dock->RunningItemAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                runningItem->SetSelected(true);
                dockPressedRunningAppKey_ = runningItem->GetIdentityKey();
                const auto running = std::find_if(
                    dockUnpinnedRunningApps_.begin(),
                    dockUnpinnedRunningApps_.end(),
                    [&](const DockRunningAppInfo& app) {
                        return app.identityKey ==
                            dockPressedRunningAppKey_;
                    });
                if (running != dockUnpinnedRunningApps_.end())
                {
                    const bool hasLiveWindow =
                        IsWindow(running->window) != FALSE;
                    const bool minimized =
                        hasLiveWindow
                        ? IsIconic(running->window) != FALSE
                        : running->minimized;
                    dockPressedWindowAction_ =
                        snowdesktop::dock_window_rules::
                            ResolveDockClickAction(
                                true, minimized,
                                running->foreground);
                    if (hasLiveWindow)
                        dockPressedTargetWindow_ =
                            running->window;
                }
                mouseDownHit_ = runningItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                primeDockMinimizeSnapshot();
                return;
            }
            if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                frequentItem->SetSelected(true);
                dockPressedFrequentItem_ = frequentItem->GetItemIndex();
                if (dockPressedFrequentItem_ < items_.size())
                {
                    const DockWindowVisualState state =
                        GetDockWindowVisualState(
                            dockPressedFrequentItem_);
                    dockPressedWindowAction_ =
                        snowdesktop::dock_window_rules::
                            ResolveDockClickAction(
                                state != DockWindowVisualState::Closed,
                                state == DockWindowVisualState::Minimized,
                                state == DockWindowVisualState::Foreground);
                    const auto running = dockRunningWindows_.find(
                        DockItemWindowKey(
                            items_[dockPressedFrequentItem_]));
                    if (running != dockRunningWindows_.end() &&
                        IsWindow(running->second.window))
                        dockPressedTargetWindow_ =
                            running->second.window;
                }
                mouseDownHit_ = frequentItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                primeDockMinimizeSnapshot();
                return;
            }
            if (!ctrl) ClearSelection();
            mouseDown_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // ── Widget hit-test ─────────────────────────────────────
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    widgetAction_ = WidgetAction::None;
    widgetCollectionGroupTargetIndex_ = static_cast<size_t>(-1);
    widgetCollectionGroupInsertIndex_ = static_cast<size_t>(-1);

    // Defocus search box when clicking outside all search boxes
    {
        bool clickedSearchBox = false;
        for (auto& c : containers_)
        {
            if (desktopIconsHidden_ &&
                !IsRetainedContainer(c.get()))
                continue;
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (!searchable) continue;
            RECT sr = searchable->GetSearchBoxRect();
            if (!IsRectEmptyRect(sr) && PtInRect(&sr, pt))
            {
                clickedSearchBox = true;
                break;
            }
        }
        if (!clickedSearchBox)
        {
            for (auto& c : containers_)
            {
                auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (searchable && searchable->IsSearchFocused())
                {
                    searchable->SetSearchFocused(false);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
        }
    }

    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t wi = n - 1;
        WidgetHit wh = HitTestStandaloneWidget(wi, pt);
        if (wh == WidgetHit::None) continue;

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        bool hostInputArea = false;
        RECT luaFrame{};
        if (widgetEngine_ &&
            widgets_[wi].type == DesktopWidgetType::LuaScript)
        {
            luaFrame = GetStandaloneWidgetFrameRect(widgets_[wi]);
            widgetEngine_->EnsureWidgetLoaded(
                widgets_[wi].id, widgets_[wi].packageId);
            hostInputArea = widgetEngine_->IsHostInputAt(
                widgets_[wi].id, pt.x - luaFrame.left,
                pt.y - luaFrame.top);
        }

        // An input field owns its pointer interaction. Do not select the
        // containing Lua widget here, otherwise the outer blue selection
        // frame appears on top of the field and disrupts live editing.
        if (hostInputArea)
            ClearSelection();
        else
            SelectWidgetOnly(wi);
        mouseDownWidgetIndex_ = wi;
        mouseDownHit_ = nullptr;
        SetCapture(hwnd_);
        if (widgetEngine_ && widgets_[wi].type == DesktopWidgetType::LuaScript)
        {
            if (IsRectEmptyRect(luaFrame))
                luaFrame = GetStandaloneWidgetFrameRect(widgets_[wi]);
            int localX = pt.x - luaFrame.left;
            int localY = pt.y - luaFrame.top;
            if (!widgetEngine_->HandleHostUiPointer(
                    widgets_[wi].id, localX, localY, 0, false))
                widgetEngine_->InvokeMouseEvent(widgets_[wi].id, "onMouseDown",
                    localX, localY, 1, 0);
            else
                UpdateHostInputImePosition();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        if (desktopIconsHidden_ &&
            !widgets_[wi].keepWhenDesktopHidden)
            continue;
        WidgetContainer* wc = nullptr;
        for (auto& c : containers_)
        {
            wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[wi]) break;
            wc = nullptr;
        }
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::None) continue;

        if (wh != WidgetHit::ResizeHandle &&
            wh != WidgetHit::MoveHandle)
        {
            const int maximum = wc->GetMaxScrollOffset();
            const int visible = wc->GetVisibleContentHeight();
            const RECT viewport = wc->GetContentViewportRect();
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewport.top, viewport.bottom,
                    visible + maximum, visible,
                    wc->GetScrollOffset(), wc->GetCellScale());
            if (snowdesktop::widget_scroll_rules::ScrollbarThumbHit(
                    geometry, pt.y, pt.x, viewport.right,
                    wc->GetCellScale()))
            {
                widgetScrollbarDragging_ = true;
                widgetScrollbarDragContainer_ = wc;
                widgetScrollbarDragStartY_ = pt.y;
                widgetScrollbarDragStartOffset_ =
                    wc->GetScrollOffset();
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ = nullptr;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, &widgets_[wi].bounds, FALSE);
                return;
            }
        }

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::GuideAddWidgetBtn)
        {
            pendingGuideAction_ = wh;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, &widgets_[wi].bounds, FALSE);
            return;
        }
        else if (wh == WidgetHit::GuideDetailsBtn)
        {
            pendingGuideAction_ = wh;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, &widgets_[wi].bounds, FALSE);
            return;
        }
        else if (wh == WidgetHit::DetailsModifiedDivider ||
                 wh == WidgetHit::DetailsTypeDivider ||
                 wh == WidgetHit::DetailsSizeDivider)
        {
            auto* list = dynamic_cast<ScrollingItemWidget*>(wc);
            if (!list) return;
            const RECT header = list->GetDetailsHeaderRectFromViewport(
                list->GetContentViewportRect());
            if (IsRectEmptyRect(header)) return;
            detailColumnResizeActive_ = true;
            detailColumnResizePopup_ = false;
            if (wh == WidgetHit::DetailsModifiedDivider)
            {
                detailColumnResizeColumn_ =
                    snowdesktop::list_detail_rules::Column::Modified;
            }
            else if (wh == WidgetHit::DetailsTypeDivider)
            {
                detailColumnResizeColumn_ =
                    snowdesktop::list_detail_rules::Column::Type;
            }
            else
            {
                detailColumnResizeColumn_ =
                    snowdesktop::list_detail_rules::Column::Size;
            }
            detailColumnResizeHeaderLeft_ = header.left;
            detailColumnResizeHeaderWidth_ = std::max<int>(
                1, header.right - header.left);
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return;
        }
        else if (wh == WidgetHit::DetailsNameHeader ||
                 wh == WidgetHit::DetailsModifiedHeader ||
                 wh == WidgetHit::DetailsTypeHeader ||
                 wh == WidgetHit::DetailsSizeHeader)
        {
            auto* list = dynamic_cast<ScrollingItemWidget*>(wc);
            if (!list) return;
            auto column = snowdesktop::list_detail_rules::Column::Name;
            int mode = snowdesktop::folder_sort_rules::kName;
            if (wh == WidgetHit::DetailsModifiedHeader)
            {
                column = snowdesktop::list_detail_rules::Column::Modified;
                mode = snowdesktop::folder_sort_rules::kModified;
            }
            else if (wh == WidgetHit::DetailsTypeHeader)
            {
                column = snowdesktop::list_detail_rules::Column::Type;
                mode = snowdesktop::folder_sort_rules::kType;
            }
            else if (wh == WidgetHit::DetailsSizeHeader)
            {
                column = snowdesktop::list_detail_rules::Column::Size;
                mode = snowdesktop::folder_sort_rules::kSize;
            }
            const DesktopWidget* sortData = list->GetDetailsSortData();
            const bool ascending = sortData &&
                sortData->contentSortColumn == column
                ? !sortData->contentSortAscending
                : snowdesktop::list_detail_rules::
                    DefaultAscending(column);
            SortWidgetContents(wi, mode, ascending);
            return;
        }
        else if (wh == WidgetHit::Content)
        {
            Item* memberItem = nullptr;
            RECT bodyRect = wc->GetBodyRect();
            auto& slots = wc->GetSlots();
            for (auto& slot : slots)
            {
                RECT bounds = slot->GetBounds();
                if (PtInRect(&bounds, pt) && PtInRect(&bodyRect, pt))
                {
                    memberItem = slot->GetItem();
                    break;
                }
            }

            if (memberItem)
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(wi);
                    if (memberItem->IsSelected())
                        pendingCtrlToggleWidgetItem_ = memberItem;
                    else
                        memberItem->SetSelected(true);
                }
                else if (!memberItem->IsSelected())
                {
                    ClearSelection();
                    memberItem->SetSelected(true);
                }
                else
                {
                    ClearSelectionOutsideWidget(wi);
                }
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ = memberItem;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                SyncKeyboardNavFromSelection();
                return;
            }

            // Empty content selects the widget itself.
            ClearSelection();
            widgets_[wi].selected = true;
            mouseDownWidgetIndex_ = wi;
            marqueeWidgetIndex_ = wi;
            marqueeInitialScrollOffset_ = wc->GetScrollOffset();
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::CollectionOpenBtn)
        {
            SelectWidgetOnly(wi);
            OpenCollectionPopupAt(
                wi, pt, L"",
                collectionPopupClosedByPointerDown);
            mouseDown_ = false;
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            mouseDownHit_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::ListToggleBtn)
        {
            if (widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::Collection ||
                widgets_[wi].type == DesktopWidgetType::CollectionGroup ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                widgets_[wi].listMode = !widgets_[wi].listMode;
                if (auto* group =
                        dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    wc->InvalidateSlots();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::DateHeaderToggleBtn)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                widgets_[wi].dateHeaders = !widgets_[wi].dateHeaders;
                widgets_[wi].scrollOffset = 0;
                if (auto* mapping = dynamic_cast<FolderMapping*>(wc))
                    mapping->InvalidateFilterCache();
                else if (auto* group =
                             dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    RebuildContainersAndItems();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::OpenFolderBtn)
        {
            DesktopWidget* folder = &widgets_[wi];
            if (auto* group = dynamic_cast<FileGroup*>(wc))
            {
                const size_t activeIndex =
                    FindWidgetIndexById(
                        group->GetActiveSourceId());
                folder = activeIndex < widgets_.size()
                    ? &widgets_[activeIndex] : nullptr;
            }
            if (folder &&
                folder->type ==
                    DesktopWidgetType::FolderMapping &&
                !folder->sourceFolderPath.empty())
            {
                shellLaunchWorker_.Enqueue(
                    hwnd_, folder->sourceFolderPath);
            }
            return;
        }
        else if (wh == WidgetHit::SourceTab)
        {
            auto* group = dynamic_cast<FileGroup*>(wc);
            const std::wstring id = group
                ? group->SourceIdAtPoint(pt) : L"";
            if (!id.empty())
            {
                widgets_[wi].activeCategoryId = id;
                widgets_[wi].scrollOffset = 0;
                group->InvalidateHostedView();
                ClearSelection();
                const size_t childIndex =
                    FindWidgetIndexById(id);
                if (childIndex < widgets_.size())
                    widgets_[childIndex].selected = true;
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ =
                    group->GetSourceTabItemAtPoint(pt);
                if (mouseDownHit_)
                {
                    SetCapture(hwnd_);
                    SyncKeyboardNavFromSelection();
                }
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::CategoryTab)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::CollectionGroup ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                auto* categorized = dynamic_cast<ScrollingItemWidget*>(wc);
                std::wstring id = categorized
                    ? categorized->CategoryIdAtPoint(pt)
                    : L"";
                if (!id.empty())
                {
                    DesktopWidget* categorizedData =
                        &widgets_[wi];
                    if (auto* fileGroup =
                            dynamic_cast<FileGroup*>(wc))
                    {
                        if (auto* active =
                                fileGroup->
                                    GetActiveSourceContainer())
                            categorizedData =
                                active->GetWidgetData();
                    }
                    if (!categorizedData) return;
                    categorizedData->activeCategoryId = id;
                    widgets_[wi].scrollOffset = 0;
                    if (auto* group =
                        dynamic_cast<CollectionGroup*>(wc))
                    {
                        group->InvalidateFilterCache();
                        ClearSelection();
                        const size_t childIndex =
                            FindWidgetIndexById(id);
                        if (childIndex < widgets_.size())
                            widgets_[childIndex].selected = true;
                        mouseDownWidgetIndex_ = wi;
                        mouseDownHit_ =
                            group->GetTabItemAtPoint(pt);
                        if (mouseDownHit_)
                        {
                            SetCapture(hwnd_);
                            SyncKeyboardNavFromSelection();
                        }
                    }
                    else
                    {
                        if (auto* fileGroup =
                                dynamic_cast<FileGroup*>(wc))
                            fileGroup->
                                InvalidateHostedView();
                        else
                            wc->InvalidateSlots();
                    }
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            return;
        }
        else if (wh == WidgetHit::SearchBox)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(wc);
            for (auto& c : containers_)
            {
                auto* other = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (other && other != searchable)
                    other->SetSearchFocused(false);
            }
            if (searchable)
            {
                // Search focus belongs to the widget interaction, not merely
                // to the shared hidden IME input HWND. Keep the owning widget
                // selected so a hover-only widget remains visible while the
                // pointer temporarily enters the IME candidate window.
                SelectWidgetOnly(wi);
                searchable->BeginSearchPointerSelection(
                    pt, (wp & MK_SHIFT) != 0);
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ = nullptr;
                SetCapture(hwnd_);
                UpdateHostInputImePosition();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // ── Desktop icon hit-test ───────────────────────────────
    DesktopIcon* hit = HitTestIcon(pt);
    mouseDownHit_ = hit;

    // Defocus search when clicking on desktop area
    if (hit || mouseDownWidgetIndex_ == static_cast<size_t>(-1))
    {
        for (auto& c : containers_)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (searchable && searchable->IsSearchFocused())
            {
                searchable->SetSearchFocused(false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }

    // Clear widget selection when clicking desktop area
    if (!hit && mouseDownWidgetIndex_ == static_cast<size_t>(-1) && !ctrl)
    {
        for (auto& w : widgets_) w.selected = false;
    }

    if (hit)
    {
        DesktopItem* di = hit->GetDesktopItem();
        if (ctrl)
        {
            ClearSelectionOutsideDesktop();
            size_t hitIndex = (di && !di->layoutKey.empty())
                ? FindItemIndexByKey(di->layoutKey)
                : static_cast<size_t>(-1);
            if (hit->IsSelected())
                pendingCtrlToggleDesktopIndex_ = hitIndex;
            else
                hit->SetSelected(true);
        }
        else if (!di->selected)
        {
            ClearSelection();
            hit->SetSelected(true);
        }
        else
        {
            ClearSelectionOutsideDesktop();
        }
    }
    else if (!ctrl)
        ClearSelection();

    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
    SyncKeyboardNavFromSelection();
}
