#include "app.h"
#include "../desktop_keyboard_rules.h"
#include "../drag_input_rules.h"

#include <imm.h>
#include <shldisp.h>

// Main desktop-window message dispatch.

bool DesktopApp::RequestWindowsShutdownDialog()
{
    ComPtr<IShellDispatch> shell;
    HRESULT result = CoCreateInstance(
        CLSID_Shell, nullptr,
        CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&shell));
    if (SUCCEEDED(result))
        result = shell->ShutdownWindows();
    if (FAILED(result))
    {
        wchar_t message[160]{};
        wsprintfW(message,
            L"Desktop Alt+F4 shutdown dialog request failed hr=0x%08X",
            static_cast<unsigned>(result));
        WriteDiagnosticLogEntry(message);
        return false;
    }

    WriteDiagnosticLogEntry(
        L"Desktop Alt+F4 requested the Windows shutdown dialog");
    return true;
}

bool DesktopApp::HandleShellContextMenuMessage(
    UINT message, WPARAM wParam, LPARAM lParam,
    LRESULT& result)
{
    const bool supportsContextMenu2 =
        message == WM_INITMENUPOPUP ||
        message == WM_DRAWITEM ||
        message == WM_MEASUREITEM;
    const bool supportsContextMenu3 =
        supportsContextMenu2 || message == WM_MENUCHAR;
    if (!supportsContextMenu3)
        return false;

    if (newMenuContextMenu_ && supportsContextMenu2 &&
        newMenuContextMenu_->HandleMenuMsg(
            message, wParam, lParam) == S_OK)
    {
        result = 0;
        return true;
    }
    if (activeContextMenu3_ &&
        activeContextMenu3_->HandleMenuMsg2(
            message, wParam, lParam, &result) == S_OK)
    {
        return true;
    }
    if (activeContextMenu2_ && supportsContextMenu2 &&
        activeContextMenu2_->HandleMenuMsg(
            message, wParam, lParam) == S_OK)
    {
        result = 0;
        return true;
    }
    return false;
}

