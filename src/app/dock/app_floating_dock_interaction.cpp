#include "app/app.h"
#include "platform/window_support/window_support_runtime.h"
#include "dock_platform_helpers.h"
#include "dock_input_diagnostics.h"
#include "platform/foreground_fullscreen.h"
#include "app/popup/popup_window_pair_z_order.h"

namespace
{
struct ImmediateDockPaintDeferral
{
    bool& deferred;
    bool previous;
    explicit ImmediateDockPaintDeferral(bool& value) : deferred(value), previous(value)
    { deferred = true; }
    ~ImmediateDockPaintDeferral() { deferred = previous; }
};
}

void DesktopApp::RefreshFloatingDockPromotionInput(PersistentDockHost& host, bool requestFocus,
    bool nativePointerInput)
{
    if (floatingDockPromotionInputUpdateActive_) return;
    LASTINPUTINFO nativeInput{sizeof(LASTINPUTINFO)};
    if (nativePointerInput && (!host.promoted || host.promotionInputPointerRetryUsed ||
        host.promotionInputCapabilityMissing != true || !GetLastInputInfo(&nativeInput) ||
        host.promotionInputRequestInputTime == nativeInput.dwTime)) return;
    const bool raised = host.promoted || host.passivelyRevealed || host.mergedInteractionHeld;
    const bool visible = raised && host.active && host.frameReady && host.hwnd &&
        IsWindowVisible(host.hwnd) &&
        snowdesktop::popup_window_pair_z_order::IsTopmost(host.hwnd);
    const bool newlyVisible = visible && !host.promotionInputVisible;
    host.promotionInputVisible = visible;
    if (!visible)
    {
        host.promotionInputBorrowedFocus = false;
        host.promotionInputFocusPendingUntil = 0;
        host.promotionInputRequestInputTime.reset();
        host.promotionInputPointerRetryUsed = false;
        host.promotionInputForeground = nullptr;
        host.promotionInputProcess = 0;
        host.promotionInputCapabilityMissing.reset();
        if (host.explicitDismissAvailable)
        {
            host.explicitDismissAvailable = false;
            host.explicitDismissRect = {};
            host.windowRegionPending = true;
            InvalidateFloatingDockWindow(host, true);
        }
        return;
    }
    if (!floatingDockPromotionInputToken_)
        floatingDockPromotionInputToken_ = uiAnimationScheduler_.ScheduleInterval(50,
            [this](snowdesktop::UiScheduleToken token) {
                if (token != floatingDockPromotionInputToken_) return;
                (void)ObserveDockPromotionActivation(GetForegroundWindow());
                RefreshFloatingDockPromotionInputs();
            });

    const HWND foreground = GetForegroundWindow();
    DWORD process = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &process);
    const bool fullscreen = snowdesktop::fullscreen::ObserveForeground().has_value();
    const bool blocked = fullscreen || host.passivelyRevealed || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || floatingDockExternalPointerDrag_.Active() ||
        HasActiveContextMenuSession() || shellPopupMenuLayerDepth_ > 0 || shellFileOperationInFlight_ > 0 ||
        (dockStartMenu_ && dockStartMenu_->Active()) ||
        (dockWindowTransition_ && dockWindowTransition_->IsActive());
    const bool retryDeferred = host.promotionInputBlocked && !blocked;
    const bool request = requestFocus || newlyVisible || retryDeferred;
    if (request) host.promotionInputPointerRetryUsed = false;
    const auto borrowedContext = std::find_if(persistentDockHosts_.begin(), persistentDockHosts_.end(),
        [](const auto& candidate) { return candidate && candidate->promotionInputBorrowedFocus; });
    const bool compensatingSession = borrowedContext != persistentDockHosts_.end();
    const bool observingBorrowedForeground = floatingDockKeyboardSessionActive_ &&
        process == GetCurrentProcessId() && compensatingSession;
    const HWND pointerContext = observingBorrowedForeground
        ? (*borrowedContext)->promotionInputForeground : foreground;
    const DWORD pointerContextProcess = observingBorrowedForeground
        ? (*borrowedContext)->promotionInputProcess : process;
    DWORD currentContextProcess = 0;
    const bool contextCurrent = !observingBorrowedForeground ||
        (pointerContext && IsWindow(pointerContext) && GetWindowThreadProcessId(pointerContext, &currentContextProcess) &&
            currentContextProcess == pointerContextProcess);
    // Borrowing our own input queue does not establish observation of the
    // outside target. Keep every promoted host bound to the original context.
    const bool pointerUnavailable = !contextCurrent || snowdesktop::window_support::WindowSupport().Resolve(
        snowdesktop::window_support::Capability::PointerObservation, pointerContext).NeedsFallback();
    host.promotionInputCapabilityMissing = pointerUnavailable;
    if (!pointerUnavailable && host.promotionInputBorrowedFocus)
    {
        host.promotionInputBorrowedFocus = false;
        host.promotionInputFocusPendingUntil = 0;
        if (foreground == floatingDockInputHwnd_)
            for (const auto& candidate : persistentDockHosts_)
                if (candidate && candidate->promoted)
                    candidate->desktopLayerForegroundWindow = pointerContext;
        EndFloatingDockKeyboardSession(FloatingDockCloseFocusPolicy::PreserveCurrent);
        return; // Hiding the proxy can rebuild hosts; reacquire on the next observation.
    }
    host.promotionInputForeground = pointerContext;
    host.promotionInputProcess = pointerContextProcess;
    host.promotionInputBlocked = blocked;
    const bool borrowForeground = (request || nativePointerInput) && host.promotionInputCapabilityMissing.value_or(false) &&
        !blocked && (!host.promotionInputFocusPendingUntil || nativePointerInput);

    floatingDockPromotionInputUpdateActive_ = true;
    struct InputUpdateScope { bool& active; ~InputUpdateScope() { active = false; } }
        updateScope{floatingDockPromotionInputUpdateActive_};
    const HWND hostWindow = host.hwnd;
    auto* visibleHost = FindPersistentDockHost(hostWindow);
    if (borrowForeground && visibleHost && visibleHost->active && visibleHost->frameReady &&
        IsWindowVisible(hostWindow) && snowdesktop::popup_window_pair_z_order::IsTopmost(hostWindow) &&
        GetForegroundWindow() == foreground)
    {
        // A registered hotkey supplies activation permission. A polled gesture
        // may not; allow one more request only when new native Dock input arrives.
        // Last-input time deduplicates that input, it never proves an outside click.
        if (nativePointerInput || GetLastInputInfo(&nativeInput))
            host.promotionInputRequestInputTime = nativeInput.dwTime;
        else
            host.promotionInputRequestInputTime.reset();
        if (nativePointerInput) host.promotionInputPointerRetryUsed = true;
        SelectPersistentDockHost(visibleHost);
        CancelDockWindowActivationObservation(foreground);
        const bool borrowSession = !floatingDockKeyboardSessionActive_ || compensatingSession;
        BeginFloatingDockKeyboardSession(false);
        if (auto* current = FindPersistentDockHost(hostWindow))
            current->promotionInputBorrowedFocus = floatingDockKeyboardSessionActive_ && borrowSession;
        const HWND observed = GetForegroundWindow();
        DWORD observedProcess = 0;
        if (observed) GetWindowThreadProcessId(observed, &observedProcess);
        if (auto* current = FindPersistentDockHost(hostWindow))
        {
            current->promotionInputForeground = pointerContext;
            current->promotionInputProcess = pointerContextProcess;
            current->promotionInputCapabilityMissing = !contextCurrent || snowdesktop::window_support::WindowSupport().Resolve(
                snowdesktop::window_support::Capability::PointerObservation, pointerContext).NeedsFallback();
            // Publish the owned foreground immediately; a quick return click
            // must not depend on a timer first sampling the proxy's activation.
            current->desktopLayerForegroundWindow = observed;
            // User32 can finish cross-input-queue activation after the API
            // returns. Keep the proxy alive while observing that result;
            // hiding it immediately can cancel an otherwise accepted request.
            current->promotionInputFocusPendingUntil =
                observedProcess != GetCurrentProcessId() && floatingDockKeyboardSessionActive_
                ? GetTickCount64() + 150 : 0;
        }
    }
    if (auto* current = FindPersistentDockHost(hostWindow))
    {
        const HWND actualForeground = GetForegroundWindow();
        DWORD actualProcess = 0;
        if (actualForeground) GetWindowThreadProcessId(actualForeground, &actualProcess);
        if (floatingDockKeyboardSessionActive_ && actualForeground == floatingDockInputHwnd_)
            floatingDockInputForegroundSeen_ = true;
        if (current->promotionInputFocusPendingUntil)
        {
            if (actualProcess == GetCurrentProcessId())
                current->promotionInputFocusPendingUntil = 0;
            else if (blocked || !floatingDockKeyboardSessionActive_ ||
                current->promotionInputCapabilityMissing == false ||
                GetTickCount64() >= current->promotionInputFocusPendingUntil)
            {
                current->promotionInputFocusPendingUntil = 0;
                EndFloatingDockKeyboardSession(FloatingDockCloseFocusPolicy::PreserveCurrent);
            }
        }
        const bool focusDismissAvailable = floatingDockKeyboardSessionActive_ &&
            floatingDockInputForegroundSeen_ && actualProcess == GetCurrentProcessId();
        const bool keepFallback = !focusDismissAvailable && !current->promotionInputFocusPendingUntil &&
            (!current->promotionInputCapabilityMissing.has_value() ||
                current->promotionInputCapabilityMissing.value_or(false));
        if (current->explicitDismissAvailable != keepFallback)
        {
            current->explicitDismissAvailable = keepFallback;
            UpdateFloatingDockWindowBounds(*current, true, true);
        }
    }
}

