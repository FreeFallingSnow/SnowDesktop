#include "ui/input/private_pointer.h"
#include "dock/dock_tray_rules.h"
#include "dock/floating_dock_rules.h"
#include "app/app.h"
#include "dock/dock_window_rules.h"
#include "dock/dock_window_preview.h"
#include "diagnostics/diagnostic_log.h"
#include "diagnostics/performance_trace.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "app/dock/persistent_dock_host.h"
#include "dock/dock_settings.h"
#include "app/popup/categorized_popup_scope.h"
#include "app/layout/interactive_layout_save.h"
#include "app/dock/dock_taskbar_diagnostics.h"
#include "desktop/desktop_hover_rules.h"
#include "drag_drop/drag_input_rules.h"
#include "steam/steam_app_identity.h"
#include "widget/runtime/widget_visibility_rules.h"
#include "widget/view/widget_composition_layer_rules.h"
#include "widget/view/widget_scroll_rules.h"
#include "widgets/guide_widget.h"
#include "layout/popup_scrollbar_layout.h"

#include <utility>

// Pointer leave, Dock click release and primary-button drag completion.

namespace
{
bool RetainStartButtonHover(const auto& hosts, HMONITOR monitor, int left, int top)
{
    POINT cursor{};
    if (!monitor || !snowdesktop::private_pointer::CursorPosition(&cursor)) return false;
    cursor.x -= left; cursor.y -= top;
    return std::any_of(hosts.begin(), hosts.end(), [&](const auto& host) {
        return host && host->active && host->monitor == monitor && host->container &&
            host->container->IsWindowsButtonPoint(cursor);
    });
}

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

RECT DesktopApp::GetDesktopHoverDamage(POINT point, const DockContainer* dock) const
{
    RECT dirty{};
    if (point.x == LONG_MIN && point.y == LONG_MIN) return dirty;
    const auto include = [&](RECT bounds, LONG padding) {
        if (IsRectEmpty(&bounds)) return;
        InflateRect(&bounds, padding, padding);
        UnionRect(&dirty, &dirty, &bounds);
    };
    for (const auto& item : items_)
    {
        const RECT frame = item.largeIcon ? GetLargeIconFrameRect(item) : item.bounds;
        if (PtInRect(&frame, point)) include(frame, 8);
    }
    for (const auto& widget : widgets_)
    {
        const RECT frame = GetStandaloneWidgetFrameRect(widget);
        const RECT hoverFrame = GetDesktopWidgetHoverFrameRect(widget, frame);
        if (PtInRect(&frame, point) || PtInRect(&hoverFrame, point))
            include(frame, snowdesktop::widget_composition_layer_rules::kWidgetSurfaceBorderOverdraw);
    }
    if (dock && !IsDockHostedByPersistentHost(dock))
        include(snowdesktop::floating_dock_rules::ExpandHostForTitleLayer(
            dock->GetInteractiveBounds(), dockSettings_.position), 4);
    if (GetOpenPopupWidget() && !IsCollectionPopupHostedByFloatingWindow() && PtInRect(&popupRect_, point))
        include(popupRect_, 6);
    if (!luaWidgetPanelRequest_.widgetId.empty() && !ShouldShowFloatingPopupWindow() &&
        PtInRect(&luaWidgetPanelRect_, point))
        include(luaWidgetPanelRect_, 6);
    if (IsPointInUsageGuide(point)) include(usageGuideFrame_, 4);
    RECT left{}, right{};
    GetNavHotEdgeRects(left, right);
    if (PtInRect(&left, point)) include(left, 4);
    if (PtInRect(&right, point)) include(right, 4);
    return dirty;
}

void DesktopApp::PresentPassiveHoverVisualChange(POINT previousHover)
{
    snowdesktop::performance::Scope trace("input", "hover.passive.present");
    RecordShellHoverTrace(ShellHoverTraceEvent::PassivePresent);
    const RECT oldBounds = GetDesktopHoverDamage(previousHover, GetDockContainerAtPoint(previousHover));
    UpdateWidgetHoverExpansion(lastMousePoint_);
    const RECT newBounds = GetDesktopHoverDamage(lastMousePoint_, GetDockContainerAtPoint(lastMousePoint_));
    if (hwnd_)
    {
        const auto present = [&](const RECT& bounds) {
            if (IsRectEmpty(&bounds)) return;
            InvalidateRect(hwnd_, &bounds, FALSE);
            if (!compositionPaintInProgress_) UpdateWindow(hwnd_);
        };
        RECT overlap{}, both{};
        if (IntersectRect(&overlap, &oldBounds, &newBounds))
        {
            UnionRect(&both, &oldBounds, &newBounds);
            present(both);
        }
        else
        {
            // Windows returns one bounding box for all outstanding damage.
            // Present disjoint hover targets separately when jumping between displays.
            present(oldBounds);
            present(newBounds);
        }
        if (IsRectEmpty(&oldBounds) && IsRectEmpty(&newBounds))
        {
            RECT client{};
            GetClientRect(hwnd_, &client);
            const RECT tooltip = desktopForegroundTooltipDamage_.IncludePrevious({}, client);
            if (!IsRectEmpty(&tooltip)) (void)PresentDesktopForegroundComposition(tooltip);
        }
        SyncDockTitleTooltip();
        SyncPageNavHotEdgeTooltip();
        SyncLuaWidgetTooltip();
    }
    InvalidateFloatingDockWindow(true);
    InvalidateFloatingPopupWindow(true);
}

bool DesktopApp::PresentCollectionPopupHoverChange(POINT previousHover, bool allowResize)
{
    if (dragSession_.HasContext() || dragDropController_.IsTransportActive())
        return false;

    const bool floatingCollection = IsCollectionPopupHostedByFloatingWindow();
    const bool resizing = allowResize && floatingCollection && GetCollectionPopupResizeTransition();
    const auto* previousDock = GetDockContainerAtPoint(previousHover);
    const auto* currentDock = GetDockContainerAtPoint(lastMousePoint_);
    if (!resizing)
    {
        if (mouseDown_ || widgetAction_ != WidgetAction::None || marqueeActive_ || middleButtonWidgetMove_ ||
            detailColumnResizeActive_ || luaWidgetPanelMouseDown_ || snowdesktop::private_pointer::Capture() ||
            desktopBackdropFullCollectionPending_ || HasActiveContextMenuSession() || IsLuaStackIndicatorVisible() ||
            (floatingCollection && (!popupAnchoredToDock_ || !collectionPopupDockHost_ || !collectionPopupDockHost_->active)) ||
            (!floatingCollection && (folderPreview_ || ShouldShowFloatingPopupWindow())))
            return false;

        if (!floatingCollection && !previousDock && !currentDock) return false;
        const auto collectionSurface = [&](POINT point, const DockContainer* dock) {
            if (floatingCollection && IsPointOccludedByOpenPopup(point)) return true;
            if (!dock || !IsDockHostedByPersistentHost(dock)) return false;
            if (floatingCollection) return dock == collectionPopupDockHost_->container;
            // The close completion has released the popup owner. Only a known
            // collection entrance can identify the returning Dock hover here.
            const auto* entry = dock->EntryAtPoint(point);
            if (!entry || entry->GetEntryIndex() >= dockEntries_.size()) return false;
            return IsLogicalDockEntryType(entry->GetEntryType()) || IsFolderDockEntry(dockEntries_[entry->GetEntryIndex()]);
        };
        const bool previousOwned = collectionSurface(previousHover, previousDock);
        const bool currentOwned = collectionSurface(lastMousePoint_, currentDock);
        const auto cleared = [](POINT point) { return point.x == LONG_MIN && point.y == LONG_MIN; };
        // Another input HWND can send its leave after the previous one has
        // already cleared the shared desktop hover for this Dock popup.
        if (!previousOwned && !currentOwned &&
            !(floatingCollection && cleared(previousHover) && cleared(lastMousePoint_)))
            return false;
        const auto emptyDesktop = [&](POINT point, const DockContainer* dock) {
            if (cleared(point)) return true;
            RECT left{}, right{};
            GetNavHotEdgeRects(left, right);
            return !dock && !IsPointInUsageGuide(point) && !PtInRect(&left, point) && !PtInRect(&right, point) &&
                !HitTestIcon(point) && !IsPointOverWidgetChrome(point);
        };
        if ((!previousOwned && !emptyDesktop(previousHover, previousDock)) ||
            (!currentOwned && !emptyDesktop(lastMousePoint_, currentDock)))
            return false;
        // Hover-only frames can extend beyond their present hit target. Keep
        // the visibility transaction when such a frame is entered or left.
        for (const auto& widget : widgets_)
        {
            if (!widget.showOnHoverOnly) continue;
            const RECT frame = GetDesktopWidgetHoverFrameRect(widget, GetStandaloneWidgetFrameRect(widget));
            if ((PtInRect(&frame, previousHover) != FALSE) != (PtInRect(&frame, lastMousePoint_) != FALSE))
                return false;
        }
    }

    snowdesktop::performance::Scope present("input", resizing ? "hover.popup_resize.present" : "hover.dock_popup.present");
    const auto invalidateHover = [&](POINT point, const DockContainer* dock) {
        const RECT dirty = GetDesktopHoverDamage(point, dock);
        if (hwnd_ && !IsRectEmpty(&dirty)) InvalidateRect(hwnd_, &dirty, FALSE);
    };
    // Passive window handoffs and timer samples change the same two hover
    // footprints. Leave their paint pending while the popup presents its frame.
    invalidateHover(previousHover, previousDock);
    if (lastMousePoint_.x != previousHover.x || lastMousePoint_.y != previousHover.y)
        invalidateHover(lastMousePoint_, currentDock);
    UpdateWidgetHoverExpansion(lastMousePoint_);
    UpdateLargeIconHover();
    if (floatingCollection) InvalidateFloatingPopupWindow(false);

    const auto* presentedDock = floatingDockHoverTargetOwner_;
    if (!currentDock || currentDock != presentedDock)
    {
        floatingDockHoverTargetOwner_ = nullptr;
        floatingDockHoverTargetIndex_ = 0;
        floatingDockHoverTargetKind_ = 0;
        if (floatingDockHoverTailToken_)
            uiAnimationScheduler_.Cancel(std::exchange(floatingDockHoverTailToken_, 0));
    }
    for (const auto& host : persistentDockHosts_)
        if (host && host->container && (host->container == previousDock ||
                host->container == currentDock || host->container == presentedDock))
            UpdateFloatingDockWindowBounds(*host, true);
    if (!resizing)
    {
        RECT client{};
        if (GetClientRect(hwnd_, &client))
        {
            const RECT tooltip = desktopForegroundTooltipDamage_.IncludePrevious({}, client);
            if (!IsRectEmpty(&tooltip)) InvalidateRect(hwnd_, &tooltip, FALSE);
        }
        SyncDockTitleTooltip();
        SyncLuaWidgetTooltip();
    }
    return true;
}

void DesktopApp::OnMouseLeave()
{
    snowdesktop::performance::Scope trace("input", "hover.mouse_leave");
    UpdateLuaStackIndicator(POINT{LONG_MIN, LONG_MIN});
    POINT popupPoint{};
    if (hwnd_ && IsWindow(hwnd_) && snowdesktop::private_pointer::CursorPosition(&popupPoint) &&
        ScreenToClient(hwnd_, &popupPoint))
    {
        DockTrayPopupPointerMove(popupPoint);
        UpdatePopupCloseOnMouseLeave(popupPoint);
    }
    else DockTrayPopupPointerMove({LONG_MIN, LONG_MIN});
    POINT hoverPoint{};
    const bool hoverPointAvailable = TryGetDesktopHoverPointFromCursor(hoverPoint);
    if (hoverPointAvailable) UpdatePopupHover(hoverPoint);
    else CancelPopupHover();
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseLeaveBegin);
    POINT hoverScreenPoint = hoverPoint;
    const HWND hoverWindow = hoverPointAvailable &&
        ClientToScreen(hwnd_, &hoverScreenPoint)
        ? snowdesktop::private_pointer::WindowAtPoint(hoverScreenPoint) : nullptr;
    const HWND hoverRoot = hoverWindow
        ? GetAncestor(hoverWindow, GA_ROOT) : nullptr;
    // A leave belongs to an HWND, not the logical Dock. Capture release or
    // insertion can transfer the pointer to its replacement content/backdrop
    // before that HWND's move arrives. Popups and menus remain separate input
    // surfaces even when their screen rectangle overlaps the Dock.
    const bool pointerOnDockSurface = hoverWindow &&
        (IsBaseDesktopHoverSurfaceWindow(hoverWindow) ||
         IsPersistentDockHostWindow(hoverWindow) ||
         IsPersistentDockHostWindow(hoverRoot) ||
         IsPersistentDockBackdropWindow(hoverWindow) ||
         IsPersistentDockBackdropWindow(hoverRoot));
    const bool retainDockHover = pointerOnDockSurface &&
        GetDockContainerAtPoint(hoverPoint);
    if ((IsDockStartMenuActive() && RetainStartButtonHover(
            persistentDockHosts_, dockStartMenuHeldMonitor_, virtualLeft_, virtualTop_)) ||
        retainDockHover)
    {
        if (retainDockHover) lastMousePoint_ = hoverPoint;
        // Retain the live sample without replaying input or painting. The
        // rebuilt Dock resolves its current item from that sample, so newly
        // inserted content can take over the existing magnification progress.
        RecordShellHoverTrace(
            ShellHoverTraceEvent::MouseLeaveEnd);
        return;
    }
    POINT cursorScreen{};
    const bool cursorValid = snowdesktop::private_pointer::CursorPosition(&cursorScreen) != FALSE;
    if (dockWindowPreview_ && dockWindowPreview_->IsVisible())
    {
        if (dockWindowPreviewHover_.TimerArmed() &&
            dockWindowPreviewHover_.UpdateTarget({}, true, false).cancelTimer)
            KillTimer(hwnd_, kDockWindowPreviewHoverTimerId);
        dockWindowPreview_->ScheduleHide();
    }
    else
        HideDockWindowPreview();
    const int previousNavSide = navHoverSide_;
    const bool clearedDesktopHover = previousNavSide != 0 || navHotEdgeHover_ ||
        (!listTitleHoverPopup_ && !listTitleHoverOwner_.empty());
    SetPageNavHotEdgeHover(0);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    POINT retainedDragPoint{};
    const bool preserveDragDwell =
        (dragSession_.IsActive() ||
         dragDropController_.IsTransportActive()) &&
        TryGetDesktopHoverPointFromCursor(retainedDragPoint);
    if (preserveDragDwell)
    {
        // WM_MOUSELEAVE belongs to one native HWND, while collection dwell
        // belongs to the complete SnowDesktop interaction surface. A desktop
        // icon returning from OLE and an external OLE source may have no local
        // capture, so crossing a backdrop, Dock host, or popup HWND must not
        // erase the candidate that a component-origin drag can retain.
        UpdateCollectionPopupDwell(retainedDragPoint);
        UpdateCollectionGroupTabDwell(retainedDragPoint);
    }
    else
    {
        CancelCollectionPopupDwell();
        CancelCollectionGroupTabDwell();
    }
    ResetDockHandoffDwell();

