#pragma once

#include "dock_layout_settings.h"
#include "dock_settings_defaults.h"

#include <windows.h>

#include <cstddef>

namespace snowdesktop::floating_dock_rules
{

// A window animation borrows promotion only when the user has not already
// summoned this monitor's Dock. An explicit summon takes over that lifetime.
class WindowActionPromotion
{
public:
    void Begin(bool alreadyPromoted, bool retain) noexcept;
    void Retain() noexcept;
    bool Release(bool retain) noexcept;
private:
    bool temporary_ = false;
};

inline constexpr DWORD kWindowExStyle =
    WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP;

RECT ExplicitDismissBounds(RECT panel, RECT monitor, int size);

bool IsPointOnExplicitDismiss(RECT bounds, POINT point);

inline constexpr int kEdgeSwipeBandDip = 4;
inline constexpr int kEdgeSwipeTravelDip = 72;
inline constexpr DWORD kEdgeSwipeMaximumDurationMs = 480;
inline constexpr int kPassiveDragRevealEdgeBandDip = 6;
inline constexpr ULONGLONG kPassiveDragLeaveDelayMs = 360;
inline constexpr ULONGLONG kDesktopLayerRestoreRetryMs = 250;

bool IsWindowAbove(HWND upper, HWND lower);

template <typename IsApplicationWindow, typename IsDockWindow>
HWND LowestDesktopLayerApplication(
    HWND referenceWindow, IsApplicationWindow&& isApplicationWindow,
    IsDockWindow&& isDockWindow, bool* scanComplete = nullptr)
{
    if (scanComplete) *scanComplete = true;
    HWND current = GetWindow(referenceWindow, GW_HWNDLAST);
    for (unsigned visited = 0; current && visited < 4096; ++visited)
    {
        if (current != referenceWindow && !isDockWindow(current) &&
            (GetWindowLongPtrW(current, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
            isApplicationWindow(current))
        {
            return current;
        }
        current = GetWindow(current, GW_HWNDPREV);
    }
    if (scanComplete) *scanComplete = current == nullptr;
    return nullptr;
}

// A dismissed Dock must be below applications, not merely non-topmost.
// Explorer's immediate predecessor can be a Shell/WinUI tool window whose
// placement is not a usable application-layer anchor. Prefer the lowest real
// application; use the desktop predecessor only when no application is present.
template <typename IsApplicationWindow, typename IsDockWindow>
HWND DesktopLayerInsertAfter(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow)
{
    // GW_HWNDLAST on a topmost Dock searches the topmost group. Explorer's
    // normal top-level window is the reference for ordinary application order.
    const HWND referenceWindow = desktopWindow && IsWindow(desktopWindow)
        ? desktopWindow : contentWindow;
    if (HWND application = LowestDesktopLayerApplication(
            referenceWindow, isApplicationWindow, isDockWindow))
        return application;

    HWND anchor = desktopWindow && IsWindow(desktopWindow)
        ? GetWindow(desktopWindow, GW_HWNDPREV) : nullptr;
    for (unsigned visited = 0; anchor && isDockWindow(anchor) && visited < 4096; ++visited)
        anchor = GetWindow(anchor, GW_HWNDPREV);
    return anchor && IsWindow(anchor) && anchor != contentWindow &&
        !isDockWindow(anchor) &&
        (GetWindowLongPtrW(anchor, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0
        ? anchor : HWND_NOTOPMOST;
}

template <typename IsApplicationWindow, typename IsDockWindow>
bool IsDesktopLayerRestored(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow)
{
    if (!contentWindow || !IsWindow(contentWindow) ||
        (GetWindowLongPtrW(contentWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 ||
        !desktopWindow || !IsWindow(desktopWindow) ||
        !IsWindowAbove(contentWindow, desktopWindow))
        return false;
    bool scanComplete = false;
    const HWND application = LowestDesktopLayerApplication(
        contentWindow, isApplicationWindow, isDockWindow, &scanComplete);
    return scanComplete && (!application || IsWindowAbove(application, contentWindow));
}

template <typename IsApplicationWindow, typename IsDockWindow, typename PlacePair>
bool RestoreDesktopLayer(
    HWND contentWindow, HWND desktopWindow,
    IsApplicationWindow&& isApplicationWindow, IsDockWindow&& isDockWindow,
    PlacePair&& placePair, HWND foregroundFallback = nullptr)
{
    if (!contentWindow || !IsWindow(contentWindow))
        return false;
    desktopWindow = desktopWindow && IsWindow(desktopWindow)
        ? GetAncestor(desktopWindow, GA_ROOT) : nullptr;
    // A target can disappear/change bands while User32 positions the pair.
    // Resolve again instead of accepting a band-only fallback as completion.
    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        const HWND anchor = DesktopLayerInsertAfter(
            contentWindow, desktopWindow, isApplicationWindow, isDockWindow);
        if (placePair(anchor) && IsDesktopLayerRestored(
                contentWindow, desktopWindow, isApplicationWindow, isDockWindow))
            return true;
    }
    // A rejected/stale lowest anchor must not leave a dismissed Dock over the
    // application the user just activated. This is a partial placement only:
    // retain recovery until the complete desktop-order predicate also passes.
    if (foregroundFallback && IsWindow(foregroundFallback) &&
        foregroundFallback != contentWindow && !isDockWindow(foregroundFallback) &&
        (GetWindowLongPtrW(foregroundFallback, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
        isApplicationWindow(foregroundFallback) && placePair(foregroundFallback))
        return IsDesktopLayerRestored(
            contentWindow, desktopWindow, isApplicationWindow, isDockWindow);
    return false;
}

// 被动 Dock hover 的同步提交限频窗口。hover 必须跟手，但也不需要每个
// WM_MOUSEMOVE 都同步重绘整个浮动 Dock。
inline constexpr ULONGLONG kPointerFrameIntervalMs = 8;
// Explorer raises Progman when the Shell enters Show Desktop. Combine that
// foreground transition with recent system-minimize evidence so an ordinary
// desktop click cannot promote the Dock by itself.
inline constexpr ULONGLONG kSystemShowDesktopEvidenceWindowMs = 500;
// A temporarily unavailable foreground must not turn desktop protection into
// an unbounded TOPMOST lease. Confirmed desktop/surface samples renew this.
inline constexpr ULONGLONG kSystemShowDesktopForegroundGraceMs = 500;

bool HasAnySummonTrigger(
    bool hotkeyEnabled, bool edgeSwipeEnabled);

struct FloatingDockInputPolicy
{
    bool closeFloatingDocks;
    bool registerHotkey;
    bool monitorPointer;
    bool monitorEdgeSwipe;
};

FloatingDockInputPolicy ResolveFloatingDockInputPolicy(
    bool dockEnabled, bool hotkeyEnabled, bool effectiveEdgeSwipeEnabled);

bool IsDockEffectivelyPromoted(
    bool manuallyPromoted,
    bool passiveDragRevealed,
    bool /*summonOnlyEnabled*/);

// Before an external drag enters a SnowDesktop drop target there is no OLE
// payload/session here. Recognize only a held, moved pointer from another app;
// actual drop acceptance remains exclusively in the normal OLE target path.
class ExternalPointerDrag
{
public:
    bool Update(POINT point, bool buttonHeld, bool externalSource,
        bool canceledOrMovingWindow, int thresholdX, int thresholdY);
    bool Active() const;
    void Reset();
private:
    POINT origin_{};
    bool held_ = false, eligible_ = false, active_ = false;
};

bool ShouldShowPersistentDockHost(
    bool active,
    bool effectivelyPromoted,
    bool summonOnlyEnabled,
    bool customDesktopVisible,
    bool desktopIconsHidden,
    bool keepWhenDesktopHidden,
    bool desktopPassthroughActive = false);

// Merging adds only a monitor-owned interaction hold and fullscreen observation
// to the ordinary Dock policy. Native Explorer desktop remains a Dock surface;
// wallpaper passthrough and summon-only still hide the idle bar.
bool ShouldShowMergedStatusBarDockHost(
    bool active, bool effectivelyPromoted, bool summonOnlyEnabled,
    bool customDesktopVisible, bool desktopIconsHidden,
    bool keepWhenDesktopHidden, bool desktopPassthroughActive,
    bool fullscreen, bool interactionHeld);

bool ShouldPassivelyRevealDockForDragAtEdge(
    bool pointerInEdgeProjection,
    bool internalDragActive,
    bool oleDragActive);

bool IsPointInMergedDockInteraction(
    POINT point, const RECT& dockInteraction, const RECT& strip);

bool IsMenuOwnedByDock(HWND menuRoot, HWND dockWindow);

enum class PassiveDragRevealAction
{
    None,
    Reveal,
    BeginLeave,
    CancelLeave,
    Hide,
};

PassiveDragRevealAction ResolvePassiveDragRevealUpdate(
    bool passiveRevealEnabled,
    bool manuallyPromoted,
    bool passiveDragRevealed,
    bool revealRequested,
    bool keepRevealed,
    bool leavePending,
    bool leaveDelayElapsed);

bool HasPassiveDragLeaveDelayElapsed(
    ULONGLONG leaveStartTick,
    ULONGLONG currentTick,
    ULONGLONG delay = kPassiveDragLeaveDelayMs);

bool ShouldSummonForDockSurface(
    bool sourceBelongsToDock,
    bool floatingDockVisible);

bool ShouldStartSystemShowDesktopLayerGuard(
    bool persistentDockHostActive,
    bool shellDesktopForeground,
    ULONGLONG lastSystemMinimizeStartTick,
    ULONGLONG currentTick,
    ULONGLONG evidenceWindowMs =
        kSystemShowDesktopEvidenceWindowMs);

enum class SystemShowDesktopLayerGuardAction
{
    None,
    Start,
    Stop,
};

enum class SystemShowDesktopForeground
{
    ShellDesktop,
    DesktopSurface,
    Application,
    Unavailable,
};

SystemShowDesktopLayerGuardAction
ResolveSystemShowDesktopLayerGuardAction(
    bool layerGuardActive,
    bool persistentDockHostActive,
    SystemShowDesktopForeground foreground,
    ULONGLONG lastSystemMinimizeStartTick,
    ULONGLONG currentTick,
    ULONGLONG lastProtectedForegroundTick);

bool ShouldDispatchDockContextMenu(
    bool persistentDockHostActive,
    bool pressBeganOnMatchingPersistentDockHost);

bool ShouldUseFloatingDockLogicalForeground(
    bool keyboardSessionActive,
    bool actualWindowOwnedByCurrentProcess,
    bool shellFileOperationInFlight,
    bool actualWindowIsTaskWindow);

bool ShouldRefocusFloatingDockKeyboardSession(
    bool floatingDockVisible,
    bool keyboardSessionActive,
    int shellFileOperationInFlight,
    int shellPopupMenuLayerDepth);

bool ShouldFloatingDockBeTopmost(
    bool floatingDockVisible,
    int shellPopupMenuLayerDepth);

bool ShouldChangeFloatingDockTopmost(
    bool currentlyTopmost,
    bool shouldBeTopmost);

int ScaleEdgeSwipeDip(int value, UINT dpi);

bool IsPointOnDockScreenEdge(
    POINT point, const RECT& monitorRect,
    DockPosition position, int edgeBand);

bool IsPointInDockEdgeProjection(
    POINT point,
    const RECT& monitorRect,
    const RECT& dockScreenRect,
    DockPosition position,
    int edgeBand);

bool IsPointInDockEdgeCorridor(
    POINT point,
    const RECT& monitorRect,
    const RECT& dockScreenRect,
    DockPosition position);

bool HasNewPointerButtonPress(
    UINT buttonsDown, UINT previousButtonsDown,
    UINT pressedSinceLastSample, bool forwardedState = false);

// Custom caption buttons announce their action after button-up. Retain an
// outside press while it is held and briefly after release, so the actual
// native action can consume it before dismissal. Never infer an action from
// the target's elevation or the presence of a helper process.
class WindowActionOutsidePress
{
public:
    static constexpr ULONGLONG SettleMs = 50;
    HWND Source() const noexcept;
    void Begin(HWND source) noexcept;
    void Cancel(HWND source = nullptr) noexcept;
    bool TakeReady(bool held, ULONGLONG now) noexcept;
private:
    HWND source_ = nullptr;
    ULONGLONG releasedAt_ = 0;
    bool released_ = false;
};

bool ShouldDismissForForegroundActivation(
    bool visible, bool foregroundChanged, bool externalForeground,
    bool windowTransitionActive, bool recentSystemMinimize) noexcept;

bool HasPointerButtonActivity(
    UINT buttonsDown, UINT previousButtonsDown,
    UINT pressedSinceLastSample,
    bool observedPointerButtonActivity = false);

bool IsGuiMenuModeActive(DWORD guiThreadFlags);

/**
 * @brief Detects a quick pointer stroke that remains on the Dock-facing edge.
 *
 * The gesture is parallel to the edge: horizontal for top/bottom Docks and
 * vertical for left/right Docks. A completed gesture stays latched until the
 * pointer leaves the edge, preventing one long stroke from opening repeatedly.
 */
class EdgeSwipeDetector
{
public:
    bool Update(
        POINT point, const RECT& monitorRect,
        DockPosition position, DWORD tick,
        int edgeBand, int requiredTravel,
        DWORD maximumDurationMs =
            kEdgeSwipeMaximumDurationMs,
        bool hover = false,
        DWORD hoverDelayMs = dock_settings_rules::kDefaultEdgeHoverDelayMs);

    void Reset();

    void SuppressUntilEdgeLeave();

    bool IsTracking() const;
    bool IsAwaitingEdgeLeave() const;

private:
    bool tracking_ = false;
    bool hover_ = false;
    bool awaitingEdgeLeave_ = false;
    RECT monitorRect_{};
    POINT startPoint_{};
    DockPosition position_ = DockPosition::Bottom;
    DWORD startTick_ = 0;
};

RECT UnionNonEmptyRects(const RECT& first, const RECT& second);

RECT ExpandHostForTitleLayer(
    RECT dockRect, DockPosition position, int titleWidth = 300, int titleHeight = 36);

int ResolveBorderOverdraw(float borderWidth) noexcept;

RECT ExpandForBorderOverdraw(
    RECT visualRect,
    float borderWidth = 1.0f);

bool ShouldRenderDesktopDock(
    bool floatingDockOwnsVisual,
    bool selectedForFloatingHost);

/** @brief 常驻 DockHost 只要处于活动状态且未关闭事务挂起就可以继续绘制。 */
bool ShouldRenderFloatingDockFrame(
    bool dockHostActive);

/**
 * @brief 浮动层接收鼠标时，其 hover 帧只应提交到浮动窗口。
 *
 * 顶层 Dock 已替代对应的桌面 Dock；继续让每个浮动 WM_MOUSEMOVE 排队
 * 重绘桌面层会挤占 UI 线程，并让快速扫过时的放大效果落后于指针。
 */
bool ShouldInvalidateDesktopHover(
    bool handlingFloatingDockInput);

/**
 * @brief 需要在当前指针消息结束前同步提交的交互状态。
 */
bool NeedsImmediatePointerPresent(
    bool itemDragFeedbackChanged,
    bool widgetPreviewActive,
    bool marqueeActive);

/**
 * @brief 限制被动 Dock hover 的同步提交频率，同时保留拖动帧的即时反馈。
 *
 * 指针反馈必须同步提交，不能改成等待 UiAnimationScheduler 的下一帧。
 * f29a882 曾把所有 hover/拖拽帧改为 EnsureUiAnimationFrame()，导致快速
 * 扫过时 Dock 放大和拖拽虚影明显落后指针。
 */
bool ShouldPresentPointerFrame(
    ULONGLONG now,
    ULONGLONG lastPresent,
    bool forceImmediate);

UINT RemainingPointerFrameDelay(
    ULONGLONG now,
    ULONGLONG lastPresent);

bool ShouldPresentDockPointerFrame(
    ULONGLONG now, ULONGLONG lastPresent,
    bool immediateInteraction, bool hoverOwnerChanged);

bool ShouldPresentPointerHost(
    bool presentAll, const void* hostOwner,
    const void* previousHoverOwner, const void* hoverOwner);

bool ShouldCloseCollectionPopup(
    std::size_t openWidgetIndex,
    std::size_t clickedWidgetIndex,
    bool sameDockHost = true);

bool ShouldCloseCollectionPopupOnPointerDown(
    std::size_t openWidgetIndex,
    std::size_t pressedDockWidgetIndex,
    bool pointInsidePopup,
    bool sameDockHost = true);

POINT WindowPointToDesktopPoint(
    POINT windowPoint, const RECT& desktopSourceRect);

RECT DesktopRectToWindowRect(
    RECT desktopRect, const RECT& desktopSourceRect);

// Returns an owned region; SetWindowRgn takes ownership only on success.
// Use the animation envelope for visibility, independently of mouse targets.
HRGN CreateHostWindowRegion(
    const RECT& dockRect, const RECT& animationVisualRect, const RECT& popupRect,
    const RECT& tooltipRect, const RECT& sourceRect,
    int radius, float dockBorderWidth);

RECT DockAssociatedPopupInteractionRect(
    bool anchoredToDock,
    const RECT& hostedPopupRect);

bool ShouldDismissForPointerDown(
    bool dragging,
    bool contextMenuActive,
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& previewRect,
    const RECT& siblingInteractionRect = RECT{});

bool IsPointInVisibleLayer(
    POINT desktopPoint,
    const RECT& dockRect,
    const RECT& popupRect,
    const RECT& tooltipRect);

// Called for points inside the HWND's visual region, including overdraw.
bool IsVisualOnlyPoint(
    POINT desktopPoint, const RECT& dockRect, const RECT& popupRect);
} // namespace snowdesktop::floating_dock_rules