void DesktopApp::RefreshFloatingDockPromotionInputs(bool requestFocus)
{
    std::vector<HWND> windows;
    for (const auto& host : persistentDockHosts_)
        if (host && host->hwnd) windows.push_back(host->hwnd);
    for (const HWND window : windows)
        if (auto* host = FindPersistentDockHost(window))
            RefreshFloatingDockPromotionInput(*host, requestFocus);
    const bool anyVisible = std::any_of(persistentDockHosts_.begin(), persistentDockHosts_.end(),
        [](const auto& host) { return host && host->promotionInputVisible; });
    if (!anyVisible && floatingDockPromotionInputToken_)
    {
        uiAnimationScheduler_.Cancel(floatingDockPromotionInputToken_);
        floatingDockPromotionInputToken_ = 0;
    }
}

// Floating-Dock visibility, pointer mapping and interaction state.

void DesktopApp::ShowFloatingDock(
    HMONITOR preferredMonitor, DockRevealSource source)
{
    floatingDockOutsidePress_.Cancel();
    if (desktopPassthroughActive_)
        return;
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
        source == DockRevealSource::Hotkey ? L"Floating Dock shortcut received" :
        source == DockRevealSource::Gesture ? L"Floating Dock edge gesture received" :
        source == DockRevealSource::WindowAction ? L"Floating Dock window action reveal received" :
        L"Floating Dock associated surface reveal received");
    if (!generalSettings_.dockEnabled)
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
            L"Floating Dock shortcut ignored: feature disabled");
        return;
    }

    const HMONITOR previouslySelectedMonitor =
        floatingDockMonitor_;
    HMONITOR targetMonitor = preferredMonitor;
    if (!targetMonitor)
    {
        POINT cursorScreen{};
        GetCursorPos(&cursorScreen);
        targetMonitor = MonitorFromPoint(
            cursorScreen, MONITOR_DEFAULTTONEAREST);
    }
    if (FullscreenBlocksDockReveal(source, targetMonitor))
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, L"Floating Dock reveal suppressed: foreground fullscreen");
        return;
    }
    // Capture fullscreen before host rebuilds, SHOW/TOPMOST transactions or
    // the keyboard proxy can change foreground ownership. The independently
    // revealed bar remains usable until its own outside dismissal.
    if (statusBar_) statusBar_->PrepareDockReveal(targetMonitor);
    HideDockWindowPreview();
    if (!SyncPersistentDockHost(targetMonitor))
    {
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
            L"Floating Dock persistent host unavailable", DiagnosticLogLevel::Warning);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    if (statusBar_ && floatingDockHost_->monitor != targetMonitor)
        statusBar_->PrepareDockReveal(floatingDockHost_->monitor);

    const bool hostWasVisible =
        IsWindowVisible(floatingDockHost_->hwnd) != FALSE;
    const bool hostWasPromoted = floatingDockHost_->promoted;
    // Each monitor owns its persistent visual independently. Summoning this
    // Host promotes only its content/backdrop pair and leaves other promoted
    // monitors untouched.
    bool revealFramePrepared = false;
    {
        // Layout, status-bar refresh and renderer callbacks can all revisit
        // visibility/layer policy. Keep the existing native band until the
        // complete current frame is committed, including a visible idle Dock.
        struct RevealPreparation
        {
            bool& pending;
            bool previous;
            explicit RevealPreparation(bool& value) : pending(value), previous(value)
            { pending = true; }
            ~RevealPreparation() { pending = previous; }
        } preparation(floatingDockHost_->revealPending);
        ImmediateDockPaintDeferral defer(floatingDockHost_->immediatePaintDeferred);
        floatingDockHost_->promoted = true;
        floatingDockHost_->promotionInputVisible = false;
        floatingDockHost_->windowActionPromotion.Retain();
        floatingDockHost_->mergedCloseAfterInteraction = false;
        floatingDockHost_->passivelyRevealed = false;
        floatingDockHost_->passiveRevealTick = 0;
        floatingDockHost_->passiveLeaveStartTick = 0;
        if (!hostWasVisible)
        {
            POINT cursorDesktop{};
            if (GetCursorPos(&cursorDesktop))
            {
                if (hwnd_ && IsWindow(hwnd_))
                    ScreenToClient(hwnd_, &cursorDesktop);
                else
                {
                    cursorDesktop.x -= virtualLeft_;
                    cursorDesktop.y -= virtualTop_;
                }
                lastMousePoint_ = cursorDesktop;
            }
        }
        RefreshFloatingDockVisibilityState();
        if (statusBar_) statusBar_->RefreshDockState(floatingDockHost_->monitor);
        // Geometry and pixels use the same pointer sample on hidden reveals.
        UpdateFloatingDockWindowBounds(*floatingDockHost_, false, true);
        floatingDockLastPointerPresentTick_ = 0;
        revealFramePrepared =
            RenderFloatingDockCompositionFrame(*floatingDockHost_) &&
            FlushPendingCompositionCommit();
    }
    if (!revealFramePrepared)
    {
        // Retain the previous idle/hidden presentation when preparation fails.
        // A recovery paint remains queued without exposing the old frame in a
        // newly promoted window.
        floatingDockHost_->promoted = hostWasPromoted;
        RefreshFloatingDockVisibilityState();
        if (statusBar_) statusBar_->RefreshDockState(floatingDockHost_->monitor);
        UpdatePersistentDockHostVisibility(*floatingDockHost_);
        InvalidateFloatingDockWindow(*floatingDockHost_, true);
        if (previouslySelectedMonitor &&
            previouslySelectedMonitor != floatingDockMonitor_)
        {
            SyncPersistentDockHost(previouslySelectedMonitor);
        }
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
            L"Floating Dock summon deferred: frame not ready");
        return;
    }
    // Rendering/host synchronization can pump work; recheck before promotion.
    if (FullscreenBlocksDockReveal(source, targetMonitor))
    {
        floatingDockHost_->promoted = false;
        RefreshFloatingDockVisibilityState();
        if (statusBar_) statusBar_->RefreshDockState(floatingDockHost_->monitor);
        UpdatePersistentDockHostVisibility(*floatingDockHost_);
        return;
    }
    UpdatePersistentDockHostVisibility(
        *floatingDockHost_);
    RefreshFloatingDockPromotionInput(*floatingDockHost_);
    // Actual keyboard editors retain their explicit input request. All other
    // promotion paths use the same established pointer capability above.
    if (source == DockRevealSource::AssociatedSurface &&
        !snowdesktop::fullscreen::ObserveForeground())
        BeginFloatingDockKeyboardSession(false);
}

