#include "floating_dock_rules.h"
#include "theme/personalization_types.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace snowdesktop::floating_dock_rules
{
RECT ExplicitDismissBounds(RECT panel, RECT monitor, int size)
{
    if (IsRectEmpty(&panel) || IsRectEmpty(&monitor)) return {};
    size = std::max(1, std::min({size, static_cast<int>(monitor.right - monitor.left),
        static_cast<int>(monitor.bottom - monitor.top)}));
    // Attach the circular badge to the actual painted corner, never the
    // maximum magnification/input reserve or an independent screen anchor.
    const LONG left = std::clamp<LONG>(panel.right - size / 2, monitor.left, monitor.right - size);
    const LONG top = std::clamp<LONG>(panel.top - size / 2, monitor.top, monitor.bottom - size);
    return {left, top, left + size, top + size};
}

bool IsPointOnExplicitDismiss(RECT bounds, POINT point)
{
    if (IsRectEmpty(&bounds)) return false;
    const double radius = static_cast<double>(bounds.right - bounds.left) / 2.0;
    const double dx = point.x - (static_cast<double>(bounds.left) + bounds.right) / 2.0;
    const double dy = point.y - (static_cast<double>(bounds.top) + bounds.bottom) / 2.0;
    return dx * dx + dy * dy < radius * radius;
}

bool IsWindowAbove(HWND upper, HWND lower)
{
    if (!upper || !lower || upper == lower)
        return false;
    HWND current = GetWindow(upper, GW_HWNDNEXT);
    for (unsigned visited = 0; current && visited < 4096; ++visited)
    {
        if (current == lower)
            return true;
        current = GetWindow(current, GW_HWNDNEXT);
    }
    return false;
}

bool HasAnySummonTrigger(
    bool hotkeyEnabled, bool edgeSwipeEnabled)
{
    return hotkeyEnabled || edgeSwipeEnabled;
}

FloatingDockInputPolicy ResolveFloatingDockInputPolicy(
    bool dockEnabled, bool hotkeyEnabled, bool effectiveEdgeSwipeEnabled)
{
    // Menus, folder popups and window previews can promote an ordinary Dock
    // with both summon triggers disabled. Their outside-click dismissal uses
    // the same pointer timer and must outlive the optional summon triggers.
    return {
        !dockEnabled || !HasAnySummonTrigger(hotkeyEnabled, effectiveEdgeSwipeEnabled),
        dockEnabled && hotkeyEnabled,
        dockEnabled,
        dockEnabled && effectiveEdgeSwipeEnabled,
    };
}

bool IsDockEffectivelyPromoted(
    bool manuallyPromoted,
    bool passiveDragRevealed,
    bool /*summonOnlyEnabled*/)
{
    return manuallyPromoted || passiveDragRevealed;
}

bool ShouldShowPersistentDockHost(
    bool active,
    bool effectivelyPromoted,
    bool summonOnlyEnabled,
    bool customDesktopVisible,
    bool desktopIconsHidden,
    bool keepWhenDesktopHidden,
    bool desktopPassthroughActive)
{
    return active && !desktopPassthroughActive &&
        (effectivelyPromoted ||
            (!summonOnlyEnabled &&
                (!customDesktopVisible || !desktopIconsHidden ||
                    keepWhenDesktopHidden)));
}

bool ShouldShowMergedStatusBarDockHost(
    bool active, bool effectivelyPromoted, bool summonOnlyEnabled,
    bool customDesktopVisible, bool desktopIconsHidden,
    bool keepWhenDesktopHidden, bool desktopPassthroughActive,
    bool fullscreen, bool interactionHeld)
{
    const bool floating = effectivelyPromoted || interactionHeld;
    return ShouldShowPersistentDockHost(active, floating, summonOnlyEnabled,
        customDesktopVisible, desktopIconsHidden, keepWhenDesktopHidden,
        desktopPassthroughActive) && (!fullscreen || floating);
}

bool ShouldPassivelyRevealDockForDragAtEdge(
    bool pointerInEdgeProjection,
    bool internalDragActive,
    bool oleDragActive)
{
    return pointerInEdgeProjection &&
        (internalDragActive || oleDragActive);
}

bool IsPointInMergedDockInteraction(
    POINT point, const RECT& dockInteraction, const RECT& strip)
{
    // Keep the real magnification envelope and the strip as separate targets:
    // their bounding rectangle would also retain unrelated desktop corners.
    return PtInRect(&dockInteraction, point) || PtInRect(&strip, point);
}

bool IsMenuOwnedByDock(HWND menuRoot, HWND dockWindow)
{
    // Both modern menus and the native Shell tracker use the actual source
    // HWND as their native owner. A menu on another monitor grants no hold.
    return menuRoot && dockWindow && IsWindow(menuRoot) && IsWindow(dockWindow) &&
        GetWindow(menuRoot, GW_OWNER) == dockWindow;
}

PassiveDragRevealAction ResolvePassiveDragRevealUpdate(
    bool passiveRevealEnabled,
    bool manuallyPromoted,
    bool passiveDragRevealed,
    bool revealRequested,
    bool keepRevealed,
    bool leavePending,
    bool leaveDelayElapsed)
{
    if (!passiveRevealEnabled || manuallyPromoted)
        return leavePending
            ? PassiveDragRevealAction::CancelLeave
            : PassiveDragRevealAction::None;
    if (!passiveDragRevealed)
        return revealRequested
            ? PassiveDragRevealAction::Reveal
            : PassiveDragRevealAction::None;
    if (keepRevealed)
        return leavePending
            ? PassiveDragRevealAction::CancelLeave
            : PassiveDragRevealAction::None;
    if (!leavePending)
        return PassiveDragRevealAction::BeginLeave;
    return leaveDelayElapsed
        ? PassiveDragRevealAction::Hide
        : PassiveDragRevealAction::None;
}

bool HasPassiveDragLeaveDelayElapsed(
    ULONGLONG leaveStartTick,
    ULONGLONG currentTick,
    ULONGLONG delay)
{
    return leaveStartTick != 0 &&
        currentTick >= leaveStartTick &&
        currentTick - leaveStartTick >= delay;
}

bool ShouldSummonForDockSurface(
    bool sourceBelongsToDock,
    bool floatingDockVisible)
{
    return sourceBelongsToDock && !floatingDockVisible;
}

bool ShouldStartSystemShowDesktopLayerGuard(
    bool protectedSurfaceActive,
    bool shellDesktopForeground,
    ULONGLONG lastSystemMinimizeStartTick,
    ULONGLONG currentTick,
    ULONGLONG evidenceWindowMs)
{
    return protectedSurfaceActive &&
        shellDesktopForeground &&
        lastSystemMinimizeStartTick != 0 &&
        currentTick >= lastSystemMinimizeStartTick &&
        currentTick - lastSystemMinimizeStartTick <=
            evidenceWindowMs;
}

SystemShowDesktopLayerGuardAction
ResolveSystemShowDesktopLayerGuardAction(
    bool layerGuardActive,
    bool protectedSurfaceActive,
    SystemShowDesktopForeground foreground,
    ULONGLONG lastSystemMinimizeStartTick,
    ULONGLONG currentTick,
    ULONGLONG lastProtectedForegroundTick)
{
    if (layerGuardActive)
    {
        // MINIMIZEEND means a window is about to restore, not that its
        // minimize animation finished. Actual foreground ownership ends the
        // guard even when no previously minimized window has been restored.
        if (!protectedSurfaceActive ||
            foreground == SystemShowDesktopForeground::Application)
            return SystemShowDesktopLayerGuardAction::Stop;
        if (foreground == SystemShowDesktopForeground::Unavailable &&
            (lastProtectedForegroundTick == 0 ||
                currentTick < lastProtectedForegroundTick ||
                currentTick - lastProtectedForegroundTick >
                    kSystemShowDesktopForegroundGraceMs))
            return SystemShowDesktopLayerGuardAction::Stop;
        return SystemShowDesktopLayerGuardAction::None;
    }
    return ShouldStartSystemShowDesktopLayerGuard(
        protectedSurfaceActive,
        foreground == SystemShowDesktopForeground::ShellDesktop,
        lastSystemMinimizeStartTick,
        currentTick)
        ? SystemShowDesktopLayerGuardAction::Start
        : SystemShowDesktopLayerGuardAction::None;
}

bool ShouldDispatchDockContextMenu(
    bool persistentDockHostActive,
    bool pressBeganOnMatchingPersistentDockHost)
{
    return !persistentDockHostActive ||
        pressBeganOnMatchingPersistentDockHost;
}

bool ShouldUseFloatingDockLogicalForeground(
    bool keyboardSessionActive,
    bool actualWindowOwnedByCurrentProcess,
    bool shellFileOperationInFlight,
    bool actualWindowIsTaskWindow)
{
    const bool shellTransientWindow =
        shellFileOperationInFlight &&
        !actualWindowIsTaskWindow;
    return keyboardSessionActive &&
        (actualWindowOwnedByCurrentProcess ||
            shellTransientWindow ||
            !actualWindowIsTaskWindow);
}

bool ShouldRefocusFloatingDockKeyboardSession(
    bool floatingDockVisible,
    bool keyboardSessionActive,
    int shellFileOperationInFlight,
    int shellPopupMenuLayerDepth)
{
    return floatingDockVisible && keyboardSessionActive &&
        shellFileOperationInFlight == 0 &&
        shellPopupMenuLayerDepth == 0;
}

bool ShouldFloatingDockBeTopmost(
    bool floatingDockVisible,
    int shellPopupMenuLayerDepth)
{
    return floatingDockVisible && shellPopupMenuLayerDepth == 0;
}

bool ShouldChangeFloatingDockTopmost(
    bool currentlyTopmost,
    bool shouldBeTopmost)
{
    return currentlyTopmost != shouldBeTopmost;
}

int ScaleEdgeSwipeDip(int value, UINT dpi)
{
    return std::max(1, MulDiv(
        value, static_cast<int>(std::max<UINT>(96, dpi)), 96));
}

bool IsPointOnDockScreenEdge(
    POINT point, const RECT& monitorRect,
    DockPosition position, int edgeBand)
{
    if (IsRectEmpty(&monitorRect) ||
        point.x < monitorRect.left ||
        point.x >= monitorRect.right ||
        point.y < monitorRect.top ||
        point.y >= monitorRect.bottom)
        return false;

    edgeBand = std::max(0, edgeBand);
    switch (position)
    {
    case DockPosition::Top:
        return point.y - monitorRect.top <= edgeBand;
    case DockPosition::Left:
        return point.x - monitorRect.left <= edgeBand;
    case DockPosition::Right:
        return monitorRect.right - 1 - point.x <= edgeBand;
    case DockPosition::Bottom:
    default:
        return monitorRect.bottom - 1 - point.y <= edgeBand;
    }
}

bool IsPointInDockEdgeProjection(
    POINT point,
    const RECT& monitorRect,
    const RECT& dockScreenRect,
    DockPosition position,
    int edgeBand)
{
    if (IsRectEmpty(&dockScreenRect) ||
        !IsPointOnDockScreenEdge(
            point, monitorRect, position, edgeBand))
    {
        return false;
    }

    if (position == DockPosition::Top ||
        position == DockPosition::Bottom)
    {
        const LONG projectionLeft = std::max(
            monitorRect.left, dockScreenRect.left);
        const LONG projectionRight = std::min(
            monitorRect.right, dockScreenRect.right);
        return projectionLeft < projectionRight &&
            point.x >= projectionLeft &&
            point.x < projectionRight;
    }

    const LONG projectionTop = std::max(
        monitorRect.top, dockScreenRect.top);
    const LONG projectionBottom = std::min(
        monitorRect.bottom, dockScreenRect.bottom);
    return projectionTop < projectionBottom &&
        point.y >= projectionTop &&
        point.y < projectionBottom;
}

bool IsPointInDockEdgeCorridor(
    POINT point,
    const RECT& monitorRect,
    const RECT& dockScreenRect,
    DockPosition position)
{
    if (IsRectEmpty(&monitorRect) ||
        IsRectEmpty(&dockScreenRect) ||
        point.x < monitorRect.left ||
        point.x >= monitorRect.right ||
        point.y < monitorRect.top ||
        point.y >= monitorRect.bottom)
    {
        return false;
    }

    if (position == DockPosition::Top ||
        position == DockPosition::Bottom)
    {
        const LONG projectionLeft = std::max(
            monitorRect.left, dockScreenRect.left);
        const LONG projectionRight = std::min(
            monitorRect.right, dockScreenRect.right);
        if (projectionLeft >= projectionRight ||
            point.x < projectionLeft ||
            point.x >= projectionRight)
        {
            return false;
        }
        if (position == DockPosition::Top)
        {
            const LONG innerEdge = std::clamp<LONG>(
                dockScreenRect.bottom,
                monitorRect.top, monitorRect.bottom);
            return point.y < innerEdge;
        }
        const LONG innerEdge = std::clamp<LONG>(
            dockScreenRect.top,
            monitorRect.top, monitorRect.bottom);
        return point.y >= innerEdge;
    }

    const LONG projectionTop = std::max(
        monitorRect.top, dockScreenRect.top);
    const LONG projectionBottom = std::min(
        monitorRect.bottom, dockScreenRect.bottom);
    if (projectionTop >= projectionBottom ||
        point.y < projectionTop ||
        point.y >= projectionBottom)
    {
        return false;
    }
    if (position == DockPosition::Left)
    {
        const LONG innerEdge = std::clamp<LONG>(
            dockScreenRect.right,
            monitorRect.left, monitorRect.right);
        return point.x < innerEdge;
    }
    const LONG innerEdge = std::clamp<LONG>(
        dockScreenRect.left,
        monitorRect.left, monitorRect.right);
    return point.x >= innerEdge;
}

bool HasNewPointerButtonPress(
    UINT buttonsDown, UINT previousButtonsDown,
    UINT pressedSinceLastSample, bool forwardedState)
{
    // A forwarded snapshot can arrive before its classified event. It only
    // describes held buttons; dispatch owns the press and its minimize intent.
    return pressedSinceLastSample != 0 ||
        (!forwardedState && (buttonsDown & ~previousButtonsDown) != 0);
}

bool ShouldDismissForForegroundActivation(
    bool visible, bool foregroundChanged, bool externalForeground,
    bool windowTransitionActive, bool recentSystemMinimize) noexcept
{
    return visible && foregroundChanged && externalForeground &&
        !windowTransitionActive && !recentSystemMinimize;
}

bool HasPointerButtonActivity(
    UINT buttonsDown, UINT previousButtonsDown,
    UINT pressedSinceLastSample,
    bool observedPointerButtonActivity)
{
    return observedPointerButtonActivity ||
        buttonsDown != 0 ||
        previousButtonsDown != 0 ||
        pressedSinceLastSample != 0;
}

bool IsGuiMenuModeActive(DWORD guiThreadFlags)
{
    return (guiThreadFlags &
        (GUI_INMENUMODE |
         GUI_SYSTEMMENUMODE |
         GUI_POPUPMENUMODE)) != 0;
}

RECT UnionNonEmptyRects(const RECT& first, const RECT& second)
{
    if (IsRectEmpty(&first))
        return second;
    if (IsRectEmpty(&second))
        return first;
    RECT result{};
    UnionRect(&result, &first, &second);
    return result;
}

RECT ExpandHostForTitleLayer(
    RECT dockRect, DockPosition position, int titleWidth, int titleHeight)
{
    // Reserve the DPI-scaled title size with its gap and vertical offset. Keep this
    // allocation stable while the pointer moves; only the exact title
    // chip is added to the HWND region, so the transparent reserve never
    // receives input.
    const int titleWidthAxisPadding = (titleWidth + 8) / 2;
    const int titleHeightAxisPadding = (titleHeight + 16) / 2;
    const int titleWidthAndGap = titleWidth + 12;
    const int titleHeightAndGap = titleHeight + 16;
    switch (position)
    {
    case DockPosition::Top:
        dockRect.left -= titleWidthAxisPadding;
        dockRect.right += titleWidthAxisPadding;
        dockRect.bottom += titleHeightAndGap;
        break;
    case DockPosition::Left:
        dockRect.top -= titleHeightAxisPadding;
        dockRect.bottom += titleHeightAxisPadding;
        dockRect.right += titleWidthAndGap;
        break;
    case DockPosition::Right:
        dockRect.top -= titleHeightAxisPadding;
        dockRect.bottom += titleHeightAxisPadding;
        dockRect.left -= titleWidthAndGap;
        break;
    case DockPosition::Bottom:
    default:
        dockRect.left -= titleWidthAxisPadding;
        dockRect.right += titleWidthAxisPadding;
        dockRect.top -= titleHeightAndGap;
        break;
    }
    return dockRect;
}

int ResolveBorderOverdraw(float borderWidth) noexcept
{
    const float width = std::clamp(borderWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    return std::max(1, static_cast<int>(std::ceil(
        (width + 1.35f) * 0.5f)));
}

RECT ExpandForBorderOverdraw(
    RECT visualRect,
    float borderWidth)
{
    // The independent edge highlight includes an antialiased outer pass wider
    // than its logical core. Floating layers reserve pixels dynamically so
    // the configured maximum width is not clipped by the host region.
    const int borderOverdraw = ResolveBorderOverdraw(borderWidth);
    if (!IsRectEmpty(&visualRect))
        InflateRect(
            &visualRect,
            borderOverdraw,
            borderOverdraw);
    return visualRect;
}

bool ShouldRenderDesktopDock(
    bool floatingDockOwnsVisual,
    bool selectedForFloatingHost)
{
    return !floatingDockOwnsVisual ||
        !selectedForFloatingHost;
}

bool ShouldRenderFloatingDockFrame(
    bool dockHostActive)
{
    return dockHostActive;
}

bool ShouldInvalidateDesktopHover(
    bool handlingFloatingDockInput)
{
    return !handlingFloatingDockInput;
}

bool NeedsImmediatePointerPresent(
    bool itemDragFeedbackChanged,
    bool widgetPreviewActive,
    bool marqueeActive)
{
    return itemDragFeedbackChanged ||
        widgetPreviewActive ||
        marqueeActive;
}

bool ShouldPresentPointerFrame(
    ULONGLONG now,
    ULONGLONG lastPresent,
    bool forceImmediate)
{
    return forceImmediate ||
        lastPresent == 0 ||
        now < lastPresent ||
        now - lastPresent >= kPointerFrameIntervalMs;
}

UINT RemainingPointerFrameDelay(
    ULONGLONG now,
    ULONGLONG lastPresent)
{
    if (lastPresent == 0 || now < lastPresent)
        return 0;
    const ULONGLONG elapsed = now - lastPresent;
    if (elapsed >= kPointerFrameIntervalMs)
        return 0;
    return static_cast<UINT>(
        kPointerFrameIntervalMs - elapsed);
}

bool ShouldPresentDockPointerFrame(
    ULONGLONG now, ULONGLONG lastPresent,
    bool immediateInteraction, bool hoverOwnerChanged)
{
    // Crossing icons inside one Dock is continuous passive motion, so it
    // shares the 8 ms budget. Enter/leave and drag feedback remain immediate.
    return ShouldPresentPointerFrame(
        now, lastPresent, immediateInteraction || hoverOwnerChanged);
}

bool ShouldPresentPointerHost(
    bool presentAll, const void* hostOwner,
    const void* previousHoverOwner, const void* hoverOwner)
{
    return presentAll || (hostOwner &&
        (hostOwner == previousHoverOwner || hostOwner == hoverOwner));
}

bool ShouldCloseCollectionPopup(
    std::size_t openWidgetIndex,
    std::size_t clickedWidgetIndex,
    bool sameDockHost)
{
    return openWidgetIndex !=
            static_cast<std::size_t>(-1) &&
        openWidgetIndex == clickedWidgetIndex &&
        sameDockHost;
}

bool ShouldCloseCollectionPopupOnPointerDown(
    std::size_t openWidgetIndex,
    std::size_t pressedDockWidgetIndex,
    bool pointInsidePopup,
    bool sameDockHost)
{
    if (openWidgetIndex ==
            static_cast<std::size_t>(-1) ||
        pointInsidePopup)
        return false;
    // The collection button that owns the popup is its toggle control. Keep
    // the popup state intact until button release so one click cannot become
    // close-on-down followed by open-on-up.
    return !ShouldCloseCollectionPopup(
        openWidgetIndex,
        pressedDockWidgetIndex,
        sameDockHost);
}

POINT WindowPointToDesktopPoint(
    POINT windowPoint, const RECT& desktopSourceRect)
{
    return POINT{
        windowPoint.x + desktopSourceRect.left,
        windowPoint.y + desktopSourceRect.top
    };
}

RECT DesktopRectToWindowRect(
    RECT desktopRect, const RECT& desktopSourceRect)
{
    OffsetRect(
        &desktopRect,
        -desktopSourceRect.left,
        -desktopSourceRect.top);
    return desktopRect;
}

HRGN CreateHostWindowRegion(
    const RECT& dockRect, const RECT& animationVisualRect, const RECT& popupRect,
    const RECT& tooltipRect, const RECT& sourceRect,
    int radius, float dockBorderWidth)
{
    const RECT dockVisualRect = UnionNonEmptyRects(dockRect, animationVisualRect);
    const int borderOverdraw =
        snowdesktop::floating_dock_rules::ResolveBorderOverdraw(
            dockBorderWidth);
    const RECT dockRegionRect =
        snowdesktop::floating_dock_rules::
            ExpandForBorderOverdraw(
                dockVisualRect, dockBorderWidth);
    const RECT dockLocal =
        snowdesktop::floating_dock_rules::
            DesktopRectToWindowRect(
                dockRegionRect,
                sourceRect);
    HRGN windowRegion = CreateRoundRectRgn(
        dockLocal.left, dockLocal.top,
        dockLocal.right + 1, dockLocal.bottom + 1,
        (radius + borderOverdraw) * 2,
        (radius + borderOverdraw) * 2);
    auto appendRegion = [&](
        const RECT& desktopRect,
        int cornerRadius) {
        if (!windowRegion ||
            IsRectEmpty(&desktopRect))
            return;
        const RECT overdrawRect =
            snowdesktop::
                floating_dock_rules::
                    ExpandForBorderOverdraw(
                        desktopRect);
        const RECT local =
            snowdesktop::floating_dock_rules::
                DesktopRectToWindowRect(
                    overdrawRect,
                    sourceRect);
        HRGN addedRegion = CreateRoundRectRgn(
            local.left, local.top,
            local.right + 1,
            local.bottom + 1,
            (cornerRadius +
                borderOverdraw) * 2,
            (cornerRadius +
                borderOverdraw) * 2);
        if (addedRegion)
        {
            CombineRgn(windowRegion, windowRegion,
                addedRegion, RGN_OR);
            DeleteObject(addedRegion);
        }
    };
    appendRegion(
        popupRect, radius);
    appendRegion(
        tooltipRect, 7);
    return windowRegion;
}

RECT DockAssociatedPopupInteractionRect(
    bool anchoredToDock,
    const RECT& hostedPopupRect)
{
    return anchoredToDock
        ? hostedPopupRect
        : RECT{};
}

bool ShouldDismissForPointerDown(
    bool dragging,
    bool contextMenuActive,
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& previewRect,
    const RECT& siblingInteractionRect)
{
    if (dragging || contextMenuActive)
        return false;
    // While a menu owns the mouse loop, its item presses are outside the Dock
    // rects but must still reach the menu command instead of tearing the host
    // down. The menu itself handles outside-click dismissal.
    // Dock-owned auxiliary surfaces receive their own clicks without tearing
    // down the floating host. Other top-level surfaces decide independently.
    if (PtInRect(&dockRect, desktopPoint) ||
        (!IsRectEmpty(&previewRect) &&
            PtInRect(&previewRect, desktopPoint)) ||
        (!IsRectEmpty(&siblingInteractionRect) &&
            PtInRect(&siblingInteractionRect, desktopPoint)))
        return false;
    return IsRectEmpty(&popupRect) ||
        !PtInRect(&popupRect, desktopPoint);
}

bool IsPointInVisibleLayer(
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& tooltipRect)
{
    return PtInRect(
               &dockRect,
               desktopPoint) != FALSE ||
        (!IsRectEmpty(&popupRect) &&
            PtInRect(
                &popupRect,
                desktopPoint) != FALSE) ||
        (!IsRectEmpty(&tooltipRect) &&
            PtInRect(
                &tooltipRect,
                desktopPoint) != FALSE);
}

bool IsVisualOnlyPoint(
    POINT desktopPoint, const RECT& dockRect, const RECT& popupRect)
{
    if (PtInRect(&dockRect, desktopPoint))
        return false;
    return IsRectEmpty(&popupRect) ||
        !PtInRect(&popupRect, desktopPoint);
}

void WindowActionPromotion::Begin(bool alreadyPromoted, bool retain) noexcept
{
    if (retain) temporary_ = false;
    else if (!alreadyPromoted) temporary_ = true;
}

void WindowActionPromotion::Retain() noexcept
{ temporary_ = false; }

bool WindowActionPromotion::Release(bool retain) noexcept
{
    const bool dismiss = temporary_ && !retain;
    temporary_ = false;
    return dismiss;
}

bool ExternalPointerDrag::Update(POINT point, bool buttonHeld, bool externalSource,
    bool canceledOrMovingWindow, int thresholdX, int thresholdY)
{
    if (!buttonHeld) { Reset(); return false; }
    if (!held_)
    {
        held_ = true;
        origin_ = point;
        eligible_ = externalSource;
    }
    if (canceledOrMovingWindow) eligible_ = active_ = false;
    if (eligible_ && (std::abs(point.x - origin_.x) >= std::max(1, thresholdX) ||
        std::abs(point.y - origin_.y) >= std::max(1, thresholdY))) active_ = true;
    return active_;
}

bool ExternalPointerDrag::Active() const
{ return active_; }

void ExternalPointerDrag::Reset()
{ held_ = eligible_ = active_ = false; origin_ = {}; }

HWND WindowActionOutsidePress::Source() const noexcept
{ return source_; }

void WindowActionOutsidePress::Begin(HWND source) noexcept
{
    source_ = source;
    released_ = false;
}

void WindowActionOutsidePress::Cancel(HWND source) noexcept
{
    if (!source || source == source_) { source_ = nullptr; released_ = false; }
}

bool WindowActionOutsidePress::TakeReady(bool held, ULONGLONG now) noexcept
{
    if (!source_) return false;
    if (held) { released_ = false; return false; }
    if (!released_) { released_ = true; releasedAt_ = now; return false; }
    if (now - releasedAt_ < SettleMs) return false;
    Cancel();
    return true;
}

bool EdgeSwipeDetector::Update(
    POINT point, const RECT& monitorRect,
    DockPosition position, DWORD tick,
    int edgeBand, int requiredTravel,
    DWORD maximumDurationMs,
    bool hover,
    DWORD hoverDelayMs)
{
    if (!IsPointOnDockScreenEdge(
            point, monitorRect, position, edgeBand))
    {
        Reset();
        return false;
    }
    if (awaitingEdgeLeave_)
        return false;

    const bool contextChanged =
        !tracking_ ||
        position != position_ || hover != hover_ ||
        !EqualRect(&monitorRect_, &monitorRect);
    const DWORD elapsed = tick - startTick_;
    if (contextChanged ||
        (!hover && tracking_ && elapsed > maximumDurationMs))
    {
        tracking_ = true;
        monitorRect_ = monitorRect;
        position_ = position;
        hover_ = hover;
        startPoint_ = point;
        startTick_ = tick;
        return false;
    }

    requiredTravel = std::max(1, requiredTravel);
    const LONG alongEdge =
        position == DockPosition::Top ||
            position == DockPosition::Bottom
        ? point.x - startPoint_.x
        : point.y - startPoint_.y;
    if (hover ? elapsed < hoverDelayMs : std::abs(alongEdge) < requiredTravel)
        return false;

    tracking_ = false;
    awaitingEdgeLeave_ = true;
    return true;
}

void EdgeSwipeDetector::Reset()
{
    tracking_ = false;
    awaitingEdgeLeave_ = false;
    monitorRect_ = {};
    startPoint_ = {};
    startTick_ = 0;
}

void EdgeSwipeDetector::SuppressUntilEdgeLeave()
{
    tracking_ = false;
    awaitingEdgeLeave_ = true;
    monitorRect_ = {};
    startPoint_ = {};
    startTick_ = 0;
}

bool EdgeSwipeDetector::IsTracking() const
{ return tracking_; }

bool EdgeSwipeDetector::IsAwaitingEdgeLeave() const
{
    return awaitingEdgeLeave_;
}
}