    // Capture-based dragging continues to receive coordinates outside the
    // window. Preserve that pointer state, but clear passive hover immediately.
    // The preview owns its independent screen-space transition triangle, so
    // Dock magnification can still be reset while the preview stays reachable.
    const HWND captureWindow = snowdesktop::private_pointer::Capture();
    const bool ownsInteractionCapture =
        snowdesktop::desktop_hover_rules::
            OwnsInteractionCapture(
                captureWindow,
                hwnd_, floatingDockHwnd_) ||
        IsPersistentDockHostWindow(captureWindow) ||
        (captureWindow != nullptr &&
         captureWindow == floatingPopupHwnd_);
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
        wchar_t phase[160]{};
        swprintf_s(phase, L"Passive hover cleared by mouse-leave: previous=%ld,%ld cursor=%ld,%ld cursorValid=%d",
            lastMousePoint_.x, lastMousePoint_.y, cursorScreen.x, cursorScreen.y,
            cursorValid ? 1 : 0);
        snowdesktop::dock_taskbar_diagnostics::Record(phase);
        const POINT previousHover = lastMousePoint_;
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
        ClearListTitleHover();
        if (widgetEngine_)
            widgetEngine_->ClearInteractionHover();
        if ((clearedDesktopHover && !GetCollectionPopupResizeTransition()) ||
            !PresentCollectionPopupHoverChange(previousHover))
        {
            // Shrink the title's input island before presenting the cleared frame.
            UpdateFloatingDockWindowBounds(false);
            PresentPassiveHoverVisualChange(previousHover);
        }
        RecordShellHoverTrace(
            ShellHoverTraceEvent::MouseLeaveEnd);
        return;
    }
    if (hwnd_ && luaWidgetPanelMouseDown_ && captureWindow &&
        captureWindow == luaWidgetPanelCaptureHwnd_ &&
        !dragSession_.HasContext() && !dragDropController_.IsTransportActive())
    {
        // The panel still receives the captured move and up when the pointer
        // crosses its HWND. Only desktop chrome cleared above needs erasing.
        if (previousNavSide)
        {
            RECT left{}, right{};
            GetNavHotEdgeRects(left, right);
            InvalidateRect(hwnd_, previousNavSide < 0 ? &left : &right, FALSE);
        }
    }
    else if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
    RecordShellHoverTrace(
        ShellHoverTraceEvent::MouseLeaveEnd);
}

