#include "app.h"
#include "../desktop_hover_rules.h"
#include "../steam_app_identity.h"
#include "../widget_visibility_rules.h"

// Pointer leave, Dock click release and primary-button drag completion.

namespace
{
bool OpenMissingWidgetWorkshopPage(HWND owner,
    const DesktopWidget& widget)
{
    if (widget.packageSourceProvider != L"steam-workshop")
        return false;
    const std::string publishedFileId = snowdesktop::widget::
        SteamPublishedFileId(
            WideToUtf8(widget.packageSourceExternalItemId));
    if (publishedFileId.empty()) return false;

    const std::wstring clientUrl = snowdesktop::
        SnowDesktopSteamCommunityItemClientUrl(publishedFileId);
    const HINSTANCE opened = ShellExecuteW(owner, L"open",
        clientUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
    {
        const std::wstring webUrl = snowdesktop::
            SnowDesktopSteamCommunityItemUrl(publishedFileId);
        ShellExecuteW(owner, L"open", webUrl.c_str(), nullptr, nullptr,
            SW_SHOWNORMAL);
    }
    return true;
}
}

void DesktopApp::OnMouseLeave()
{
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseLeaveBegin);
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const bool pointerStillInteractsWithDockPreview =
        dockWindowPreview_ &&
        dockWindowPreview_->ContainsInteractionPoint(cursorScreen);
    if (pointerStillInteractsWithDockPreview)
        dockWindowPreview_->ScheduleHide();
    else
        HideDockWindowPreview();
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    popupDwellController_.Reset();
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    ResetDockHandoffDwell();
    if (hwnd_)
    {
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        KillTimer(hwnd_, kCollectionGroupTabDwellTimerId);
    }

    // Capture-based dragging continues to receive coordinates outside the
    // window. Preserve that pointer state, but clear passive hover immediately.
    // The preview owns its independent screen-space transition triangle, so
    // Dock magnification can still be reset while the preview stays reachable.
    const HWND captureWindow = GetCapture();
    const bool ownsInteractionCapture =
        snowdesktop::desktop_hover_rules::
            OwnsInteractionCapture(
                captureWindow,
                hwnd_, floatingDockHwnd_);
    const bool canClearPassiveHover =
        snowdesktop::desktop_hover_rules::CanClearPassiveHover(
            ownsInteractionCapture,
            mouseDown_,
            dragSession_.IsActive(),
            widgetAction_ != WidgetAction::None ||
                middleButtonWidgetMove_ ||
                detailColumnResizeActive_ ||
                luaWidgetPanelMouseDown_);
    if (canClearPassiveHover)
    {
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
        if (widgetEngine_)
            widgetEngine_->ClearInteractionHover();
        PresentPassiveHoverVisualChange();
        RecordShellHoverTrace(
            ShellHoverTraceEvent::MouseLeaveEnd);
        return;
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseLeaveEnd);
}