bool DesktopApp::
EnsureFloatingDockVisibleForAssociatedSurface(
    POINT anchorScreen)
{
    if (desktopPassthroughActive_)
        return false;
    const HMONITOR monitor = MonitorFromPoint(
        anchorScreen, MONITOR_DEFAULTTONEAREST);
    if (FullscreenBlocksDockReveal(DockRevealSource::AssociatedSurface, monitor))
        return false;
    if (!SyncPersistentDockHost(monitor) ||
        !floatingDockHost_)
    {
        return false;
    }
    if (!snowdesktop::floating_dock_rules::
            ShouldSummonForDockSurface(
                true,
                IsPersistentDockHostEffectivelyFloating(
                    *floatingDockHost_)))
    {
        return true;
    }

    ShowFloatingDock(monitor);
    return floatingDockHost_ &&
        IsPersistentDockHostEffectivelyFloating(
            *floatingDockHost_);
}

// Search, restore and opted-in minimization retain promotion. Other minimize
// animations borrow it only until the presentation retires.
HWND DesktopApp::KeepDockTopmostForAction(const RECT& anchorScreen, bool retainPromotion)
{
    if (!generalSettings_.dockEnabled || desktopPassthroughActive_ ||
        IsRectEmpty(&anchorScreen)) return nullptr;
    if (!retainPromotion && (!dockWindowTransition_ ||
        !dockWindowTransition_->GetPresentationWindow())) return nullptr;
    const HMONITOR monitor = MonitorFromRect(&anchorScreen, MONITOR_DEFAULTTONEAREST);
    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (!ownedHost || !ownedHost->active || ownedHost->monitor != monitor ||
            !ownedHost->hwnd) continue;
        // Consume the native minimize-button press for hidden and visible
        // Hosts alike, before an outside-pointer timer can sample it.
        const auto buttons = snowdesktop::window_support::WindowSupport().PointerButtons();
        floatingDockPointerButtonsDown_ |= buttons ? *buttons : (
            ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) |
            ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2u : 0u) |
            ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4u : 0u));
        if (!IsWindowVisible(ownedHost->hwnd))
        {
            const bool alreadyPromoted = ownedHost->promoted;
            ShowFloatingDock(monitor, DockRevealSource::WindowAction);
            // Synchronization may rebuild the host collection. Reacquire the
            // actual revealed host before assigning animation ownership.
            for (const auto& revealed : persistentDockHosts_)
            {
                if (revealed && revealed->active && revealed->monitor == monitor && revealed->promoted &&
                    IsWindowVisible(revealed->hwnd))
                {
                    revealed->windowActionPromotion.Begin(alreadyPromoted, retainPromotion);
                    if (!retainPromotion && !dockWindowTransition_->GetPresentationWindow())
                    {
                        ReleaseTemporaryDockActionPromotion();
                        return nullptr;
                    }
                    return revealed->hwnd;
                }
            }
            return nullptr;
        }
        auto& host = *ownedHost;
        const bool alreadyPromoted = host.promoted;
        host.windowActionPromotion.Begin(host.promoted, retainPromotion);
        if (!host.promoted)
        {
            host.promoted = true;
            host.mergedCloseAfterInteraction = false;
            host.passivelyRevealed = false;
            host.passiveRevealTick = 0;
            host.passiveLeaveStartTick = 0;
            RefreshFloatingDockVisibilityState();
            UpdatePersistentDockHostVisibility(host);
        }
        RefreshFloatingDockPromotionInput(host, alreadyPromoted);
        return host.hwnd;
    }
    return nullptr;
}