void DesktopApp::ReconcileDesktopHoverState(
    snowdesktop::desktop_hover_rules::ReconcileMode mode)
{
    snowdesktop::performance::Scope trace("input", "hover.reconcile");
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
    const bool foregroundChanged = foregroundTick != 0 &&
        foregroundTick != desktopHoverForegroundObservedTick_;
    if (foregroundChanged)
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
    if (HasActiveContextMenuSession())
    {
        // Modal menus can consume the original WM_MOUSELEAVE. Keep this
        // deactivation path live without replaying passive hover into a menu.
        if (lastMousePoint_.x != LONG_MIN || lastMousePoint_.y != LONG_MIN)
            OnMouseLeave();
        desktopHoverForegroundObservedTick_ = foregroundTick;
        RecordShellHoverTrace(ShellHoverTraceEvent::ReconcileSuspended);
        return;
    }
    if ((IsDockStartMenuActive() && RetainStartButtonHover(
            persistentDockHosts_, dockStartMenuHeldMonitor_, virtualLeft_, virtualTop_)) ||
        !snowdesktop::desktop_hover_rules::
            ShouldReconcileFromSurfaceSample(
                shellPopupMenuLayerDepth_ > 0,
                shellDialogOwnerAvailable,
                !shellDialogOwnerAvailable ||
                    IsWindowEnabled(shellDialogOwner)))
    {
        // Modern menus own a separate input surface; native Shell menus and
        // commands run private modal loops. WindowFromPoint can alternate
        // between a menu/dialog shadow and the desktop during restacking.
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
        POINT dockScreenPoint = cursorPoint;
        const HWND dockHitWindow = ClientToScreen(hwnd_, &dockScreenPoint)
                ? snowdesktop::private_pointer::WindowAtPoint(dockScreenPoint) : nullptr;
        const bool pointerOnBaseDesktopSurface =
            IsBaseDesktopHoverSurfaceWindow(dockHitWindow);
        const PersistentDockHost* pointerDockHost = nullptr;
        for (const auto& host : persistentDockHosts_)
        {
            if (host && ShouldShowPersistentDockHost(*host) &&
                (dockHitWindow == host->hwnd ||
                    host->backdrop.IsBackdropWindow(dockHitWindow)) &&
                snowdesktop::floating_dock_rules::IsPointInVisibleLayer(
                    cursorPoint, host->dockRect,
                    host->popupRect, RECT{}))
            {
                pointerDockHost = host.get();
                break;
            }
        }
        const bool pointerOnActiveHoverSurface =
            snowdesktop::desktop_hover_rules::IsActiveHoverSampleSurface(
                pointerOnBaseDesktopSurface,
                IsPersistentDockHostWindow(dockHitWindow) ||
                    IsPersistentDockBackdropWindow(dockHitWindow),
                pointerDockHost != nullptr);
        const POINT activeCursorPoint = cursorPoint;
        const bool pointerPositionChanged =
            pointerOnActiveHoverSurface &&
            (activeCursorPoint.x != lastMousePoint_.x ||
                activeCursorPoint.y != lastMousePoint_.y);
        const bool widgetInteractionActive =
            middleButtonWidgetMove_ ||
            widgetAction_ != WidgetAction::None ||
            detailColumnResizeActive_ ||
            luaWidgetPanelMouseDown_;
        const bool passiveHoverAllowed =
            snowdesktop::private_pointer::Capture() == nullptr &&
            snowdesktop::desktop_hover_rules::
                ShouldResamplePassiveMouseMove(
                    mouseDown_,
                    dragSession_.IsActive(),
                    widgetInteractionActive);
        const bool activateHover = passiveHoverAllowed &&
            snowdesktop::desktop_hover_rules::ShouldActivateFromSurfaceSample(
                pointerOnActiveHoverSurface, passiveHoverCleared, mode,
                foregroundSettled);
        const bool refreshActiveHover =
            snowdesktop::desktop_hover_rules::
                ShouldRefreshActiveHoverFromSurfaceSample(
                    pointerOnActiveHoverSurface,
                    passiveHoverCleared,
                    pointerPositionChanged,
                    passiveHoverAllowed,
                    mode,
                    foregroundSettled);
        // A sampled acquisition does not deliver WM_MOUSEMOVE, so it must
        // restore the leave subscription itself. Do not track Dock content
        // while a backdrop owns the hit: that would generate repeated leaves.
        if (passiveHoverAllowed && pointerOnActiveHoverSurface &&
            (dockHitWindow == hwnd_ || IsPersistentDockHostWindow(dockHitWindow)))
        {
            TRACKMOUSEEVENT tracking{ sizeof(tracking) };
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = dockHitWindow;
            snowdesktop::private_pointer::TrackLeave(&tracking);
        }
        if (activateHover || refreshActiveHover)
        {
            const bool sameDockHover = refreshActiveHover && pointerDockHost &&
                pointerDockHost->container &&
                floatingDockHoverTargetOwner_ == pointerDockHost->container &&
                PtInRect(&pointerDockHost->dockRect, lastMousePoint_) &&
                PtInRect(&pointerDockHost->dockRect, activeCursorPoint) &&
                !dragDropController_.IsTransportActive() &&
                !ShouldShowFloatingPopupWindow();
            const POINT previousHover = lastMousePoint_;
            lastMousePoint_ = activateHover
                ? cursorPoint : activeCursorPoint;
            UpdateListTitleHover();
            if (sameDockHover)
            {
                // Recover this sample through the normal rate-limited Dock path.
                UpdateWidgetHoverExpansion(lastMousePoint_);
                UpdateDockWindowPreview(lastMousePoint_);
                PresentPointerInteractionFrame();
            }
            else if (!PresentCollectionPopupHoverChange(previousHover))
            {
                // On a surface handoff, synchronize both the old and new
                // regions so stale visuals cannot outlive the hover state.
                UpdateFloatingDockWindowBounds(false);
                PresentPassiveHoverVisualChange(previousHover);
            }
            RecordShellHoverTrace(
                activateHover
                    ? ShellHoverTraceEvent::ReconcileActivate
                    : ShellHoverTraceEvent::ReconcileRefresh,
                lastMousePoint_);
        }
        else
        {
            RecordShellHoverTrace(
                ShellHoverTraceEvent::ReconcileNoChange,
                cursorPoint);
        }
        return;
    }

    const HWND captureWindow = snowdesktop::private_pointer::Capture();
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
    const bool hoverAlreadyCleared = pointerAlreadyCleared && navHoverSide_ == 0 && !previewVisible;
    const bool foregroundAlreadyReconciled = foregroundTick != 0 && !foregroundChanged;
    desktopHoverForegroundObservedTick_ = foregroundTick;

    if (hoverAlreadyCleared && foregroundAlreadyReconciled)
        return;

    if (hoverAlreadyCleared && foregroundChanged &&
        listTitleHoverOwner_.empty() && !dragDropController_.IsTransportActive() &&
        !IsCollectionPopupHostedByFloatingWindow() && luaWidgetPanelRequest_.widgetId.empty())
    {
        // Window placement is repaired above. Reuse the cleared desktop
        // frame while cancelling any pending Dock preview on a new handoff.
        snowdesktop::performance::Scope skipped("input", "hover.reconcile.already_clear");
        HideDockWindowPreview();
        return;
    }

    // Reconcile through the current preview generation. A foreground handoff
    // must not manufacture a leave before its first native pointer entry.
    if (previewVisible) dockWindowPreview_->ScheduleHide();
    else HideDockWindowPreview();
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
            HasFocusedFolderPreviewSearch(&widget) ||
            (!interactionPinnedWidgetId_.empty() &&
                interactionPinnedWidgetId_ == widget.id) ||
            snowdesktop::widget_visibility_rules::
                ShouldRetainForKeyboardNavigation(
                    keyboardNavVisualFocus_,
                    keyboardNavInsideWidget_,
                    keyboardNavWidgetIndex_,
                    widgetIndex);
        const RECT frame = GetDesktopWidgetHoverFrameRect(
            widget, GetStandaloneWidgetFrameRect(widget));
        if (snowdesktop::widget_visibility_rules::
                ShouldRenderWidget(
                    true,
                    dragSession_.IsActive(),
                    dragDropController_.IsExternalDragActive(),
                    widgetAction_ == WidgetAction::Move,
                    widget.selected,
                    HasSelectedFilesInWidget(widgetIndex),
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
    snowdesktop::private_pointer::CursorPosition(&entry.cursorScreen);
    entry.hoverOnlyVisibleMask =
        HoverOnlyVisibleMask();
    entry.foregroundWindow = snowdesktop::private_pointer::ForegroundWindow();
    entry.captureWindow = snowdesktop::private_pointer::Capture();
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
        case ShellHoverTraceEvent::ReconcileRefresh: return L"reconcile-refresh";
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
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Input,
        report.c_str(),
        DiagnosticLogLevel::Debug);
}

bool DesktopApp::HandleDockClickRelease(POINT point)
{
    // Crossing the drag threshold owns the complete gesture, including a
    // release back on its source or at an unchanged insertion boundary.
    if (dragSession_.HasContext() || dragDropController_.IsTransportActive()) return false;
    DockContainer* dock = dockPressedContainer_;
    if (!dock) dock = GetDockContainerAtPoint(point);
    if (!dock) return false;
    PersistentDockHost* requestedDockHost =
        FindPersistentDockHost(dock);

    DockEntryType entryType = DockEntryType::DesktopItem;
    std::wstring reference;
    size_t frequentItemIndex = static_cast<size_t>(-1);
    std::wstring runningAppKey;
    const size_t pressedEntryIndex = dockPressedEntry_;
    const size_t pressedFrequentItemIndex = dockPressedFrequentItem_;
    const auto pressedWindowAction = dockPressedWindowAction_;
    const HWND pressedTargetWindow = dockPressedTargetWindow_;
    const bool pressedClosedCollectionPopup =
        dockPressedClosedCollectionPopup_;
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
        !IsLogicalDockEntryType(dockEntries_[pressedEntryIndex].type) &&
        IsFolderDockEntry(dockEntries_[pressedEntryIndex]);

    size_t appItemIndex = frequentItemIndex;
    if (appItemIndex >= items_.size() && entryType == DockEntryType::DesktopItem)
        appItemIndex = FindItemIndexByKey(reference);
    bool waitForDoubleClick = false;
    if (appItemIndex < items_.size() && !folderEntry)
        waitForDoubleClick =
            snowdesktop::dock_window_rules::ShouldWaitForDockLaunchDoubleClick(
                dockSettings_.singleClickLaunchItems,
                pressedWindowAction);
    const bool matchingPendingDoubleClick =
        snowdesktop::dock_window_rules::
            IsMatchingPendingDockDoubleClickRelease(
                pressedEntryIndex,
                pressedFrequentItemIndex,
                dockPendingDoubleClickEntry_,
                dockPendingDoubleClickFrequentItem_,
                GetTickCount() -
                    dockPendingDoubleClickTick_,
                GetDoubleClickTime());

    // 在激活外部应用前完整结束本次桌面交互，避免鼠标捕获、选择高亮或
    // 拖拽预览跨到新前台窗口后仍残留。
    const bool popupEntryOnDockHost = (IsLogicalDockEntryType(entryType) || folderEntry) &&
        IsDockHostedByPersistentHost(dock);
    EndDragSession(!popupEntryOnDockHost);
    HideDragHintWindow();
    const bool selectionCleared = !waitForDoubleClick && ClearSelection();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    ClearDockPressedState();
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    SetPageNavHotEdgeHover(0);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    snowdesktop::private_pointer::ReleasePointerCapture();
    InvalidateDragStaticScene(!popupEntryOnDockHost || selectionCleared);
    if (popupEntryOnDockHost && selectionCleared)
    {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateFloatingDockWindow(false);
    }
    else if (hwnd_ && !popupEntryOnDockHost)
    {
        if (IsDockContainerEffectivelyFloating(dock))
            InvalidateFloatingDockWindow(true);
        else
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
    }

    // A click pair can cross Dock HWNDs when the persistent Host changes its
    // selected monitor or Z-order band. Windows then emits two ordinary click
    // sequences instead of WM_LBUTTONDBLCLK. Complete the same action on the
    // second release for folders and closed items using double-click launch.
    // In single-click mode the first release already launched the item, so
    // consume the second release without launching it again.
    const size_t trayTargetCount = appItemIndex < items_.size() && !folderEntry ? DockTrayTargetCount(appItemIndex) : 0;
    if (trayTargetCount)
    {
        if (matchingPendingDoubleClick)
        {
            dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
            dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
            dockPendingDoubleClickTick_ = 0;
            if (snowdesktop::dock_tray::ResolveClick(false, trayTargetCount, dockSettings_.singleClickLaunchItems, true) ==
                snowdesktop::dock_tray::ClickAction::ActivateTray)
                TryActivateDockTray(ResolveDockAppIdentity(appItemIndex).executablePath, appItemIndex, pressedAnchorScreen);
            return true;
        }
        dockPendingDoubleClickEntry_ = pressedEntryIndex;
        dockPendingDoubleClickFrequentItem_ = pressedFrequentItemIndex;
        dockPendingDoubleClickTick_ = GetTickCount();
        const auto trayClick = snowdesktop::dock_tray::ResolveClick(false, trayTargetCount, dockSettings_.singleClickLaunchItems, false);
        if (trayClick == snowdesktop::dock_tray::ClickAction::ActivateTray || trayClick == snowdesktop::dock_tray::ClickAction::OpenPicker)
            TryActivateDockTray(ResolveDockAppIdentity(appItemIndex).executablePath, appItemIndex, pressedAnchorScreen);
        return true;
    }
    if (matchingPendingDoubleClick &&
        (folderEntry || waitForDoubleClick || dockSettings_.singleClickLaunchItems))
    {
        dockPendingDoubleClickEntry_ =
            static_cast<size_t>(-1);
        dockPendingDoubleClickFrequentItem_ =
            static_cast<size_t>(-1);
        dockPendingDoubleClickTick_ = 0;
        dockSuppressClickReleaseEntry_ =
            static_cast<size_t>(-1);
        if (folderEntry &&
            pressedEntryIndex < dockEntries_.size())
        {
            const auto target = ResolveDockFolderTarget(
                dockEntries_[pressedEntryIndex]);
            CloseCollectionPopup(false);
            if (target.available)
                shellLaunchWorker_.Enqueue(
                    hwnd_, target.path);
        }
        else if (waitForDoubleClick && appItemIndex < items_.size() &&
            LaunchDesktopItem(appItemIndex, true))
        {
            ClearSelection();
        }
        return true;
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

    if (dockSettings_.singleClickLaunchItems &&
        appItemIndex < items_.size() && !folderEntry &&
        pressedWindowAction == snowdesktop::dock_window_rules::DockClickAction::Launch)
    {
        if (LaunchDesktopItem(appItemIndex, true))
        {
            // Keep the accepted launch identity until the double-click
            // interval expires so either HWND path can consume a second click.
            dockPendingDoubleClickEntry_ = pressedEntryIndex;
            dockPendingDoubleClickFrequentItem_ = pressedFrequentItemIndex;
            dockPendingDoubleClickTick_ = GetTickCount();
        }
        return true;
    }

    if (!runningAppKey.empty())
    {
        const auto running = std::find_if(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& app) {
                return app.presence.Interactive() && app.identityKey == runningAppKey;
            });
        if (running != dockUnpinnedRunningApps_.end())
        {
            const HWND runningWindow = running->window;
            ActivateOrToggleDockWindow(
                runningWindow, pressedWindowAction,
                pressedTargetWindow, pressedAnchorScreen);
        }
    }
    else if (frequentItemIndex < items_.size())
    {
        ActivateOrToggleDockItem(
            frequentItemIndex, pressedWindowAction,
            pressedTargetWindow, pressedAnchorScreen);
    }
    else if (IsLogicalDockEntryType(entryType))
    {
        const size_t widgetIndex = FindWidgetIndexById(reference);
        if (widgetIndex < widgets_.size())
        {
            if (IsCollectionPopupInteractive() &&
                snowdesktop::floating_dock_rules::
                    ShouldCloseCollectionPopup(
                        popupWidgetIndex_,
                        widgetIndex,
                        collectionPopupDockHost_ ==
                            requestedDockHost))
                CloseCollectionPopup();
            else
                OpenCollectionPopupAt(
                    widgetIndex, point, L"",
                    pressedClosedCollectionPopup);
        }
    }
    else if (folderEntry)
    {
        const std::wstring sourceId =
            std::to_wstring(static_cast<int>(entryType)) +
            L":" + ToUpperInvariant(reference);
        if (IsCollectionPopupInteractive() &&
            dockFolderPopupOpen_ &&
            GetDockFolderPopupEntrySourceId() == sourceId &&
            collectionPopupDockHost_ == requestedDockHost)
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
            ActivateOrToggleDockItem(
                itemIndex, pressedWindowAction,
                pressedTargetWindow, pressedAnchorScreen);
        }
    }
    return true;
}