void DesktopApp::ReconcileDesktopHoverState(
    snowdesktop::desktop_hover_rules::ReconcileMode mode)
{
    RecordShellHoverTrace(
        ShellHoverTraceEvent::ReconcileBegin);
    if (!hwnd_ || !IsWindow(hwnd_))
        return;

    const DWORD foregroundTick =
        dockForegroundChangedTick_.load();
    const bool foregroundSettled =
        snowdesktop::desktop_hover_rules::HasForegroundSettled(
            foregroundTick != 0,
            GetTickCount() - foregroundTick);
    if (foregroundTick != 0 &&
        foregroundTick != desktopHoverForegroundObservedTick_)
    {
        // Foreground/minimize transitions can make Explorer restack the
        // desktop content child without its separate backdrop sibling. A
        // later hover repaired this incidentally through BeginFrame's
        // SyncWindowPlacement; repair the pair immediately on the actual
        // foreground event so glass never depends on another pointer move.
        desktopBackdropCompositor_.Reattach(hwnd_);
    }
    const HWND shellDialogOwner = ShellDialogOwnerHwnd();
    const bool shellDialogOwnerAvailable =
        shellDialogOwner && IsWindow(shellDialogOwner);
    if (!snowdesktop::desktop_hover_rules::
            ShouldReconcileFromSurfaceSample(
                shellPopupMenuLayerDepth_ > 0,
                shellDialogOwnerAvailable,
                !shellDialogOwnerAvailable ||
                    IsWindowEnabled(shellDialogOwner)))
    {
        // TrackPopupMenuEx and a synchronously invoked Shell command run a
        // private modal loop. Real pointer leave messages remain authoritative,
        // but WindowFromPoint can alternate between a menu/dialog shadow and
        // the desktop while those Shell-owned windows are being restacked.
        // Do not let foreground notifications or the periodic fallback replay
        // passive hover until both the popup layer has unwound and the Shell
        // dialog owner has been re-enabled. The latter tracks Task Dialogs
        // whose visible lifetime outlasts IContextMenu::InvokeCommand.
        desktopHoverForegroundObservedTick_ = foregroundTick;
        RecordShellHoverTrace(
            ShellHoverTraceEvent::ReconcileSuspended);
        return;
    }
    POINT cursorPoint{};
    if (TryGetDesktopHoverPointFromCursor(cursorPoint))
    {
        desktopHoverForegroundObservedTick_ = foregroundTick;
        const bool passiveHoverCleared =
            lastMousePoint_.x == LONG_MIN &&
            lastMousePoint_.y == LONG_MIN;
        if (snowdesktop::desktop_hover_rules::
                ShouldActivateFromSurfaceSample(
                    true, passiveHoverCleared, mode,
                    foregroundSettled))
        {
            lastMousePoint_ = cursorPoint;
            PresentPassiveHoverVisualChange();
            RecordShellHoverTrace(
                ShellHoverTraceEvent::ReconcileActivate,
                cursorPoint);
        }
        else
        {
            RecordShellHoverTrace(
                ShellHoverTraceEvent::ReconcileNoChange,
                cursorPoint);
        }
        return;
    }

    const HWND captureWindow = GetCapture();
    DWORD captureProcessId = 0;
    if (captureWindow)
    {
        GetWindowThreadProcessId(
            captureWindow, &captureProcessId);
    }
    const bool ownsInteractionCapture =
        captureProcessId != 0 &&
        captureProcessId == GetCurrentProcessId();
    if (!snowdesktop::desktop_hover_rules::CanClearPassiveHover(
            ownsInteractionCapture,
            mouseDown_,
            dragSession_.IsActive(),
            widgetAction_ != WidgetAction::None ||
                middleButtonWidgetMove_ ||
                detailColumnResizeActive_ ||
                luaWidgetPanelMouseDown_))
    {
        // Do not mark this foreground state as reconciled. The periodic
        // fallback will retry after the active interaction has ended.
        return;
    }

    const bool pointerAlreadyCleared =
        lastMousePoint_.x == LONG_MIN &&
        lastMousePoint_.y == LONG_MIN;
    const bool previewVisible =
        dockWindowPreview_ &&
        dockWindowPreview_->IsVisible();
    const bool foregroundAlreadyReconciled =
        foregroundTick != 0 &&
        desktopHoverForegroundObservedTick_ == foregroundTick;
    if (foregroundAlreadyReconciled &&
        pointerAlreadyCleared &&
        navHoverSide_ == 0 &&
        !previewVisible)
    {
        return;
    }

    desktopHoverForegroundObservedTick_ = foregroundTick;
    // An external application terminates the Dock-preview bridge even when its
    // old screen rectangle still contains the cursor.
    HideDockWindowPreview();
    OnMouseLeave();
    RecordShellHoverTrace(
        ShellHoverTraceEvent::ReconcileClear);
}

std::uint64_t DesktopApp::HoverOnlyVisibleMask() const
{
    std::uint64_t mask = 0;
    size_t bit = 0;
    for (size_t widgetIndex = 0;
         widgetIndex < widgets_.size() && bit < 64;
         ++widgetIndex)
    {
        const auto& widget = widgets_[widgetIndex];
        if (!widget.showOnHoverOnly)
            continue;
        const bool interactionRetained =
            popupWidgetIndex_ == widgetIndex ||
            (!interactionPinnedWidgetId_.empty() &&
                interactionPinnedWidgetId_ == widget.id);
        const RECT frame =
            GetStandaloneWidgetFrameRect(widget);
        if (snowdesktop::widget_visibility_rules::
                ShouldRenderWidget(
                    true,
                    dragSession_.IsActive(),
                    dragDropController_.IsExternalDragActive(),
                    widgetAction_ == WidgetAction::Move,
                    widget.selected,
                    interactionRetained,
                    PtInRect(&frame, lastMousePoint_) != FALSE))
        {
            mask |= std::uint64_t{ 1 } << bit;
        }
        ++bit;
    }
    return mask;
}

void DesktopApp::BeginShellHoverTrace()
{
    shellHoverTraceWriteIndex_ = 0;
    shellHoverTraceCount_ = 0;
    shellHoverTraceStartTick_ = GetTickCount64();
    shellHoverTraceMenuEndTick_ = 0;
    shellHoverTraceWrapped_ = false;
    shellHoverTraceObservedDisabledOwner_ = false;
    shellHoverTraceActive_ = true;
}