void DesktopApp::ReleaseTemporaryDockActionPromotion()
{
    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (ownedHost && ownedHost->windowActionPromotion.Release(dockSettings_.summonOnAppMinimize))
        {
            snowdesktop::dock_input_diagnostics::RecordLayer(L"promotion-release", ownedHost->hwnd, ownedHost->promoted);
            CloseFloatingDock(*ownedHost, FloatingDockCloseFocusPolicy::PreserveCurrent);
        }
    }
    RefreshFloatingDockPromotionInputs(true);
}

void DesktopApp::CloseFloatingDock(
    FloatingDockCloseFocusPolicy focusPolicy)
{
    if (rightButtonDownDockHost_ == floatingDockHost_)
        rightButtonDownDockHost_ = nullptr;
    if (!floatingDockHost_ ||
        !IsPersistentDockHostPromoted(
            *floatingDockHost_))
    {
        std::function<void()> action =
            std::move(floatingDockPostCloseAction_);
        floatingDockPostCloseAction_ = {};
        if (action)
            action();
        return;
    }
    CloseFloatingDock(*floatingDockHost_, focusPolicy);
}

void DesktopApp::CloseFloatingDock(
    PersistentDockHost& host,
    FloatingDockCloseFocusPolicy focusPolicy)
{
    if (rightButtonDownDockHost_ == &host)
        rightButtonDownDockHost_ = nullptr;
    if (!host.active || (!host.promoted && !host.passivelyRevealed && !host.explicitDismissAvailable))
    {
        std::function<void()> action =
            std::move(floatingDockPostCloseAction_);
        floatingDockPostCloseAction_ = {};
        if (action)
            action();
        return;
    }

    const bool closingSelectedHost =
        floatingDockHost_ == &host;
    snowdesktop::dock_input_diagnostics::RecordLayer(L"close-host", host.hwnd, host.promoted);
    const bool endKeyboardSession =
        closingSelectedHost &&
        floatingDockKeyboardSessionActive_;
    if (closingSelectedHost)
    {
        floatingDockHoverTargetOwner_ = nullptr;
        floatingDockHoverTargetIndex_ = 0;
        floatingDockHoverTargetKind_ = 0;
        DismissDockWindowPreviewUntilLeave();
        floatingDockPointerPresentPending_ = false;
        floatingDockHoverHandoffPending_ = false;
        floatingDockHoverHandoffRect_ = {};
    }

    host.promoted = false;
    host.promotionInputFocusPendingUntil = 0;
    host.promotionInputRequestInputTime.reset();
    host.promotionInputPointerRetryUsed = false;
    host.promotionInputVisible = false;
    host.explicitDismissAvailable = false;
    host.explicitDismissRect = {};
    host.windowActionPromotion.Retain();
    host.passivelyRevealed = false;
    host.passiveRevealTick = 0;
    host.passiveLeaveStartTick = 0;
    host.windowRegionPending = true;
    RefreshFloatingDockVisibilityState();
    // Visibility can prepare an animated reveal/hide and request its own
    // immediate paint. Publish just once after the close state is complete,
    // still before restoring keyboard focus below.
    {
        ImmediateDockPaintDeferral defer(host.immediatePaintDeferred);
        if (statusBar_) statusBar_->RefreshDockState(host.monitor);
        UpdatePersistentDockHostVisibility(host);
    }
    InvalidateFloatingDockWindow(host, true);
    if (endKeyboardSession)
        EndFloatingDockKeyboardSession(focusPolicy);

    if (closingSelectedHost && floatingDockVisible_)
    {
        for (const auto& candidate : persistentDockHosts_)
        {
            if (candidate &&
                IsPersistentDockHostPromoted(*candidate))
            {
                SelectPersistentDockHost(candidate.get());
                break;
            }
        }
    }

    std::function<void()> action =
        std::move(floatingDockPostCloseAction_);
    floatingDockPostCloseAction_ = {};
    if (action)
        action();
}