LRESULT DesktopApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct NativeMenuPresentationScope final
    {
        DesktopApp& app;
        ~NativeMenuPresentationScope()
        {
            app.FlushNativeMenuPresentation();
        }
    } nativeMenuPresentationScope{ *this };

    LRESULT shellMenuResult = 0;
    if (HandleShellContextMenuMessage(
            msg, wp, lp, shellMenuResult))
        return shellMenuResult;

    switch (msg)
    {
    case WM_GETOBJECT:
    {
        LRESULT accessibilityResult = 0;
        if (widgetAccessibilityProvider_ &&
            widgetAccessibilityProvider_->TryHandleGetObject(
                hwnd, wp, lp, accessibilityResult))
            return accessibilityResult;
        break;
    }
    case WM_NCHITTEST:
        // The desktop overlay intentionally owns input across its complete
        // client area. Be explicit for the resized portion of the layered
        // child window after a monitor is added at runtime.
        return HTCLIENT;
    case WM_MOUSEACTIVATE:
    {
        POINT point{};
        if (GetCursorPos(&point))
        {
            ScreenToClient(hwnd_, &point);
            if (DockContainer* dock = GetDockContainerAtPoint(point))
            {
                if (dock->ContainsInteractivePoint(point))
                    return MA_NOACTIVATE;
            }
        }
        break;
    }
    case WM_SETCURSOR:
    {
        if (LOWORD(lp) != HTCLIENT) break;
        bool resizeCursor = detailColumnResizeActive_;
        bool cursorPointAvailable = false;
        bool pointInsideCollectionPopup = false;
        POINT point{};
        if (!resizeCursor && GetCursorPos(&point) &&
            ScreenToClient(hwnd_, &point))
        {
            cursorPointAvailable = true;
            if (IsCollectionPopupInteractive())
            {
                if (const DesktopWidget* popupWidget =
                        GetOpenPopupWidget())
                {
                    const RECT popup =
                        GetCollectionPopupRect(*popupWidget);
                    pointInsideCollectionPopup =
                        PtInRect(&popup, point) != FALSE;
                    resizeCursor =
                        HitTestCollectionPopupDetailsDivider(
                            point, popup) !=
                        snowdesktop::list_detail_rules::
                            Column::None;
                }
            }
            for (auto it = containers_.rbegin();
                 !resizeCursor &&
                    !pointInsideCollectionPopup &&
                    it != containers_.rend(); ++it)
            {
                if (desktopIconsHidden_ &&
                    !IsRetainedContainer(it->get()))
                    continue;
                auto* widget =
                    dynamic_cast<WidgetContainer*>(it->get());
                if (!widget) continue;
                const WidgetHit hit = widget->HitTestWidget(point);
                resizeCursor =
                    hit == WidgetHit::DetailsModifiedDivider ||
                    hit == WidgetHit::DetailsTypeDivider ||
                    hit == WidgetHit::DetailsSizeDivider;
                if (resizeCursor || hit != WidgetHit::None) break;
            }
        }
        if (resizeCursor)
        {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (widgetEngine_ && cursorPointAvailable)
        {
            if (!luaWidgetPanelRequest_.widgetId.empty() &&
                luaWidgetPanelAnimation_.IsInteractive())
            {
                const RECT content = GetLuaWidgetPanelContentRect();
                if (PtInRect(&content, point))
                {
                    const std::string cursor =
                        widgetEngine_->InteractionCursorAt(
                            luaWidgetPanelRequest_.widgetId,
                            point.x - content.left,
                            point.y - content.top,
                            luaWidgetPanelRequest_.surface);
                    LPCWSTR cursorId = nullptr;
                    if (cursor == "hand") cursorId = IDC_HAND;
                    else if (cursor == "text") cursorId = IDC_IBEAM;
                    else if (cursor == "crosshair") cursorId = IDC_CROSS;
                    if (cursorId)
                    {
                        SetCursor(LoadCursorW(nullptr, cursorId));
                        return TRUE;
                    }
                }
            }
            const size_t widgetIndex =
                HitTestStandaloneWidgetIndex(point);
            if (widgetIndex < widgets_.size() &&
                widgets_[widgetIndex].type ==
                    DesktopWidgetType::LuaScript &&
                HitTestStandaloneWidget(widgetIndex, point) ==
                    WidgetHit::Content)
            {
                const RECT frame = GetStandaloneWidgetFrameRect(
                    widgets_[widgetIndex]);
                const std::string cursor =
                    widgetEngine_->InteractionCursorAt(
                        widgets_[widgetIndex].id,
                        point.x - frame.left,
                        point.y - frame.top);
                LPCWSTR cursorId = nullptr;
                if (cursor == "hand") cursorId = IDC_HAND;
                else if (cursor == "text") cursorId = IDC_IBEAM;
                else if (cursor == "crosshair") cursorId = IDC_CROSS;
                if (cursorId)
                {
                    SetCursor(LoadCursorW(nullptr, cursorId));
                    return TRUE;
                }
            }
        }
        break;
    }
    case WM_CTLCOLOREDIT:
        if (reinterpret_cast<HWND>(lp) == luaInlineEdit_)
        {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, luaInlineEditTextColor_);
            SetBkColor(dc, luaInlineEditBackgroundColor_);
            return reinterpret_cast<LRESULT>(luaInlineEditBackgroundBrush_
                ? luaInlineEditBackgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        EndPaint(hwnd_, &ps);
        OnPaint(&ps.rcPaint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
    {
        if (updatingDisplayTopology_)
        {
            virtualWidth_ = LOWORD(lp);
            virtualHeight_ = HIWORD(lp);
            return 0;
        }
        bool wasDragging = dragSession_.IsActive();
        virtualWidth_ = LOWORD(lp);
        virtualHeight_ = HIWORD(lp);
        ResetDesktopWidgetComposition();
        dcompSurface_.Reset();
        UpdateLayoutWorkArea();
        LayoutItems();
        if (wasDragging && !dragSession_.IsActive())
        {
            mouseDownHit_ = nullptr;
            mouseDown_ = false;
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt))
        {
            ShowHiddenHint();
            return 0;
        }
        OnLeftButtonDown(wp, lp);
        return 0;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt))
            return 0;
        OnMiddleButtonDown(wp, lp);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const bool nativeDragActive =
            snowdesktop::drag_input_rules::IsNativeDragActive(
                dragSession_.IsActive(),
                dragDropController_.IsTransportActive());
        const bool marqueePointerActive =
            IsMarqueePointerGesturePendingOrActive();
        const bool widgetActionActive =
            widgetAction_ != WidgetAction::None;
        const bool latencySensitivePointerActive =
            snowdesktop::drag_input_rules::
                IsLatencySensitivePointerGesture(
                    nativeDragActive,
                    marqueePointerActive,
                    widgetActionActive,
                    mouseDownWidgetIndex_ < widgets_.size());
        const bool primaryButtonDown =
            latencySensitivePointerActive &&
            !middleButtonWidgetMove_ &&
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool middleButtonDown =
            latencySensitivePointerActive &&
            middleButtonWidgetMove_ &&
            (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        const bool gestureButtonDown =
            snowdesktop::drag_input_rules::
                IsPointerGestureButtonDown(
                    middleButtonWidgetMove_,
                    primaryButtonDown,
                    middleButtonDown);
        const bool sampleLivePointer =
            snowdesktop::drag_input_rules::ShouldSampleLivePointer(
                latencySensitivePointerActive, gestureButtonDown);
        const bool widgetInteractionActive =
            middleButtonWidgetMove_ ||
            widgetActionActive ||
            detailColumnResizeActive_ ||
            luaWidgetPanelMouseDown_;
        const bool samplePassiveHover =
            snowdesktop::desktop_hover_rules::
                ShouldResamplePassiveMouseMove(
                    mouseDown_,
                    dragSession_.IsActive(),
                    widgetInteractionActive);
        if (sampleLivePointer || samplePassiveHover)
        {
            // Costly frames and modal Shell loops can leave old WM_MOUSEMOVE
            // messages queued for this HWND. Native item drags, marquee
            // selection, and widget move/resize must follow the physical
            // pointer; passive hover additionally verifies that the sample
            // still belongs to the paired desktop surface.
            POINT cursorScreen{};
            if (!GetCursorPos(&cursorScreen))
            {
                // A captured drag, marquee, or widget gesture must keep making
                // progress even if the live sample fails transiently. Passive
                // hover has no equivalent gesture state, so retain its
                // drop-on-failure rule.
                if (samplePassiveHover)
                    return 0;
            }
            else
            {
                if (samplePassiveHover)
                {
                    const HWND hitWindow =
                        WindowFromPoint(cursorScreen);
                    const bool pointerOnContentWindow =
                        IsSameWindowTree(hwnd_, hitWindow);
                    const bool pointerOnPairedBackdropWindow =
                        desktopBackdropCompositor_.
                            IsBackdropWindow(hitWindow);
                    if (!snowdesktop::desktop_hover_rules::
                            ShouldRetainHoverAcrossMouseLeave(
                                pointerOnContentWindow,
                                pointerOnPairedBackdropWindow))
                    {
                        if (lastMousePoint_.x != LONG_MIN ||
                            lastMousePoint_.y != LONG_MIN)
                        {
                            OnMouseLeave();
                        }
                        return 0;
                    }
                }
                POINT sampledClient = cursorScreen;
                if (!ScreenToClient(hwnd_, &sampledClient))
                {
                    if (samplePassiveHover)
                        return 0;
                }
                else
                    pt = sampledClient;
            }
        }
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt) &&
            GetCapture() != hwnd_ && !mouseDown_ &&
            !middleButtonWidgetMove_ && !dragSession_.IsActive() &&
            widgetAction_ == WidgetAction::None &&
            !luaWidgetPanelMouseDown_)
        {
            if (IsPointOnRetainedElement(lastMousePoint_))
                OnMouseLeave();
            return 0;
        }
        bool dragPreviewSynced = false;
        OnMouseMoveAt(wp, pt, &dragPreviewSynced);
        // Internal drags capture this HWND. Commit the cheap cached drag frame
        // synchronously so a dense WM_MOUSEMOVE queue cannot starve WM_PAINT.
        // Keep this synchronous; routing pointer feedback through
        // UiAnimationScheduler makes drag/Dock hover trail the pointer
        // (f29a882 regression).
        PresentPointerInteractionFrame(dragPreviewSynced);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // The native backdrop is a sibling HWND behind the D2D content HWND.
        // Updating its region when a hover-only glass widget appears can make
        // TrackMouseEvent report a leave even though the pointer is still on
        // the same logical desktop surface. Clearing hover here removes that
        // region again and creates a show/leave/hide/move feedback loop.
        // Re-sample only the paired desktop content/backdrop windows before
        // accepting the leave; external dialogs and unrelated SnowDesktop
        // popups remain genuine leave targets.
        POINT cursorScreen{};
        if (GetCursorPos(&cursorScreen))
        {
            const HWND hitWindow =
                WindowFromPoint(cursorScreen);
            const bool pointerOnContentWindow =
                IsSameWindowTree(hwnd_, hitWindow);
            const bool pointerOnPairedBackdropWindow =
                desktopBackdropCompositor_.
                    IsBackdropWindow(hitWindow);
            if (snowdesktop::desktop_hover_rules::
                    ShouldRetainHoverAcrossMouseLeave(
                        pointerOnContentWindow,
                        pointerOnPairedBackdropWindow))
            {
                POINT cursorClient = cursorScreen;
                if (ScreenToClient(
                        hwnd_, &cursorClient))
                {
                    const bool pointerPositionChanged =
                        cursorClient.x != lastMousePoint_.x ||
                        cursorClient.y != lastMousePoint_.y;
                    const bool widgetInteractionActive =
                        middleButtonWidgetMove_ ||
                        widgetAction_ != WidgetAction::None ||
                        detailColumnResizeActive_ ||
                        luaWidgetPanelMouseDown_;
                    const bool passiveHover =
                        snowdesktop::desktop_hover_rules::
                            ShouldResamplePassiveMouseMove(
                                mouseDown_,
                                dragSession_.IsActive(),
                                widgetInteractionActive);
                    lastMousePoint_ = cursorClient;
                    if (snowdesktop::desktop_hover_rules::
                            ShouldPresentRetainedMouseLeave(
                                passiveHover,
                                pointerPositionChanged))
                        PresentPassiveHoverVisualChange();
                }
                return 0;
            }
        }
        if (floatingDockHoverHandoffPending_)
        {
            POINT cursorPoint{};
            if (TryGetDesktopHoverPointFromCursor(cursorPoint) &&
                PtInRect(
                    &floatingDockHoverHandoffRect_,
                    cursorPoint))
            {
                lastMousePoint_ = cursorPoint;
                return 0;
            }
            floatingDockHoverHandoffPending_ = false;
            floatingDockHoverHandoffRect_ = {};
        }
        if (floatingDockVisible_)
        {
            POINT cursor{};
            if (GetCursorPos(&cursor))
            {
                ScreenToClient(hwnd_, &cursor);
                if (IsPointInPromotedDockLayer(cursor))
                    return 0;
            }
        }
        OnMouseLeave();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt) &&
            GetCapture() != hwnd_ && !mouseDown_ &&
            !dragSession_.IsActive() &&
            widgetAction_ == WidgetAction::None &&
            !luaWidgetPanelMouseDown_)
            return 0;
        OnLeftButtonUpAt(wp, pt);
        InvalidateFloatingDockWindow(true);
        return 0;
    }
    case WM_MBUTTONUP:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt) &&
            GetCapture() != hwnd_ && !mouseDown_ &&
            !middleButtonWidgetMove_ &&
            widgetAction_ == WidgetAction::None)
            return 0;
        OnMiddleButtonUpAt(wp, pt);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        POINT wheelPt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_)
        {
            ScreenToClient(hwnd_, &wheelPt);
            if (!IsPointOnRetainedElement(wheelPt))
                return 0;
        }
        OnMouseWheel(wp, lp);
        return 0;
    }
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        OnRightButtonDown(nullptr);
        return 0;
    case WM_RBUTTONUP:
    {
        const POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt))
        {
            ShowHiddenHint();
            return 0;
        }
        OnRightButtonUp(lp);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        const auto clearSelectionAfterAcceptedOpen =
            [this](bool accepted) {
                if (!accepted)
                    return;
                ClearSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
            };

        if (!luaWidgetPanelRequest_.widgetId.empty() &&
            luaWidgetPanelAnimation_.IsInteractive())
        {
            const RECT content = GetLuaWidgetPanelContentRect();
            if (PtInRect(&content, pt) && widgetEngine_)
            {
                const std::string& surface =
                    luaWidgetPanelRequest_.surface;
                const char* eventName = surface == "dialog"
                    ? "onDialogDoubleClick"
                    : (surface == "popover"
                        ? "onPopoverDoubleClick"
                        : "onPanelDoubleClick");
                widgetEngine_->InvokeMouseEvent(
                    luaWidgetPanelRequest_.widgetId,
                    eventName,
                    pt.x - content.left,
                    pt.y - content.top, 1, 0);
                return 0;
            }
            if (luaWidgetPanelRequest_.modal)
                return 0;
        }

        if (desktopIconsHidden_ && !IsPointOnRetainedElement(pt))
        {
            ToggleDesktopIconsVisibility();
            return 0;
        }

        if (quickNavigationOpen_)
        {
            if (HandleQuickNavigationClick(pt))
                return 0;
        }

        if (DockContainer* dock = GetDockContainerAtPoint(pt))
        {
            RECT dockBounds = dock->GetInteractiveBounds();
            if (dock->ContainsInteractivePoint(pt))
            {
                if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
                {
                    const DWORD elapsed = GetTickCount() - dockPendingDoubleClickTick_;
                    const size_t entryIndex =
                        dockItem->GetEntryIndex();
                    bool specialDoubleClickHandled = false;
                    if (entryIndex < dockEntries_.size() &&
                        IsFolderDockEntry(
                            dockEntries_[entryIndex]) &&
                        dockPendingDoubleClickEntry_ ==
                            entryIndex &&
                        elapsed <= GetDoubleClickTime())
                    {
                        const auto target =
                            ResolveDockFolderTarget(
                                dockEntries_[entryIndex]);
                        dockSuppressClickReleaseEntry_ =
                            entryIndex;
                        dockPressedContainer_ = dock;
                        dockPressedEntry_ = entryIndex;
                        dockPressedClosedCollectionPopup_ = false;
                        mouseDownPoint_ = pt;
                        mouseDown_ = true;
                        dockPendingDoubleClickEntry_ =
                            static_cast<size_t>(-1);
                        dockPendingDoubleClickFrequentItem_ =
                            static_cast<size_t>(-1);
                        dockPendingDoubleClickTick_ = 0;
                        CloseCollectionPopup(false);
                        if (target.available)
                            clearSelectionAfterAcceptedOpen(
                                shellLaunchWorker_.Enqueue(
                                    hwnd_, target.path));
                        InvalidateRect(
                            hwnd_, &dockBounds, FALSE);
                        specialDoubleClickHandled = true;
                    }
                    else if (dockItem->GetEntryType() == DockEntryType::DesktopItem &&
                        dockPendingDoubleClickEntry_ == dockItem->GetEntryIndex() &&
                        elapsed <= GetDoubleClickTime())
                    {
                        dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
                        dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
                        dockPendingDoubleClickTick_ = 0;
                        if (entryIndex < dockEntries_.size())
                        {
                            const size_t itemIndex =
                                FindItemIndexByKey(dockEntries_[entryIndex].reference);
                            if (itemIndex < items_.size())
                                clearSelectionAfterAcceptedOpen(
                                    LaunchDesktopItem(
                                        itemIndex, true));
                        }
                        InvalidateRect(hwnd_, &dockBounds, FALSE);
                        specialDoubleClickHandled = true;
                    }
                    if (snowdesktop::dock_window_rules::
                            ShouldDispatchDockDoubleClickPress(
                                specialDoubleClickHandled))
                    {
                        OnLeftButtonDown(wp, lp);
                    }
                    return 0;
                }
                if (dock->RunningItemAtPoint(pt))
                {
                    // WM_LBUTTONDBLCLK replaces the second button-down. Replay
                    // it so the following button-up can reverse an animation.
                    OnLeftButtonDown(wp, lp);
                    return 0;
                }
                if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
                {
                    const size_t itemIndex = frequentItem->GetItemIndex();
                    const DWORD elapsed = GetTickCount() - dockPendingDoubleClickTick_;
                    bool specialDoubleClickHandled = false;
                    if (dockPendingDoubleClickFrequentItem_ == itemIndex &&
                        elapsed <= GetDoubleClickTime())
                    {
                        dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
                        dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
                        dockPendingDoubleClickTick_ = 0;
                        if (itemIndex < items_.size())
                            clearSelectionAfterAcceptedOpen(
                                LaunchDesktopItem(
                                    itemIndex, true));
                        InvalidateRect(hwnd_, &dockBounds, FALSE);
                        specialDoubleClickHandled = true;
                    }
                    if (snowdesktop::dock_window_rules::
                            ShouldDispatchDockDoubleClickPress(
                                specialDoubleClickHandled))
                    {
                        OnLeftButtonDown(wp, lp);
                    }
                    return 0;
                }
                return 0;
            }
        }

        bool collectionOpenButtonHit = false;
        for (const auto& container : containers_)
        {
            auto* widgetContainer =
                dynamic_cast<WidgetContainer*>(
                    container.get());
            const DesktopWidget* widget =
                widgetContainer
                ? widgetContainer->GetWidgetData()
                : nullptr;
            if (widget &&
                widget->type ==
                    DesktopWidgetType::Collection &&
                widgetContainer->HitTestWidget(pt) ==
                    WidgetHit::CollectionOpenBtn)
            {
                collectionOpenButtonHit = true;
                break;
            }
        }
        bool pointerInsideInteractivePopup = false;
        if (IsCollectionPopupInteractive())
        {
            if (const DesktopWidget* popupWidget =
                    GetOpenPopupWidget())
            {
                const RECT popup =
                    GetCollectionPopupRect(*popupWidget);
                pointerInsideInteractivePopup =
                    PtInRect(&popup, pt) != FALSE;
            }
        }
        if (snowdesktop::popup_animation_rules::
                ShouldDispatchCollectionDoubleClickPress(
                    collectionOpenButtonHit,
                    pointerInsideInteractivePopup))
        {
            // Restore the second button-down so a rapid third click can
            // reverse a close animation before its 90 ms duration ends.
            OnLeftButtonDown(wp, lp);
            return 0;
        }

        if (dockFolderPopupOpen_ &&
            IsPointOccludedByOpenPopup(pt))
        {
            RECT popup = GetCollectionPopupRect(
                dockFolderPopupWidget_);
            if (PtInRect(&popup, pt))
            {
                RECT content =
                    GetCollectionPopupContentRect(popup);
                for (size_t i = 0;
                     i < dockFolderPopupWidget_.
                        folderEntries.size(); ++i)
                {
                    RECT itemRect =
                        GetCollectionPopupItemRect(
                            popup, i);
                    RECT clipped = itemRect;
                    clipped.top = std::max(
                        clipped.top, content.top);
                    clipped.bottom = std::min(
                        clipped.bottom,
                        content.bottom);
                    if (clipped.bottom <= clipped.top ||
                        !PtInRect(&clipped, pt))
                        continue;
                    const std::wstring path =
                        dockFolderPopupWidget_.
                            folderEntries[i].fullPath;
                    if (shellLaunchWorker_.Enqueue(
                            hwnd_, path))
                        CloseCollectionPopup();
                    return 0;
                }
                return 0;
            }
        }
        else if (popupWidgetIndex_ < widgets_.size() &&
            IsPointOccludedByOpenPopup(pt))
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, pt))
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                RECT content = GetCollectionPopupContentRect(popup);
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
                        if (LaunchDesktopItem(itemIndex))
                            CloseCollectionPopup();
                        return 0;
                    }
                }
                return 0;
            }
        }

        size_t standaloneWidget = HitTestStandaloneWidgetIndex(pt);
        if (standaloneWidget != static_cast<size_t>(-1) &&
            widgets_[standaloneWidget].type == DesktopWidgetType::LuaScript)
        {
            if (HitTestStandaloneWidget(standaloneWidget, pt) == WidgetHit::Content && widgetEngine_)
            {
                RECT frame = GetStandaloneWidgetFrameRect(widgets_[standaloneWidget]);
                widgetEngine_->EnsureWidgetLoaded(widgets_[standaloneWidget].id,
                    widgets_[standaloneWidget].packageId);
                widgetEngine_->InvokeMouseEvent(widgets_[standaloneWidget].id, "onDoubleClick",
                    pt.x - frame.left, pt.y - frame.top, 1, 0);
            }
            return 0;
        }

        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(it->get());
            if (!wc) continue;
            RECT bodyRect = wc->GetBodyRect();
            for (auto& slot : wc->GetSlots())
            {
                if (!slot) continue;
                RECT slotBounds = slot->GetBounds();
                if (!PtInRect(&slotBounds, pt)) continue;
                if (!PtInRect(&bodyRect, pt)) continue;
                if (auto* groupEntry =
                    dynamic_cast<CollectionGroupEntryItem*>(
                        slot->GetItem()))
                {
                    const size_t collectionIndex =
                        FindWidgetIndexById(
                            groupEntry->GetCollectionId());
                    if (collectionIndex < widgets_.size())
                        OpenCollectionPopupAt(collectionIndex, pt);
                    return 0;
                }
                if (auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem()))
                {
                    DesktopItem* item = icon->GetDesktopItem();
                    if (item)
                    {
                        const size_t itemIndex = FindItemIndexByKey(item->layoutKey);
                        if (itemIndex < items_.size())
                            clearSelectionAfterAcceptedOpen(
                                LaunchDesktopItem(itemIndex));
                        return 0;
                    }
                }
                if (auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem()))
                {
                    FolderEntry* entry = folderIcon->GetFolderEntry();
                    if (entry)
                    {
                        clearSelectionAfterAcceptedOpen(
                            shellLaunchWorker_.Enqueue(
                                hwnd_, entry->fullPath));
                        return 0;
                    }
                }
            }
        }

        int hit = HitTestItem(pt);
        if (hit >= 0)
        {
            clearSelectionAfterAcceptedOpen(
                LaunchDesktopItem(
                    static_cast<size_t>(hit)));
            return 0;
        }

        // Double-click on empty desktop area (exclude widget areas): toggle visibility
        bool overWidgetArea = (HitTestStandaloneWidgetIndex(pt) != static_cast<size_t>(-1));
        if (!overWidgetArea)
        {
            for (auto& c : containers_)
            {
                auto* wc = dynamic_cast<WidgetContainer*>(c.get());
                if (!wc) continue;
                RECT bodyRect = wc->GetBodyRect();
                if (PtInRect(&bodyRect, pt))
                {
                    overWidgetArea = true;
                    break;
                }
            }
        }
        if (!overWidgetArea)
            overWidgetArea = IsPointOverWidgetChrome(pt);

        if (!overWidgetArea && generalSettings_.doubleClickHideDesktop)
            ToggleDesktopIconsVisibility();

        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS;
    case WM_KEYDOWN:
    {
        const bool repeated =
            (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) != 0;
        if (TryHandlePageNavigationKey(wp, repeated))
            return 0;
        DispatchLuaWidgetViewKeyEvent(wp, true,
            repeated);
        OnKeyDown(wp, repeated);
        return 0;
    }
    case WM_SYSKEYDOWN:
    {
        using snowdesktop::desktop_keyboard_rules::AltF4Action;
        const AltF4Action altF4Action =
            snowdesktop::desktop_keyboard_rules::ResolveAltF4Action(
                true,
                wp == VK_F4,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 29)) != 0,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0);
        if (altF4Action != AltF4Action::PassThrough)
        {
            if (altF4Action ==
                AltF4Action::RequestWindowsShutdownDialog)
                RequestWindowsShutdownDialog();
            return 0;
        }
        if (TryHandlePageNavigationKey(
                wp,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0))
            return 0;
        if ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9'))
            DispatchLuaWidgetViewKeyEvent(wp, true,
                (static_cast<ULONG_PTR>(lp) &
                    (ULONG_PTR{1} << 30)) != 0);
        break;
    }
    case WM_SYSCHAR:
    {
        if (wp > 0x7f) break;
        const char key = static_cast<char>(wp);
        if (!((key >= 'A' && key <= 'Z') ||
                (key >= 'a' && key <= 'z') ||
                (key >= '0' && key <= '9')))
            break;
        const bool repeated =
            (static_cast<ULONG_PTR>(lp) & (ULONG_PTR{1} << 30)) != 0;
        if (!OnKeyDown(static_cast<WPARAM>(key), repeated)) break;
        return 0;
    }
    case WM_HOTKEY:
        if (settingsWindow_ &&
            settingsWindow_->IsHotkeyCaptureActive())
        {
            settingsWindow_->CaptureRegisteredHotkey(
                LOWORD(lp), HIWORD(lp));
            return 0;
        }
        if (static_cast<int>(wp) == kQuickNavigationHotkeyId)
        {
            ToggleQuickNavigation();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kFloatingDockHotkeyId)
        {
            ToggleFloatingDock();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kDesktopPassthroughHotkeyId)
        {
            BeginDesktopPassthroughHold();
            return 0;
        }
        break;
    case WM_KEYUP:
        DispatchLuaWidgetViewKeyEvent(wp, false, false);
        RefreshDragHintFromKeyboard();
        return 0;
    case WM_SYSKEYUP:
        if ((wp >= 'A' && wp <= 'Z') || (wp >= '0' && wp <= '9'))
            DispatchLuaWidgetViewKeyEvent(wp, false, false);
        break;
    case WM_KILLFOCUS:
        if (widgetEngine_)
        {
            widgetEngine_->ClearHostViewKeyState();
            widgetEngine_->CancelInteractionPointerPress();
        }
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        ForgetLuaWidgetPanelCapture(hwnd);
        if (msg == WM_CANCELMODE ||
            !IsOwnedPointerCaptureWindow(
                reinterpret_cast<HWND>(lp)))
        {
            if (CanCancelPointerPressAfterCaptureLoss())
            {
                CancelPointerPressWithoutCaptureRelease();
            }
        }
        break;
    case WM_DISPLAYCHANGE:
        InvalidateDragHintRaster();
        ScheduleDisplayTopologyRefresh();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
    {
        InvalidateDragHintRaster();
        const wchar_t* settingArea =
            reinterpret_cast<const wchar_t*>(lp);
        const bool traySettings = settingArea &&
            _wcsicmp(settingArea, L"TraySettings") == 0;
        const bool immersiveColor = settingArea &&
            _wcsicmp(settingArea, L"ImmersiveColorSet") == 0;

        if (!traySettings && !immersiveColor)
            ScheduleDisplayTopologyRefresh();
        if (immersiveColor)
            ApplyPersistentDockHostAppearance();
        InvalidateRect(hwnd_, nullptr, FALSE);
        // Explorer also broadcasts this message for view options such as
        // "Hidden items". Theme and taskbar notifications do not change the
        // desktop namespace, so avoid a synchronous full item reload for them.
        if (!traySettings && !immersiveColor)
            ReloadItems(false);
        return 0;
    }
    case WM_FONTCHANGE:
    case WM_THEMECHANGED:
        InvalidateDragHintRaster();
        if (msg == WM_THEMECHANGED)
            ApplyPersistentDockHostAppearance();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case kShellChangeMessage:
    {
        // SHCNRF_NewDelivery 模式下 lParam 携带通知句柄，
        // 必须 Lock/Unlock 消费释放，否则每次事件泄漏句柄。
        // 本应用不依赖事件 PIDL 细节（debounce 后全量刷新），
        // 因此只消费不处理。自身 PostMessage(0, 0) 的 lParam 为 0。
        const HANDLE notify = reinterpret_cast<HANDLE>(lp);
        if (notify)
        {
            LONG eventId = 0;
            PIDLIST_ABSOLUTE* pidls = nullptr;
            if (SHChangeNotification_Lock(notify, 1, &pidls, &eventId))
                SHChangeNotification_Unlock(notify);
        }
        shellReloadPending_ = true;
        shellReloadLayoutFromDiskPending_ = true;
        SetTimer(hwnd_, kShellChangeTimerId, kShellChangeDebounceMs, nullptr);
        return 0;
    }
    case kIconLoadedMessage:
        OnIconLoaded(wp, lp);
        return 0;
    case kDemoIconDecodedMessage:
        OnDemoIconDecoded(lp);
        return 0;
    case kWidgetConsentResolvedMessage:
        CompleteLuaWidgetConsent(wp, lp);
        return 0;
    case kWidgetConsentOpenedMessage:
        NotifyLuaWidgetConsentDialogOpened(wp, lp);
        return 0;
    case kWidgetAudioAnalysisWakeMessage:
        if (widgetEngine_)
            widgetEngine_->OnAudioAnalysisWake();
        return 0;
    case kQuickNavigationAppsIndexedMessage:
        OnQuickNavigationAppsIndexed(wp, lp);
        return 0;
    case kQuickNavigationEverythingSearchMessage:
        OnQuickNavigationEverythingSearchCompleted(wp);
        return 0;
    case kCommitRenameMessage:
        renameCommitPending_ = false;
        CommitRename(wp != 0);
        return 0;
    case kShellFileOperationCompletedMessage:
        OnShellFileOperationCompleted(lp);
        return 0;
    case kUrlDropDownloadCompletedMessage:
        OnUrlDropDownloadCompleted(lp);
        return 0;
    case kForegroundInteractionChangedMessage:
        ReconcileDesktopHoverState();
        return 0;
    case kSteamWorkshopSubscriptionReadyMessage:
        PollSteamWorkshopSubscriptions();
        return 0;
    case kSteamWorkshopSubscriptionChangedMessage:
        PollSteamWorkshopSubscriptions(true);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        if (widgetAccessibilityProvider_)
            widgetAccessibilityProvider_->DetachWindow(hwnd);
        StopDemoIconLoader();
        if (luaInlineEdit_)
            CommitLuaInlineTextEdit(false);
        UnregisterNavigationHotkey();
        UnregisterFloatingDockHotkey();
        if (!exitRequested_)
        {
            ResetDesktopWindowResources();
            if (controlHwnd_ && IsWindow(controlHwnd_))
                SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
            return 0;
        }
        SaveLayoutSlots();
        RemoveTrayIcon();
        ResetDesktopWindowResources();
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
        RestoreExplorerIcons();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