void DesktopApp::RecordShellHoverTrace(
    ShellHoverTraceEvent event,
    POINT eventPoint)
{
    if (!shellHoverTraceActive_)
        return;

    const HWND owner = ShellDialogOwnerHwnd();
    const bool ownerAvailable =
        owner && IsWindow(owner);
    const bool ownerEnabled =
        !ownerAvailable || IsWindowEnabled(owner);
    shellHoverTraceObservedDisabledOwner_ =
        shellHoverTraceObservedDisabledOwner_ ||
        (ownerAvailable && !ownerEnabled);

    ShellHoverTraceEntry entry{};
    entry.tick = GetTickCount64();
    entry.event = event;
    entry.eventPoint = eventPoint;
    entry.lastMousePoint = lastMousePoint_;
    GetCursorPos(&entry.cursorScreen);
    entry.hoverOnlyVisibleMask =
        HoverOnlyVisibleMask();
    entry.foregroundWindow = GetForegroundWindow();
    entry.captureWindow = GetCapture();
    entry.popupDepth = shellPopupMenuLayerDepth_;
    if (compositionCommitPending_)
        entry.flags |= 1u << 0;
    if (desktopBackdropFullCollectionPending_)
        entry.flags |= 1u << 1;
    if (compositionPaintInProgress_)
        entry.flags |= 1u << 2;
    if (ownerAvailable)
        entry.flags |= 1u << 3;
    if (ownerEnabled)
        entry.flags |= 1u << 4;
    if (lastMousePoint_.x != LONG_MIN &&
        lastMousePoint_.y != LONG_MIN)
        entry.flags |= 1u << 5;
    if (entry.captureWindow)
        entry.flags |= 1u << 6;
    if (mouseDown_)
        entry.flags |= 1u << 7;

    shellHoverTrace_[shellHoverTraceWriteIndex_] =
        entry;
    shellHoverTraceWriteIndex_ =
        (shellHoverTraceWriteIndex_ + 1) %
        kShellHoverTraceCapacity;
    if (shellHoverTraceCount_ <
        kShellHoverTraceCapacity)
    {
        ++shellHoverTraceCount_;
    }
    else
    {
        shellHoverTraceWrapped_ = true;
    }

    const bool menuEnded =
        shellPopupMenuLayerDepth_ == 0 &&
        shellHoverTraceMenuEndTick_ != 0;
    const bool observedModalOwnerFinished =
        shellHoverTraceObservedDisabledOwner_ &&
        ownerEnabled;
    const bool postMenuTraceTimedOut =
        menuEnded &&
        entry.tick - shellHoverTraceMenuEndTick_ >= 10000;
    if (menuEnded &&
        (observedModalOwnerFinished ||
            postMenuTraceTimedOut))
    {
        FlushShellHoverTrace();
    }
}

void DesktopApp::FlushShellHoverTrace()
{
    if (!shellHoverTraceActive_)
        return;
    shellHoverTraceActive_ = false;
    if (shellHoverTraceCount_ == 0)
        return;

    const auto eventName = [](ShellHoverTraceEvent event) {
        switch (event)
        {
        case ShellHoverTraceEvent::MenuBegin: return L"menu-begin";
        case ShellHoverTraceEvent::MouseMoveBegin: return L"move-begin";
        case ShellHoverTraceEvent::MouseMoveEnd: return L"move-end";
        case ShellHoverTraceEvent::MouseLeaveBegin: return L"leave-begin";
        case ShellHoverTraceEvent::MouseLeaveEnd: return L"leave-end";
        case ShellHoverTraceEvent::ReconcileBegin: return L"reconcile-begin";
        case ShellHoverTraceEvent::ReconcileSuspended: return L"reconcile-suspended";
        case ShellHoverTraceEvent::ReconcileActivate: return L"reconcile-activate";
        case ShellHoverTraceEvent::ReconcileClear: return L"reconcile-clear";
        case ShellHoverTraceEvent::ReconcileNoChange: return L"reconcile-no-change";
        case ShellHoverTraceEvent::PaintBegin: return L"paint-begin";
        case ShellHoverTraceEvent::PaintEnd: return L"paint-end";
        case ShellHoverTraceEvent::PassivePresent: return L"passive-present";
        case ShellHoverTraceEvent::CommitQueued: return L"commit-queued";
        case ShellHoverTraceEvent::CommitFlushed: return L"commit-flushed";
        case ShellHoverTraceEvent::MenuEnd: return L"menu-end";
        }
        return L"unknown";
    };

    std::wstring report =
        L"Shell hover trace begin: entries=" +
        std::to_wstring(shellHoverTraceCount_) +
        L" wrapped=" +
        std::to_wstring(shellHoverTraceWrapped_ ? 1 : 0) +
        L" ownerDisabled=" +
        std::to_wstring(
            shellHoverTraceObservedDisabledOwner_ ? 1 : 0) +
        L"\r\n";
    const size_t first = shellHoverTraceWrapped_
        ? shellHoverTraceWriteIndex_ : 0;
    for (size_t offset = 0;
         offset < shellHoverTraceCount_;
         ++offset)
    {
        const auto& entry = shellHoverTrace_[
            (first + offset) %
            kShellHoverTraceCapacity];
        wchar_t line[384]{};
        swprintf_s(
            line,
            L"+%llu %ls event=(%ld,%ld) last=(%ld,%ld) cursor=(%ld,%ld) mask=0x%016llX fg=%p cap=%p depth=%d flags=0x%04X\r\n",
            entry.tick - shellHoverTraceStartTick_,
            eventName(entry.event),
            entry.eventPoint.x,
            entry.eventPoint.y,
            entry.lastMousePoint.x,
            entry.lastMousePoint.y,
            entry.cursorScreen.x,
            entry.cursorScreen.y,
            static_cast<unsigned long long>(
                entry.hoverOnlyVisibleMask),
            entry.foregroundWindow,
            entry.captureWindow,
            entry.popupDepth,
            static_cast<unsigned>(entry.flags));
        report += line;
    }
    report += L"Shell hover trace end";
    WriteDiagnosticLogEntry(
        report.c_str(),
        DiagnosticLogLevel::Debug);
}