void DesktopApp::CloseAllFloatingDocks(
    FloatingDockCloseFocusPolicy focusPolicy)
{
    floatingDockOutsidePress_.Cancel();
    rightButtonDownDockHost_ = nullptr;
    const bool endKeyboardSession =
        floatingDockKeyboardSessionActive_;
    floatingDockHoverTargetOwner_ = nullptr;
    floatingDockHoverTargetIndex_ = 0;
    floatingDockHoverTargetKind_ = 0;
    if (floatingDockVisible_)
        DismissDockWindowPreviewUntilLeave();
    floatingDockPointerPresentPending_ = false;
    floatingDockHoverHandoffPending_ = false;
    floatingDockHoverHandoffRect_ = {};

    for (const auto& host : persistentDockHosts_)
    {
        if (host)
        {
            host->promoted = false;
            host->promotionInputFocusPendingUntil = 0;
            host->promotionInputRequestInputTime.reset();
            host->promotionInputPointerRetryUsed = false;
            host->promotionInputVisible = false;
            host->explicitDismissAvailable = false;
            host->explicitDismissRect = {};
            host->windowActionPromotion.Retain();
            host->passivelyRevealed = false;
            host->passiveRevealTick = 0;
            host->passiveLeaveStartTick = 0;
            host->windowRegionPending = true;
        }
    }
    RefreshFloatingDockVisibilityState();
    for (const auto& host : persistentDockHosts_)
    {
        if (!host)
            continue;
        {
            ImmediateDockPaintDeferral defer(host->immediatePaintDeferred);
            if (statusBar_) statusBar_->RefreshDockState(host->monitor);
            UpdatePersistentDockHostVisibility(*host);
        }
        InvalidateFloatingDockWindow(*host, true);
    }
    if (endKeyboardSession)
        EndFloatingDockKeyboardSession(focusPolicy);

    std::function<void()> action =
        std::move(floatingDockPostCloseAction_);
    floatingDockPostCloseAction_ = {};
    if (action)
        action();
}

