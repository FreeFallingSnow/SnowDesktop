#include "app/app.h"
#include "dock_platform_helpers.h"
#include "platform/foreground_fullscreen.h"

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

// Floating-Dock visibility, pointer mapping and interaction state.

void DesktopApp::ShowFloatingDock(
    HMONITOR preferredMonitor, DockRevealSource source)
{
    if (desktopPassthroughActive_)
        return;
    WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
        source == DockRevealSource::Hotkey ? L"Floating Dock shortcut received" :
        source == DockRevealSource::Gesture ? L"Floating Dock edge gesture received" :
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
    const HWND revealForeground = GetForegroundWindow();
    const auto privilegeGap = snowdesktop::dock_input_privilege::
        ForegroundHasHigherInputPrivilege(revealForeground);
    const bool foregroundFullscreen = snowdesktop::fullscreen::ObserveForeground().has_value();
    const bool requestKeyboardSession = snowdesktop::dock_fullscreen::StartsKeyboardSession(
        source, privilegeGap.value_or(false), foregroundFullscreen);
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
    // Each monitor owns its persistent visual independently. Summoning this
    // Host promotes only its content/backdrop pair and leaves other promoted
    // monitors untouched.
    floatingDockHost_->promoted = true;
    const bool needsDismissFallback = !privilegeGap.has_value() || privilegeGap.value_or(false) || foregroundFullscreen;
    floatingDockHost_->explicitDismissAvailable = needsDismissFallback && !requestKeyboardSession;
    floatingDockHost_->windowActionPromotion.Retain();
    floatingDockHost_->mergedCloseAfterInteraction = false;
    floatingDockHost_->passivelyRevealed = false;
    floatingDockHost_->passiveRevealTick = 0;
    floatingDockHost_->passiveLeaveStartTick = 0;
    RefreshFloatingDockVisibilityState();
    if (statusBar_) statusBar_->RefreshDockState(floatingDockHost_->monitor);
    // Rebuild shared chrome and hit regions before the first revealed frame.
    UpdateFloatingDockWindowBounds(*floatingDockHost_, false, true);
    floatingDockLastPointerPresentTick_ = 0;
    bool revealFramePrepared = false;
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
        // A summon-only Host is fully hidden between sessions. Prepare and
        // commit its current Dock/backdrop frame before showing the pair so
        // DWM cannot expose the surface retained from the previous session.
        revealFramePrepared =
            RenderFloatingDockCompositionFrame(*floatingDockHost_);
        if (revealFramePrepared)
            revealFramePrepared =
                FlushPendingCompositionCommit();
        if (!revealFramePrepared)
        {
            // Do not trade availability for a stale or blank first frame.
            // Keep the pair hidden, restore the idle state and leave an
            // immediate paint queued so the next summon can reuse a fresh
            // composition surface after recovery.
            floatingDockHost_->promoted = false;
            RefreshFloatingDockVisibilityState();
            if (statusBar_) statusBar_->RefreshDockState(floatingDockHost_->monitor);
            InvalidateFloatingDockWindow(
                *floatingDockHost_, true);
            if (previouslySelectedMonitor &&
                previouslySelectedMonitor != floatingDockMonitor_)
            {
                SyncPersistentDockHost(
                    previouslySelectedMonitor);
            }
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
                L"Floating Dock summon deferred: frame not ready");
            return;
        }
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
    if (hostWasVisible)
    {
        // A visible desktop-layer Host needs only the existing immediate
        // repaint after its Z-order promotion; no SHOW transaction occurs.
        InvalidateFloatingDockWindow(
            *floatingDockHost_, true);
    }
    // Reuse the proven shortcut input proxy only for a real keyboard surface
    // or a higher-privilege foreground. Never deliberately exit fullscreen.
    bool keyboardSessionReady = false;
    if (requestKeyboardSession && GetForegroundWindow() == revealForeground &&
        !snowdesktop::fullscreen::ObserveForeground())
    {
        BeginFloatingDockKeyboardSession(false);
        DWORD process = 0;
        if (const HWND foreground = GetForegroundWindow())
            GetWindowThreadProcessId(foreground, &process);
        keyboardSessionReady = process == GetCurrentProcessId();
        if (!keyboardSessionReady)
            EndFloatingDockKeyboardSession(FloatingDockCloseFocusPolicy::PreserveCurrent);
    }
    const bool explicitDismiss = needsDismissFallback && !keyboardSessionReady;
    if (floatingDockHost_->explicitDismissAvailable != explicitDismiss)
    {
        floatingDockHost_->explicitDismissAvailable = explicitDismiss;
        UpdateFloatingDockWindowBounds(*floatingDockHost_, true, true);
    }
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
        floatingDockPointerButtonsDown_ |=
            ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1u : 0u) |
            ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 2u : 0u) |
            ((GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 4u : 0u);
        if (!IsWindowVisible(ownedHost->hwnd))
        {
            const bool alreadyPromoted = ownedHost->promoted;
            ShowFloatingDock(monitor);
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
        return host.hwnd;
    }
    return nullptr;
}

void DesktopApp::ReleaseTemporaryDockActionPromotion()
{
    for (const auto& ownedHost : persistentDockHosts_)
    {
        if (ownedHost && ownedHost->windowActionPromotion.Release(dockSettings_.summonOnAppMinimize))
            CloseFloatingDock(*ownedHost, FloatingDockCloseFocusPolicy::PreserveCurrent);
    }
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
    if (!IsPersistentDockHostPromoted(host))
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