bool DesktopApp::HandleDockClickRelease(POINT point)
{
    DockContainer* dock = dockPressedContainer_;
    if (!dock) dock = GetDockContainerAtPoint(point);
    if (!dock) return false;

    DockEntryType entryType = DockEntryType::DesktopItem;
    std::wstring reference;
    size_t frequentItemIndex = static_cast<size_t>(-1);
    std::wstring runningAppKey;
    const size_t pressedEntryIndex = dockPressedEntry_;
    const size_t pressedFrequentItemIndex = dockPressedFrequentItem_;
    const auto pressedWindowAction = dockPressedWindowAction_;
    const HWND pressedTargetWindow = dockPressedTargetWindow_;
    std::optional<RECT> pressedAnchorScreen;
    if (mouseDownHit_ && hwnd_)
    {
        RECT anchor = dock->GetElementVisualRect(
            mouseDownHit_->GetBounds(), mouseDownPoint_);
        if (!IsRectEmpty(&anchor))
        {
            MapWindowPoints(
                hwnd_, nullptr,
                reinterpret_cast<POINT*>(&anchor), 2);
            pressedAnchorScreen = anchor;
        }
    }
    if (dockPressedEntry_ < dockEntries_.size())
    {
        DockEntryItem* hit = dock->EntryAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetEntryIndex() != dockPressedEntry_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        entryType = dockEntries_[dockPressedEntry_].type;
        reference = dockEntries_[dockPressedEntry_].reference;
    }
    else if (!dockPressedRunningAppKey_.empty())
    {
        DockRunningItem* hit = dock->RunningItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetIdentityKey() != dockPressedRunningAppKey_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        runningAppKey = dockPressedRunningAppKey_;
    }
    else if (dockPressedFrequentItem_ < items_.size())
    {
        DockFrequentItem* hit = dock->FrequentItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetItemIndex() != dockPressedFrequentItem_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        frequentItemIndex = dockPressedFrequentItem_;
    }
    else
    {
        return false;
    }
    const bool folderEntry =
        pressedEntryIndex < dockEntries_.size() &&
        IsFolderDockEntry(dockEntries_[pressedEntryIndex]);

    size_t appItemIndex = frequentItemIndex;
    if (appItemIndex >= items_.size() && entryType == DockEntryType::DesktopItem)
        appItemIndex = FindItemIndexByKey(reference);
    bool waitForDoubleClick = false;
    if (appItemIndex < items_.size() && !folderEntry)
        waitForDoubleClick =
            pressedWindowAction ==
                snowdesktop::dock_window_rules::DockClickAction::Launch;

    // 在激活外部应用前完整结束本次桌面交互，避免鼠标捕获、选择高亮或
    // 拖拽预览跨到新前台窗口后仍残留。
    EndDragSession();
    HideDragHintWindow();
    if (!waitForDoubleClick)
        ClearSelection();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    ReleaseCapture();
    InvalidateDragStaticScene();
    const bool popupEntryOnFloatingDock =
        (entryType == DockEntryType::Collection ||
         folderEntry) &&
        floatingDockVisible_ &&
        dock == floatingDockContainer_;
    if (hwnd_ && !popupEntryOnFloatingDock)
    {
        if (floatingDockVisible_ &&
            dock == floatingDockContainer_)
            InvalidateFloatingDockWindow(true);
        else
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
    }

    if (waitForDoubleClick)
    {
        dockPendingDoubleClickEntry_ = pressedEntryIndex;
        dockPendingDoubleClickFrequentItem_ = pressedFrequentItemIndex;
        dockPendingDoubleClickTick_ = GetTickCount();
        return true;
    }

    dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
    dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
    dockPendingDoubleClickTick_ = 0;

    if (snowdesktop::dock_window_rules::
            ShouldSuppressDockClickRelease(
                pressedEntryIndex,
                dockSuppressClickReleaseEntry_))
    {
        dockSuppressClickReleaseEntry_ =
            static_cast<size_t>(-1);
        return true;
    }

    const auto dispatchWindowCommand =
        [this, pressedWindowAction](
            std::function<bool(
                DockWindowTransitionCapturePolicy)> command) {
            if (!command)
                return;
            const bool requiresFloatingDockClose =
                snowdesktop::dock_window_rules::
                    RequiresFloatingDockMinimizeCaptureIsolation(
                        floatingDockVisible_,
                        pressedWindowAction);
            if (!requiresFloatingDockClose &&
                !floatingDockClosePending_)
            {
                command(DockWindowTransitionCapturePolicy::
                    SnapshotPreferred);
                return;
            }

            // A DWM thumbnail contains only the target HWND, so it can animate
            // minimize without hiding the floating Dock. If registration is
            // unavailable, retain the proven close-and-snapshot path.
            if (requiresFloatingDockClose &&
                !floatingDockClosePending_ &&
                command(DockWindowTransitionCapturePolicy::
                    LiveThumbnailOnly))
            {
                return;
            }

            CloseFloatingDockThen(
                [command = std::move(command)]() mutable {
                    command(DockWindowTransitionCapturePolicy::
                        SnapshotPreferred);
                },
                true,
                FloatingDockCloseFocusPolicy::PreserveCurrent);
        };

    if (!runningAppKey.empty())
    {
        const auto running = std::find_if(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& app) {
                return app.identityKey == runningAppKey;
            });
        if (running != dockUnpinnedRunningApps_.end())
        {
            const HWND runningWindow = running->window;
            dispatchWindowCommand(
                [this, runningWindow, pressedWindowAction,
                    pressedTargetWindow, pressedAnchorScreen](
                    DockWindowTransitionCapturePolicy capturePolicy) {
                    return ActivateOrToggleDockWindow(
                        runningWindow, pressedWindowAction,
                        pressedTargetWindow,
                        pressedAnchorScreen,
                        capturePolicy);
                });
        }
    }
    else if (frequentItemIndex < items_.size())
    {
        dispatchWindowCommand(
            [this, frequentItemIndex, pressedWindowAction,
                pressedTargetWindow, pressedAnchorScreen](
                DockWindowTransitionCapturePolicy capturePolicy) {
                return ActivateOrToggleDockItem(
                    frequentItemIndex, pressedWindowAction,
                    pressedTargetWindow,
                    pressedAnchorScreen,
                    capturePolicy);
            });
    }
    else if (entryType == DockEntryType::Collection)
    {
        const size_t widgetIndex = FindWidgetIndexById(reference);
        if (widgetIndex < widgets_.size())
        {
            if (IsCollectionPopupInteractive() &&
                snowdesktop::floating_dock_rules::
                    ShouldCloseCollectionPopup(
                        popupWidgetIndex_,
                        widgetIndex))
                CloseCollectionPopup();
            else
                OpenCollectionPopupAt(
                    widgetIndex, point);
        }
    }
    else if (folderEntry)
    {
        const std::wstring sourceId =
            std::to_wstring(static_cast<int>(entryType)) +
            L":" + ToUpperInvariant(reference);
        if (IsCollectionPopupInteractive() &&
            dockFolderPopupOpen_ &&
            dockFolderPopupSourceId_ == sourceId)
            CloseCollectionPopup();
        else
            OpenDockFolderPopupAt(
                pressedEntryIndex, point);
        dockPendingDoubleClickEntry_ =
            pressedEntryIndex;
        dockPendingDoubleClickTick_ =
            GetTickCount();
    }
    else
    {
        const size_t itemIndex = FindItemIndexByKey(reference);
        if (itemIndex < items_.size())
        {
            dispatchWindowCommand(
                [this, itemIndex, pressedWindowAction,
                    pressedTargetWindow, pressedAnchorScreen](
                    DockWindowTransitionCapturePolicy capturePolicy) {
                    return ActivateOrToggleDockItem(
                        itemIndex, pressedWindowAction,
                        pressedTargetWindow,
                        pressedAnchorScreen,
                        capturePolicy);
                });
        }
    }
    return true;
}