void DesktopApp::CloseFloatingDockThen(
    std::function<void()> action,
    FloatingDockCloseFocusPolicy focusPolicy)
{
    if (!IsSelectedPersistentDockHostPromoted())
    {
        if (action)
            action();
        return;
    }

    floatingDockPostCloseAction_ = std::move(action);
    CloseFloatingDock(focusPolicy);
}

void DesktopApp::CloseAllFloatingDocksThen(
    std::function<void()> action,
    FloatingDockCloseFocusPolicy focusPolicy)
{
    if (!floatingDockVisible_)
    {
        if (action)
            action();
        return;
    }

    floatingDockPostCloseAction_ = std::move(action);
    CloseAllFloatingDocks(focusPolicy);
}

void DesktopApp::ToggleFloatingDock()
{
    if (desktopPassthroughActive_)
        return;
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const HMONITOR monitor = MonitorFromPoint(
        cursorScreen, MONITOR_DEFAULTTONEAREST);
    if (FullscreenBlocksDockReveal(DockRevealSource::Hotkey, monitor))
    {
        CloseAllFloatingDocks(FloatingDockCloseFocusPolicy::PreserveCurrent);
        return;
    }
    if (!SyncPersistentDockHost(monitor) ||
        !floatingDockHost_)
        return;
    if (IsPersistentDockHostPromoted(*floatingDockHost_))
        CloseFloatingDock(*floatingDockHost_);
    else
        ShowFloatingDock(monitor, DockRevealSource::Hotkey);
}