void DesktopApp::OnLeftButtonUpAt(WPARAM wp, POINT upPoint,
    bool rightButtonRelease)
{
    const auto outsidePanelGeneration = std::exchange(luaPanelOutsidePressGeneration_, 0);
    const bool rightDrop = rightButtonItemDrag_;
    if (DockTrayPopupPointerUp(upPoint)) return;
    if (!snowdesktop::drag_input_rules::IsItemDragRelease(
            rightDrop, rightButtonRelease)) return;
    if (auto* view = GetCategorizedPopupView(); view &&
        (view->HasCategoryTabPress() || view->IsSearchPointerSelecting()))
    {
        {
            const RECT popup = GetCollectionPopupRect(*GetOpenPopupWidget());
            CategorizedPopupScope scope(view, GetCategorizedPopupFrame(popup));
            if (view->HasCategoryTabPress())
            {
                view->UpdateCategoryTabDrag(upPoint);
                view->EndCategoryTabDrag(true);
                if (dockFolderPopupOpen_)
                {
                    const size_t source = FindWidgetIndexById(dockFolderPopupMappingWidgetId_);
                    if (source < widgets_.size())
                    {
                        widgets_[source].categoryTabOrder = dockFolderPopupWidget_.categoryTabOrder;
                        widgets_[source].activeCategoryId = dockFolderPopupWidget_.activeCategoryId;
                    }
                }
                RequestLayoutSave();
            }
            else view->FinishSearchPointerSelection(upPoint);
        }
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        snowdesktop::private_pointer::ReleasePointerCapture();
        if (folderPreview_) RefreshDockFolderPopupGeometry();
        if (auto* current = GetCategorizedPopupView(); current && !(folderPreview_ && folderPreview_->inlineBounds))
            InvalidateWidgetControls(*current);
        return;
    }
    for (auto& container : containers_)
        if (auto* categorized = dynamic_cast<ScrollingItemWidget*>(container.get());
            categorized && categorized->HasCategoryTabPress())
        {
            categorized->UpdateCategoryTabDrag(upPoint);
            categorized->EndCategoryTabDrag(true);
            mouseDown_ = false;
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            mouseDownHit_ = nullptr;
            InvalidateWidgetControls(*categorized);
            snowdesktop::private_pointer::ReleasePointerCapture();
            return;
        }
    if (HandleUsageGuidePointerUp(upPoint)) return;
    {
        snowdesktop::InteractiveLayoutSave save(this);
        if (HandleLargeIconPointerUp()) return;
    }
    if (middleButtonWidgetMove_) return;
    if (dragSession_.IsActive())
    {
        // Retain the source and final hit-test context through commit. Only
        // the floating ghost ends here, before any synchronous drop work.
        dragSession_.SetVisualVisible(false);
        HideDragPreviewWindow();
    }
    (void)wp;
    int dropPreviewMods = 0;
    HideDragHintWindow();

    if (popupScrollbarDragging_)
    {
        DesktopWidget* popupWidget = dockFolderPopupOpen_
            ? &dockFolderPopupWidget_
            : (popupWidgetIndex_ < widgets_.size()
                ? &widgets_[popupWidgetIndex_] : nullptr);
        if (popupWidget && IsCollectionPopupInteractive())
        {
            const RECT popup = GetCollectionPopupRect(*popupWidget);
            const auto [viewport, visible] = GetCollectionPopupScrollbarLayout(popup);
            const int maximum = GetCollectionPopupMaxScrollOffset(
                *popupWidget, popup);
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewport.top, viewport.bottom,
                    visible + maximum, visible,
                    popupScrollbarDragStartOffset_, GetOpenCollectionPopupLayoutMetrics().scale);
            const int scroll = snowdesktop::widget_scroll_rules::
                ApplyScrollbarThumbDrag(
                    popupScrollbarDragStartOffset_,
                    upPoint.y - popupScrollbarDragStartY_, geometry);
            auto* search = GetCategorizedPopupView();
            if (!search || !search->SetFolderSearchScrollOffset(scroll)) popupScrollOffset_ = scroll;
        }
        popupScrollbarDragging_ = false;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        snowdesktop::private_pointer::ReleasePointerCapture();
        InvalidateCollectionPopupControls(true);
        return;
    }

    if (widgetScrollbarDragging_)
    {
        WidgetContainer* container = widgetScrollbarDragContainer_;
        if (container && container->GetWidgetData())
        {
            const int maximum = container->GetMaxScrollOffset();
            const int visible = container->GetVisibleContentHeight();
            const RECT viewport = container->GetScrollbarViewportRect();
            const auto geometry = snowdesktop::widget_scroll_rules::
                ResolveScrollbarAxisGeometry(
                    viewport.top, viewport.bottom,
                    visible + maximum, visible,
                    widgetScrollbarDragStartOffset_,
                    container->GetCellScale());
            const int next = snowdesktop::widget_scroll_rules::ApplyScrollbarThumbDrag(
                widgetScrollbarDragStartOffset_, upPoint.y - widgetScrollbarDragStartY_, geometry);
            auto* search = dynamic_cast<ScrollingItemWidget*>(container);
            if (!search || !search->SetFolderSearchScrollOffset(next)) container->GetWidgetData()->scrollOffset = next;
            if (auto* group = dynamic_cast<FileGroup*>(container))
                group->InvalidateHostedView();
            else
                container->InvalidateSlots();
        }
        widgetScrollbarDragging_ = false;
        widgetScrollbarDragContainer_ = nullptr;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        if (container) InvalidateWidgetControls(*container);
        snowdesktop::private_pointer::ReleasePointerCapture();
        RequestLayoutSave();
        return;
    }

    if (detailColumnResizeActive_)
    {
        const size_t widgetIndex = mouseDownWidgetIndex_;
        const bool popupResize = detailColumnResizePopup_;
        const bool dockFolderPopupResize =
            popupResize && dockFolderPopupOpen_;
        detailColumnResizeActive_ = false;
        detailColumnResizePopup_ = false;
        detailColumnResizeColumn_ =
            snowdesktop::list_detail_rules::Column::None;
        detailColumnResizeHeaderLeft_ = 0;
        detailColumnResizeHeaderWidth_ = 1;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        snowdesktop::private_pointer::ReleasePointerCapture();
        if (dockFolderPopupResize && folderPreview_)
            CommitFolderPreviewDisplaySettings();
        else if (dockFolderPopupResize)
        {
            snowdesktop::InteractiveLayoutSave save(this);
            CommitDockFolderPopupStateToSource();
        }
        else
            RequestLayoutSave();
        if (popupResize)
        {
            if (DesktopWidget* popupWidget = GetOpenPopupWidget())
            {
                const RECT popup =
                    GetCollectionPopupRect(*popupWidget);
                (void)PresentDesktopForegroundComposition(
                    popup);
            }
        }
        else if (widgetIndex < widgets_.size())
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
        const std::string surface =
            luaWidgetPanelRequest_.surface;
        const auto panelGeneration = floatingPopupMouseHookGeneration_;
        const RECT previousDesktopPanel = IsLuaPanelHostedByFloatingWindow()
            ? RECT{} : GetLuaWidgetPanelRect();
        const auto panelStillInteractive = [&] {
            return panelGeneration == floatingPopupMouseHookGeneration_ &&
                luaWidgetPanelRequest_.widgetId == panelWidgetId &&
                luaWidgetPanelAnimation_.IsInteractive();
        };
        const bool pointerCaptured = widgetEngine_ &&
            widgetEngine_->HasInteractionPointerCapture(
                panelWidgetId, surface);
        const bool hostInputHandled =
            widgetEngine_ &&
            widgetEngine_->HandleHostInputPointerUp(
                panelWidgetId, localX, localY, surface);
        const bool dispatchMouseUp = widgetEngine_ && !hostInputHandled &&
            (PtInRect(&content, upPoint) || pointerCaptured);
        if (dispatchMouseUp)
        {
            const char* upEvent = surface == "dialog"
                ? "onDialogMouseUp"
                : (surface == "popover"
                    ? "onPopoverMouseUp" : "onPanelMouseUp");
            const char* clickEvent = surface == "dialog"
                ? "onDialogClick"
                : (surface == "popover"
                    ? "onPopoverClick" : "onPanelClick");
            widgetEngine_->InvokeMouseEvent(
                panelWidgetId,
                upEvent,
                localX, localY, 1, 0);
            if (panelStillInteractive() && PtInRect(&content, upPoint))
            {
                widgetEngine_->InvokeMouseEvent(
                    panelWidgetId,
                    clickEvent,
                    localX, localY, 1, 0);
            }
        }
        luaWidgetPanelMouseDown_ = false;
        mouseDown_ = false;
        ReleaseLuaWidgetPanelCaptureIfOwned();
        UpdateHostInputImePosition();
        // A panel callback can change the model its desktop widget draws, even
        // when the callback closes the panel or replaces it with another one.
        if ((hostInputHandled || dispatchMouseUp) && widgetEngine_ &&
            FindWidgetIndexById(panelWidgetId) < widgets_.size())
            widgetEngine_->RuntimeInvalidateHost(panelWidgetId, std::nullopt, "desktop");
        // Closing or replacing a panel publishes its old footprint and its successor.
        if (!panelStillInteractive()) return;
        if (IsLuaPanelHostedByFloatingWindow())
        {
            // Its WM_LBUTTONUP dispatcher presents after this handler returns.
            if (!handlingFloatingPopupInput_)
                InvalidateFloatingPopupWindow();
        }
        else
        {
            const RECT currentPanel = GetLuaWidgetPanelRect();
            const auto invalidatePanel = [this](RECT panel) {
                if (IsRectEmpty(&panel)) return;
                InflateRect(&panel, 3, 3);
                InvalidateRect(hwnd_, &panel, FALSE);
            };
            if (!EqualRect(&previousDesktopPanel, &currentPanel))
                invalidatePanel(previousDesktopPanel);
            invalidatePanel(currentPanel);
        }
        return;
    }

    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        luaWidgetPanelRequest_.modal)
        return;

    for (auto& container : containers_)
    {
        auto* searchable =
            dynamic_cast<ScrollingItemWidget*>(container.get());
        if (!searchable ||
            !searchable->IsSearchPointerSelecting())
            continue;
        searchable->FinishSearchPointerSelection(upPoint);
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ =
            static_cast<size_t>(-1);
        InvalidateWidgetControls(*searchable);
        snowdesktop::private_pointer::ReleasePointerCapture();
        UpdateHostInputImePosition();
        return;
    }

    // Widget buttons act only after the initiating click has been
    // released. Opening the menu from WM_LBUTTONDOWN lets that same click's
    // button-up event immediately dismiss the newly-created menu.
    if (pendingWidgetButtonAction_ != WidgetHit::None)
    {
        const WidgetHit pendingAction = pendingWidgetButtonAction_;
        const bool closedPopupByPress = pendingWidgetButtonClosedPopup_;
        const size_t widgetIndex = mouseDownWidgetIndex_;
        WidgetContainer* buttonOwner = nullptr;
        if (widgetIndex < widgets_.size())
        {
            for (auto& container : containers_)
            {
                auto* candidate = dynamic_cast<WidgetContainer*>(container.get());
                if (candidate &&
                    candidate->GetWidgetData() == &widgets_[widgetIndex])
                {
                    buttonOwner = candidate;
                    break;
                }
            }
        }
        const bool invoke = buttonOwner &&
            (pendingAction != WidgetHit::CollectionOpenBtn ||
                (widgetAction_ == WidgetAction::None && !dragSession_.IsActive())) &&
            buttonOwner->HitTestWidget(upPoint) == pendingAction;
        const RECT widgetBounds = widgetIndex < widgets_.size()
            ? widgets_[widgetIndex].bounds : RECT{};

        pendingWidgetButtonAction_ = WidgetHit::None;
        pendingWidgetButtonClosedPopup_ = false;
        if (pendingAction == WidgetHit::CollectionOpenBtn)
            widgetAction_ = WidgetAction::None;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        snowdesktop::private_pointer::ReleasePointerCapture();
        if (!IsRectEmpty(&widgetBounds))
            InvalidateRect(hwnd_, &widgetBounds, FALSE);

        if (!invoke)
            return;
        if (pendingAction == WidgetHit::CollectionOpenBtn)
        {
            OpenCollectionPopupAt(widgetIndex, upPoint, L"", closedPopupByPress);
            return;
        }
        if (pendingAction == WidgetHit::SearchMoreBtn)
        {
            if (auto* search = dynamic_cast<ScrollingItemWidget*>(buttonOwner)) search->ExpandFolderSearchAtPoint(upPoint);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (pendingAction == WidgetHit::CollapseToggleBtn)
        {
            ToggleWidgetCollapsed(widgetIndex);
            return;
        }
        if (pendingAction == WidgetHit::GuideDetailsBtn)
        {
            if (auto* guide = dynamic_cast<GuideWidget*>(buttonOwner))
                guide->ToggleDetails();
            InvalidateRect(hwnd_, &widgetBounds, FALSE);
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
        const WidgetAction completedWidgetAction = widgetAction_;
        if (completedWidgetAction == WidgetAction::Move)
        {
            // Sample both geometry and modifiers at release, including a final
            // key transition with no intervening mouse-move message.
            OnMouseMoveAt(wp, upPoint);
            HideDragHintWindow();
            DWORD menuKeyState = MK_RBUTTON;
            const bool commitMove = !rightDrop || ChooseRightDragDropAction(upPoint,
                menuKeyState, DROPEFFECT_MOVE | DROPEFFECT_COPY, false);
            const auto menuPairAction = widgetPairAction_;
            if (commitMove && mouseDownWidgetIndex_ < widgets_.size() && luaStackMemberDrag_.Armed())
            {
                widgetAction_ = WidgetAction::None;
                bool committed = false;
                if (!IsPointOccludedByOpenPopup(upPoint) && !IsExternalDropWindowAt(upPoint))
                {
                    if (widgetPairTargetIndex_ < widgets_.size())
                        committed = CommitLuaWidgetStack(mouseDownWidgetIndex_, widgetPairTargetIndex_,
                            &luaStackMemberDrag_);
                    else committed = CommitLuaStackMemberDrag(widgetPreviewCell_);
                }
                if (!committed)
                    MessageBeep(MB_ICONWARNING);
            }
            else if (commitMove && mouseDownWidgetIndex_ < widgets_.size())
            {
                if (rightDrop)
                {
                    wp = menuKeyState;
                    // Menu messages can rebuild containers. Re-resolve Dock/group
                    // bindings after the source ID has been validated and rebound.
                    widgetAction_ = WidgetAction::Move;
                    OnMouseMoveAt(wp, upPoint);
                }
                const size_t pairTarget = HitTestWidgetPairTarget(upPoint, mouseDownWidgetIndex_);
                const auto pairAction = rightDrop ? menuPairAction : pairTarget < widgets_.size()
                    ? snowdesktop::widget_pair_drop::ResolveAction(
                        snowdesktop::widget_pair_drop::GetOptions(
                            widgets_[mouseDownWidgetIndex_].type, widgets_[pairTarget].type),
                        (wp & MK_CONTROL) != 0, (wp & MK_SHIFT) != 0,
                        (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0)
                    : snowdesktop::widget_pair_drop::Action::None;
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
                // Preserve the drag-time Dock hit geometry through the final
                // release test, then end the gesture before any placement path
                // rebuilds the persistent DockHost from normal interaction bounds.
                widgetAction_ = WidgetAction::None;
                if (pairAction != snowdesktop::widget_pair_drop::Action::None)
                {
                    CommitWidgetPairDrop(mouseDownWidgetIndex_, pairTarget, pairAction);
                }
                else if (canGroup)
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
                else if (IsPointOccludedByOpenPopup(upPoint))
                    MessageBeep(MB_ICONWARNING);
                else if (canDock)
                {
                    Widget dockSource(&widgets_[mouseDownWidgetIndex_], this);
                    int mods = rightDrop ? static_cast<int>(wp & MK_CONTROL) :
                        (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0;
                    CommitDockDrop({ &dockSource }, nullptr, dock,
                        widgetDockInsertIndex_, mods);
                    RequestLayoutSave();
                    LayoutItems();
                }
                else
                    PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
            }
        }
        else
        {
            // The native move entry ignores stale samples after physical
            // release. Resize, like Move above, must resolve its final geometry
            // from this real button-up rather than the last preview sample.
            if (completedWidgetAction == WidgetAction::Resize)
            {
                OnMouseMoveAt(wp, upPoint);
                HideDragHintWindow();
            }
            widgetAction_ = WidgetAction::None;
            if (completedWidgetAction == WidgetAction::Resize)
                PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        }
        // PendingMove/PendingResize: just cancel without displacement
        luaStackMemberDrag_.Reset();
        widgetAction_ = WidgetAction::None;
        rightButtonItemDrag_ = false;
        snowdesktop::private_pointer::Cursor(LoadCursorW(nullptr, IDC_ARROW));
        UpdateWidgetHandleCursor(upPoint);
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);
        widgetPairTargetIndex_ = static_cast<size_t>(-1);
        widgetPairAction_ = snowdesktop::widget_pair_drop::Action::None;
        KillTimer(hwnd_, kNativeDragHoverRecoveryTimerId);
        InvalidateDragStaticScene();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        snowdesktop::private_pointer::ReleasePointerCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.IsActive())
    {
        if (HandleDockClickRelease(upPoint))
            return;
        const bool previewMarquee = folderPreview_ && marqueeActive_ && marqueeDockFolderPopup_;
        if (previewMarquee) UpdateMarqueeSelection(upPoint);
        const bool otherSelectionDamage = marqueeActive_ || dockPressedContainer_ ||
            pendingCtrlToggleDesktopIndex_ < items_.size() || pendingCtrlToggleWidgetItem_;
        ClearDockPressedState();
        RECT luaDamage{};
        if (mouseDownWidgetIndex_ < widgets_.size() &&
            IsLuaDesktopSurface(widgets_[mouseDownWidgetIndex_]) &&
            widgetEngine_)
        {
            auto* luaContent = ResolveLuaWidgetContent(&widgets_[mouseDownWidgetIndex_]);
            if (!luaContent) return;
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            const bool loaded = widgetEngine_->EnsureWidgetLoaded(
                luaContent->id,
                luaContent->packageId);
            const bool contentClicked =
                HitTestStandaloneWidget(mouseDownWidgetIndex_,
                    upPoint) == WidgetHit::Content;
            const auto hostState = widgetEngine_->GetWidgetHostState(
                luaContent->id,
                luaContent->packageId);
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
                OpenMissingWidgetWorkshopPage(hwnd_, *luaContent);
            }
            else if (!loaded && hostActionClicked &&
                hostAction == snowdesktop::widget_runtime::
                    WidgetHostAction::RequestPermission)
            {
                POINT screenPoint = upPoint;
                ClientToScreen(hwnd_, &screenPoint);
                BeginLuaWidgetConsent(screenPoint,
                    luaContent->packageId,
                    luaContent->id);
            }
            else if (!loaded && hostActionClicked &&
                hostAction == snowdesktop::widget_runtime::
                    WidgetHostAction::Reload)
            {
                (void)widgetEngine_->RetryWidget(
                    luaContent->id,
                    luaContent->packageId);
            }
            else if (loaded)
            {
                luaDamage = frame;
                const bool pointerCaptured =
                    widgetEngine_->HasInteractionPointerCapture(
                        luaContent->id);
                const bool hostInputHandled =
                    widgetEngine_->HandleHostInputPointerUp(
                        luaContent->id,
                        upPoint.x - frame.left,
                        upPoint.y - frame.top);
                if (hostInputHandled)
                    UpdateHostInputImePosition();
                if (!hostInputHandled &&
                    (contentClicked || pointerCaptured))
                {
                    widgetEngine_->InvokeMouseEvent(
                        luaContent->id,
                        "onMouseUp", upPoint.x - frame.left,
                        upPoint.y - frame.top, 1, 0);
                    if (contentClicked)
                    {
                        widgetEngine_->InvokeClick(
                            luaContent->id,
                            upPoint.x - frame.left,
                            upPoint.y - frame.top);
                    }
                }
            }
        }
        if (outsidePanelGeneration && outsidePanelGeneration == floatingPopupMouseHookGeneration_ &&
            !luaWidgetPanelRequest_.widgetId.empty())
        {
            CloseLuaWidgetPanel(luaWidgetPanelRequest_.widgetId, "outside");
        }
        if (pendingCtrlToggleDesktopIndex_ < items_.size())
            items_[pendingCtrlToggleDesktopIndex_].selected = false;
        pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
        if (pendingCtrlToggleWidgetItem_)
        {
            pendingCtrlToggleWidgetItem_->SetSelected(!pendingCtrlToggleWidgetItem_->IsSelected());
            pendingCtrlToggleWidgetItem_ = nullptr;
        }
        CompleteRenameClick(wp, upPoint);
        mouseDown_ = false;
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        marqueeDockFolderPopup_ = false;
        dockFolderPopupMarqueeInitialSelection_.clear();
        RefreshPageNavHotEdgeHoverAt(upPoint);
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        // Popup wrappers still exist here; normal capture release must not
        // cancel the rename delay through pointer-abort cleanup.
        ++expectedCaptureReleaseDepth_;
        snowdesktop::private_pointer::ReleasePointerCapture();
        --expectedCaptureReleaseDepth_;
        if (previewMarquee && folderPreview_ && folderPreview_->inlineBounds)
            (void)QueueDesktopWidgetComposition(folderPreview_->hostWidgetId);
        else if (previewMarquee && ShouldShowFloatingPopupWindow())
            InvalidateFloatingPopupWindow(true);
        else if (otherSelectionDamage || IsRectEmpty(&luaDamage))
            InvalidateRect(hwnd_, nullptr, FALSE);
        else
        {
            InflateRect(&luaDamage, 4, 4);
            InvalidateRect(hwnd_, &luaDamage, FALSE);
        }
        return;
    }

    if (dockRunningDrag_ && dragSession_.IsActive())
    {
        CommitDockRunningDrag(upPoint, static_cast<int>(wp & (MK_CONTROL | MK_SHIFT)));
        goto cleanup;
    }

    if (rightDrop)
    {
        ResolveCurrentDragTargetAt(upPoint);
        DWORD releaseKeyState = MK_RBUTTON;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) releaseKeyState |= MK_CONTROL;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) releaseKeyState |= MK_ALT;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) releaseKeyState |= MK_SHIFT;
        if (!ChooseRightDragDropAction(upPoint, releaseKeyState,
                DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, false))
            goto cleanup;
        wp = releaseKeyState;
        dropPreviewMods = static_cast<int>(releaseKeyState & (MK_CONTROL | MK_ALT | MK_SHIFT));
    }

    if (IsExternalDropWindowAt(upPoint))
    {
        dragSession_.UpdatePoint(upPoint);
        dragSession_.UpdateTarget(
            nullptr, nullptr, HitRegion::None);
        goto cleanup;
    }

    if (HitTestLuaFileDropTarget(upPoint) < widgets_.size())
    {
        (void)DeliverLuaFileDrop(upPoint, dragSession_.SourceList().FilePaths());
        goto cleanup;
    }

    if ((!rightDrop || widgetPairAction_ != snowdesktop::widget_pair_drop::Action::None) &&
        TryCommitWidgetPairDragDrop(upPoint, rightDrop ? dropPreviewMods :
            static_cast<int>(wp & (MK_CONTROL | MK_SHIFT)) |
            ((snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) ? MK_ALT : 0)))
        goto cleanup;

    if (!rightDrop)
    {
        dropPreviewMods = 0;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) dropPreviewMods |= MK_CONTROL;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) dropPreviewMods |= MK_ALT;
        if (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) dropPreviewMods |= MK_SHIFT;
    }
    CommitInternalDragRelease(upPoint, dropPreviewMods | ((rightDrop && (wp & MK_RBUTTON)) ? MK_RBUTTON : MK_LBUTTON),
        DROPEFFECT_MOVE | DROPEFFECT_COPY | DROPEFFECT_LINK);

cleanup:
    if (dragSession_.HasContext()) EndDragSession();
    ClearPopupMouseDownItem();
    dockFolderPopupDragItems_.clear();
    dockFolderPopupMarqueeInitialSelection_.clear();
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    RefreshPageNavHotEdgeHoverAt(upPoint);
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ClearDockPressedState();
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeDockFolderPopup_ = false;
    snowdesktop::private_pointer::ReleasePointerCapture();
    if (generalSettings_.dockEnabled && !reloading_ &&
        dockRunningWindowsRefreshTick_ == 0)
        RefreshDockRunningWindows(false);
}