void DesktopApp::OnLeftButtonUp(WPARAM wp, LPARAM lp)
{
    if (middleButtonWidgetMove_) return;
    (void)wp;
    POINT upPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int dropPreviewMods = 0;
    bool commitVisualBeforeDrop = false;
    HideDragHintWindow();

    if (detailColumnResizeActive_)
    {
        const size_t widgetIndex = mouseDownWidgetIndex_;
        detailColumnResizeActive_ = false;
        detailColumnResizeColumn_ =
            snowdesktop::list_detail_rules::Column::None;
        detailColumnResizeHeaderLeft_ = 0;
        detailColumnResizeHeaderWidth_ = 1;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        SaveLayoutSlots();
        if (widgetIndex < widgets_.size())
            InvalidateRect(hwnd_, &widgets_[widgetIndex].bounds, FALSE);
        return;
    }

    if (luaWidgetPanelMouseDown_ &&
        !luaWidgetPanelRequest_.widgetId.empty())
    {
        const std::wstring panelWidgetId =
            luaWidgetPanelRequest_.widgetId;
        const RECT content =
            GetLuaWidgetPanelContentRect();
        const int localX =
            upPoint.x - content.left;
        const int localY =
            upPoint.y - content.top;
        const bool hostInputHandled =
            widgetEngine_ &&
            widgetEngine_->HandleHostInputPointerUp(
                panelWidgetId, localX, localY, "panel");
        if (!hostInputHandled &&
            PtInRect(&content, upPoint) &&
            widgetEngine_)
        {
            widgetEngine_->InvokeMouseEvent(
                panelWidgetId,
                "onPanelMouseUp",
                localX, localY, 1, 0);
            widgetEngine_->InvokeMouseEvent(
                panelWidgetId,
                "onPanelClick",
                localX, localY, 1, 0);
        }
        luaWidgetPanelMouseDown_ = false;
        mouseDown_ = false;
        ReleaseCapture();
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    for (auto& container : containers_)
    {
        auto* searchable =
            dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!searchable ||
            !searchable->IsSearchPointerSelecting())
            continue;
        searchable->UpdateSearchPointerSelection(upPoint);
        searchable->EndSearchPointerSelection();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ =
            static_cast<size_t>(-1);
        ReleaseCapture();
        UpdateHostInputImePosition();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Guide buttons open modal UI only after the initiating click has been
    // released. Opening the menu from WM_LBUTTONDOWN lets that same click's
    // button-up event immediately dismiss the newly-created menu.
    if (pendingGuideAction_ != WidgetHit::None)
    {
        const WidgetHit pendingAction = pendingGuideAction_;
        const size_t widgetIndex = mouseDownWidgetIndex_;
        GuideWidget* guide = nullptr;
        if (widgetIndex < widgets_.size() &&
            widgets_[widgetIndex].type == DesktopWidgetType::Guide)
        {
            for (auto& container : containers_)
            {
                auto* candidate = dynamic_cast<GuideWidget*>(container.get());
                if (candidate &&
                    candidate->GetWidgetData() == &widgets_[widgetIndex])
                {
                    guide = candidate;
                    break;
                }
            }
        }
        const bool invoke = guide &&
            guide->HitTestWidget(upPoint) == pendingAction;
        const RECT guideBounds = widgetIndex < widgets_.size()
            ? widgets_[widgetIndex].bounds : RECT{};

        pendingGuideAction_ = WidgetHit::None;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        if (!IsRectEmpty(&guideBounds))
            InvalidateRect(hwnd_, &guideBounds, FALSE);

        if (!invoke)
            return;
        if (pendingAction == WidgetHit::GuideDetailsBtn)
        {
            guide->ToggleDetails();
            InvalidateRect(hwnd_, &guideBounds, FALSE);
            return;
        }

        POINT screenPoint = upPoint;
        ClientToScreen(hwnd_, &screenPoint);
        ShowAddWidgetMenu(screenPoint);
        return;
    }

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move)
        {
            const auto movingPayload = snowdesktop::slot_contract::
                PayloadForWidgetType(
                    widgets_[mouseDownWidgetIndex_].type);
            const bool canCollectionGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::CollectionGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::CollectionGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canFileGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::FileGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::FileGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canGroup =
                canCollectionGroup || canFileGroup;
            DockContainer* dock = canGroup
                ? nullptr : GetDockContainerAtPoint(upPoint);
            const bool canDock = !canGroup && dock &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Dock,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            if (canGroup)
            {
                if (canCollectionGroup)
                    AddCollectionToGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
                else
                    AddWidgetToFileGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
            }
            else if (canDock)
            {
                Widget dockSource(&widgets_[mouseDownWidgetIndex_], this);
                int mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0;
                CommitDockDrop({ &dockSource }, nullptr, dock,
                    widgetDockInsertIndex_, mods);
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
            }
            else
                PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
        }
        else if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        // PendingMove/PendingResize: just cancel without displacement
        widgetAction_ = WidgetAction::None;
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);
        InvalidateDragStaticScene();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.IsActive())
    {
        if (HandleDockClickRelease(upPoint))
            return;
        dockPressedEntry_ = static_cast<size_t>(-1);
        dockPressedFrequentItem_ = static_cast<size_t>(-1);
        dockPressedRunningAppKey_.clear();
        dockPressedWindowAction_ =
            snowdesktop::dock_window_rules::DockClickAction::None;
        dockPressedTargetWindow_ = nullptr;
        dockPressedContainer_ = nullptr;
        if (mouseDownWidgetIndex_ < widgets_.size() &&
            widgets_[mouseDownWidgetIndex_].type ==
                DesktopWidgetType::LuaScript &&
            widgetEngine_)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            const bool loaded = widgetEngine_->EnsureWidgetLoaded(
                widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].packageId);
            const bool contentClicked =
                HitTestStandaloneWidget(mouseDownWidgetIndex_,
                    upPoint) == WidgetHit::Content;
            const auto hostState = widgetEngine_->GetWidgetHostState(
                widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].packageId);
            const auto hostAction = snowdesktop::widget_runtime::
                HostActionFor(hostState.kind);
            const RECT hostActionRect = GetLuaWidgetHostActionRect(
                widgets_[mouseDownWidgetIndex_]);
            const bool hostActionClicked = contentClicked &&
                PtInRect(&hostActionRect, mouseDownPoint_) &&
                PtInRect(&hostActionRect, upPoint);
            if (!loaded && hostActionClicked &&
                hostAction == snowdesktop::widget_runtime::
                    WidgetHostAction::OpenPackageSource)
            {
                OpenMissingWidgetWorkshopPage(hwnd_,
                    widgets_[mouseDownWidgetIndex_]);
            }
            else if (!loaded && hostActionClicked &&
                hostAction == snowdesktop::widget_runtime::
                    WidgetHostAction::RequestPermission)
            {
                POINT screenPoint = upPoint;
                ClientToScreen(hwnd_, &screenPoint);
                BeginLuaWidgetConsent(screenPoint,
                    widgets_[mouseDownWidgetIndex_].packageId,
                    widgets_[mouseDownWidgetIndex_].id);
            }
            else if (!loaded && hostActionClicked &&
                hostAction == snowdesktop::widget_runtime::
                    WidgetHostAction::Reload)
            {
                (void)widgetEngine_->RetryWidget(
                    widgets_[mouseDownWidgetIndex_].id,
                    widgets_[mouseDownWidgetIndex_].packageId);
            }
            else if (loaded)
            {
                const bool hostInputHandled =
                    widgetEngine_->HandleHostInputPointerUp(
                        widgets_[mouseDownWidgetIndex_].id,
                        upPoint.x - frame.left,
                        upPoint.y - frame.top);
                if (hostInputHandled)
                    UpdateHostInputImePosition();
                if (!hostInputHandled && contentClicked)
                {
                    widgetEngine_->InvokeMouseEvent(
                        widgets_[mouseDownWidgetIndex_].id,
                        "onMouseUp", upPoint.x - frame.left,
                        upPoint.y - frame.top, 1, 0);
                    widgetEngine_->InvokeClick(
                        widgets_[mouseDownWidgetIndex_].id,
                        upPoint.x - frame.left,
                        upPoint.y - frame.top);
                }
            }
        }
        if (pendingCtrlToggleDesktopIndex_ < items_.size())
            items_[pendingCtrlToggleDesktopIndex_].selected = false;
        pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
        if (pendingCtrlToggleWidgetItem_)
        {
            pendingCtrlToggleWidgetItem_->SetSelected(!pendingCtrlToggleWidgetItem_->IsSelected());
            pendingCtrlToggleWidgetItem_ = nullptr;
        }
        mouseDown_ = false;
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        marqueeDockFolderPopup_ = false;
        dockFolderPopupMarqueeInitialSelection_.clear();
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // Dock 项轻微拖动后仍落回原项时，按单击处理而不是吞掉本次操作。
    if (HandleDockClickRelease(upPoint))
        goto cleanup;

    if (!GetDockDragOutRemovalHint(upPoint).empty())
    {
        const bool removed = RemoveDockDragOutItems(dragSession_.Items());
        ClearSelection();
        EndDragSession();
        if (removed)
        {
            SaveLayoutSlots();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        goto cleanup;
    }

    if (!dragSession_.TargetContainer() ||
        dragSession_.TargetRegion() == HitRegion::None ||
        dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        goto cleanup;
    }

    dropPreviewMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        dropPreviewMods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        dropPreviewMods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        dropPreviewMods |= MK_SHIFT;
    commitVisualBeforeDrop =
        dragSession_.TargetRegion() == HitRegion::Handoff;
    if (!commitVisualBeforeDrop)
    {
        const DropPreviewList dropPreview = BuildDropPreviewList(
            dragSession_.SourceList(),
            dragSession_.TargetContainer(),
            dragSession_.TargetSlot(),
            dragSession_.TargetRegion(),
            dropPreviewMods,
            dragSession_.CurrentPoint());
        commitVisualBeforeDrop = dropPreview.fileBacked;
    }

    // Shell/file-backed drop handlers may synchronously show a progress
    // window. Commit their visual end up front, but keep pure internal moves
    // in the current frame until the model has its final position. Otherwise
    // the source item is synchronously painted once at its old location.
    dragSession_.DeactivateForDrop();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapture();
    if (commitVisualBeforeDrop)
        CommitDragVisualEndBeforeShellOperation();

    {
        const int mods = dropPreviewMods;

        if (dragSession_.TargetRegion() == HitRegion::Handoff && dragSession_.TargetSlot()
            && dragSession_.TargetSlot()->GetItem())
        {
            Item* targetItem = dragSession_.TargetSlot()->GetItem();
            const bool dockFolderPopupTarget =
                IsOpenDockFolderPopupDropTarget(
                    dragSession_.TargetContainer(),
                    targetItem);
            const bool dockFolderPopupSource =
                dockFolderPopupOpen_ &&
                dragSession_.Source() ==
                    dockFolderPopupContainer_.get();
            bool explicitDockFolderTarget =
                dockFolderPopupTarget;
            if (auto* dockTarget = dynamic_cast<DockEntryItem*>(targetItem))
            {
                const size_t targetEntryIndex =
                    dockTarget->GetEntryIndex();
                explicitDockFolderTarget =
                    targetEntryIndex <
                        dockEntries_.size() &&
                    IsFolderDockEntry(
                        dockEntries_[
                            targetEntryIndex]);
                if (dynamic_cast<DockContainer*>(
                        dragSession_.Source()) &&
                    targetEntryIndex <
                        dockEntries_.size() &&
                    IsRecycleBinDockEntry(
                        dockEntries_[targetEntryIndex]) &&
                    RemoveDockDragOutItems(
                        dragSession_.Items()))
                {
                    ClearSelection();
                    EndDragSession();
                    SaveLayoutSlots();
                    RebuildContainersAndItems();
                    LayoutItems();
                    InvalidateRect(
                        hwnd_, nullptr, FALSE);
                    goto cleanup;
                }
                if (dockTarget->GetEntryType() == DockEntryType::Collection)
                {
                    const bool executed = DropItemsIntoDockCollection(
                        dragSession_.Items(), dragSession_.Source(), dockTarget, mods);
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    if (executed)
                    {
                        RebuildContainersAndItems();
                        LayoutItems();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    goto cleanup;
                }
            }
            auto* targetDesktopIcon = dynamic_cast<DesktopIcon*>(targetItem);
            DesktopItem* targetDesktopItem = targetDesktopIcon
                ? targetDesktopIcon->GetDesktopItem() : nullptr;
            if (dynamic_cast<DockContainer*>(dragSession_.Source()) && targetDesktopItem &&
                _wcsicmp(targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0)
            {
                MoveDockItemsToDesktop(dragSession_.Items(),
                    CellFromPointForDrag(dragSession_.CurrentPoint()));
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
            const std::vector<std::wstring> sourcePaths =
                dragSession_.SourceList().FilePaths();
            const bool fullyPathBackedSource =
                !sourcePaths.empty() &&
                sourcePaths.size() ==
                    dragSession_.SourceList().entries.size();
            const std::wstring targetPath = targetItem->GetPath();
            const DWORD targetAttributes = targetPath.empty()
                ? INVALID_FILE_ATTRIBUTES
                : GetFileAttributesW(targetPath.c_str());
            if (!sourcePaths.empty() &&
                targetAttributes != INVALID_FILE_ATTRIBUTES &&
                (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                DWORD keyState = MK_LBUTTON;
                if (mods & MK_CONTROL) keyState |= MK_CONTROL;
                if (mods & MK_ALT) keyState |= MK_ALT;
                if (mods & MK_SHIFT) keyState |= MK_SHIFT;
                const DWORD selectedEffect = ChooseDropEffect(
                    keyState,
                    DROPEFFECT_COPY |
                        DROPEFFECT_MOVE |
                        DROPEFFECT_LINK);
                const DropAction action =
                    selectedEffect == DROPEFFECT_LINK
                        ? DropAction::Link
                        : selectedEffect == DROPEFFECT_COPY
                            ? DropAction::Copy
                            : DropAction::Move;
                DragSourceList fileSources =
                    dragSession_.SourceList();
                auto finished = [this,
                    dockFolderPopupTarget,
                    dockFolderPopupSource](bool succeeded) {
                    if (!succeeded)
                        return;
                    ReloadItems(false);
                    if ((dockFolderPopupTarget ||
                         dockFolderPopupSource) &&
                        dockFolderPopupOpen_)
                        RefreshDockFolderPopup();
                };
                if (MaterializeFilesToFolder(
                        fileSources, targetPath, action,
                        std::move(finished)))
                {
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    goto cleanup;
                }
            }
            DWORD shellKeyState = MK_LBUTTON;
            if (mods & MK_CONTROL) shellKeyState |= MK_CONTROL;
            if (mods & MK_ALT) shellKeyState |= MK_ALT;
            if (mods & MK_SHIFT) shellKeyState |= MK_SHIFT;
            if (explicitDockFolderTarget &&
                (mods & (MK_CONTROL | MK_ALT | MK_SHIFT)) == 0)
                shellKeyState |= MK_SHIFT;
            POINT shellPoint = dragSession_.CurrentPoint();
            ClientToScreen(hwnd_, &shellPoint);
            if (fullyPathBackedSource && !targetPath.empty() &&
                QueueShellDrop(
                    sourcePaths,
                    targetPath,
                    shellKeyState,
                    POINTL{ shellPoint.x, shellPoint.y },
                    DROPEFFECT_COPY | DROPEFFECT_MOVE |
                        DROPEFFECT_LINK,
                    [this,
                     dockFolderPopupTarget,
                     dockFolderPopupSource](bool succeeded) {
                        if (!succeeded)
                            return;
                        if ((dockFolderPopupTarget ||
                             dockFolderPopupSource) &&
                            dockFolderPopupOpen_)
                            RefreshDockFolderPopup();
                    }))
            {
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
            // Only non-path or queue-rejection compatibility cases reach this
            // synchronous Shell fallback.
            DwmFlush();
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                ComPtr<IDropTarget> dropTarget;
                if (auto* targetIcon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* desktopItem = targetIcon->GetDesktopItem();
                    if (desktopItem && desktopItem->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(desktopItem->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dropTarget.GetAddressOf()));
                    }
                }
                else if (!targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dropTarget));
                    }
                }

                if (dropTarget)
                {
                    POINT screen = dragSession_.CurrentPoint();
                    ClientToScreen(hwnd_, &screen);
                    POINTL spl{ screen.x, screen.y };
                    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    if (SUCCEEDED(dropTarget->DragEnter(
                            dataObj.Get(), shellKeyState, spl, &effect)))
                    {
                        dropTarget->DragOver(
                            shellKeyState, spl, &effect);
                        dropTarget->Drop(
                            dataObj.Get(), shellKeyState, spl, &effect);
                    }
                }
            }
            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            if (dockFolderPopupTarget ||
                dockFolderPopupSource)
                RefreshDockFolderPopup();
            ReloadItems();
            goto cleanup;
        }

        Container* targetContainer = dragSession_.TargetContainer();
        bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
        const bool dockFolderPopupTarget =
            IsOpenDockFolderPopupDropTarget(
                targetContainer,
                dragSession_.TargetSlot()
                    ? dragSession_.TargetSlot()->GetItem()
                    : nullptr);
        const bool dockFolderPopupSource =
            dockFolderPopupOpen_ &&
            dragSession_.Source() ==
                dockFolderPopupContainer_.get();
        targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
            dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

        SaveLayoutSlots();
        ClearSelection();
        EndDragSession();
        if (dockFolderPopupTarget ||
            dockFolderPopupSource)
            RefreshDockFolderPopup();
        if (needsReload)
        {
            RebuildContainersAndItems();
            ReloadItems();
        }
        else
        {
            // 内容变更可能使某些溢出页变空（后面有非空页时应立即清理顺延）
            // 先 ApplyPageMapping（可能重排 pageId），再 RebuildContainersAndItems + LayoutItems
            ApplyPageMapping();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

cleanup:
    EndDragSession();
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    popupDwellController_.Reset();
    KillTimer(hwnd_, kCollectionPopupDwellTimerId);
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    ReleaseCapture();
}