void DesktopApp::InvalidateFloatingDockWindow(
    bool immediate)
{
    InvalidatePersistentDockHosts(immediate);
}

void DesktopApp::InvalidatePersistentDockHosts(
    bool immediate)
{
    for (const auto& host : persistentDockHosts_)
        if (host)
            InvalidateFloatingDockWindow(
                *host, immediate);
}

void DesktopApp::InvalidateFloatingDockWindow(
    PersistentDockHost& host,
    bool immediate)
{
    if (snowdesktop::floating_dock_rules::
            ShouldRenderFloatingDockFrame(
                host.active) &&
        host.hwnd && IsWindow(host.hwnd))
    {
        InvalidateRect(
            host.hwnd, nullptr, FALSE);
        // WM_MOUSEMOVE 和 OLE DragOver 会持续占满输入队列，只 InvalidateRect
        // 会让放大/插入预览等队列空闲才绘制，快速扫过时明显落后指针。
        // immediate 必须同步 UpdateWindow。历史回归：f29a882 曾改成
        // floatingDockPointerPresentPending_ + EnsureUiAnimationFrame()，
        // 导致浮动 Dock hover 和拖放反馈晚一帧；仅当合成绘制重入时才允许兜底。
        if (immediate && !host.immediatePaintDeferred)
        {
            if (!host.compositionPaintInProgress)
                UpdateWindow(host.hwnd);
            else
            {
                floatingDockPointerPresentPending_ = true;
                EnsureUiAnimationFrame();
            }
        }
    }
}

POINT DesktopApp::FloatingDockClientToDesktop(
    const PersistentDockHost& host,
    POINT point) const
{
    if (host.hwnd && IsWindow(host.hwnd) &&
        hwnd_ && IsWindow(hwnd_))
    {
        MapWindowPoints(
            host.hwnd, hwnd_,
            &point, 1);
        return point;
    }
    return snowdesktop::floating_dock_rules::
        WindowPointToDesktopPoint(
            point, host.sourceRect);
}

HRESULT DesktopApp::
CreateOrResizeFloatingDockCompositionSurface(
    PersistentDockHost& host,
    ComPtr<IDCompositionSurface>& frameSurface)
{
    if (!dcompDevice_ || !host.hwnd ||
        !IsWindow(host.hwnd))
        return E_UNEXPECTED;
    RECT client{};
    GetClientRect(host.hwnd, &client);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, client.right));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, client.bottom));

    if (!host.dcompTarget)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            host.hwnd, FALSE,
            &host.dcompTarget);
        if (FAILED(hr))
            return hr;
    }
    if (!host.dcompVisual)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &host.dcompVisual);
        if (FAILED(hr) || !host.dcompVisual)
            return FAILED(hr) ? hr : E_FAIL;
        hr = host.dcompTarget->SetRoot(
            host.dcompVisual.Get());
        if (FAILED(hr))
            return hr;
    }
    if (host.dcompSurface &&
        host.compWidth == width &&
        host.compHeight == height)
    {
        frameSurface = host.dcompSurface;
        return S_OK;
    }

    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &frameSurface);
    if (FAILED(hr) || !frameSurface)
        return FAILED(hr) ? hr : E_FAIL;
    // Keep a replacement detached until its first full frame has finished.
    // The renderer binds it after EndDraw so DirectComposition receives the
    // pixels and the visual-content switch in one final transaction.
    return S_OK;
}

void DesktopApp::
RecoverFloatingDockCompositionFailure(
    PersistentDockHost& host,
    const wchar_t* stage, HRESULT hr,
    bool preserveExistingFrame)
{
    if (RequestGraphicsDeviceRecovery(stage, hr))
        return;
    wchar_t message[224]{};
    wsprintfW(message,
        L"FloatingDock %s FAILED hr=0x%08X; %s composition frame",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr),
        preserveExistingFrame
            ? L"retaining previous"
            : L"resetting");
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message, DiagnosticLogLevel::Warning);
    if (!preserveExistingFrame)
        ResetFloatingDockCompositionResources(host);
    if (!host.compositionRenderRecoveryPending &&
        host.hwnd && IsWindow(host.hwnd))
    {
        host.compositionRenderRecoveryPending = true;
        InvalidateRect(
            host.hwnd, nullptr, FALSE);
    }
}

bool DesktopApp::IsAnyPersistentDockHostPainting() const
{
    for (const auto& host : persistentDockHosts_)
        if (host && host->compositionPaintInProgress)
            return true;
    return false;
}
