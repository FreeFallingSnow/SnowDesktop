#include "app.h"

// Floating-Dock visibility, pointer mapping and interaction state.

void DesktopApp::ShowFloatingDock(
    HMONITOR preferredMonitor)
{
    WriteDiagnosticLogEntry(
        preferredMonitor
            ? L"Floating Dock associated surface reveal received"
            : L"Floating Dock shortcut received");
    if (!generalSettings_.dockEnabled)
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock shortcut ignored: feature disabled");
        return;
    }

    HideDockWindowPreview();
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
    if (!SyncPersistentDockHost(targetMonitor))
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock persistent host unavailable");
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const bool hostWasVisible =
        IsWindowVisible(floatingDockHost_->hwnd) != FALSE;
    // Each monitor owns its persistent visual independently. Summoning this
    // Host promotes only its content/backdrop pair and leaves other promoted
    // monitors untouched.
    floatingDockHost_->promoted = true;
    floatingDockHost_->passivelyRevealed = false;
    floatingDockHost_->passiveRevealTick = 0;
    floatingDockHost_->passiveLeaveStartTick = 0;
    RefreshFloatingDockVisibilityState();
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
            InvalidateFloatingDockWindow(
                *floatingDockHost_, true);
            if (previouslySelectedMonitor &&
                previouslySelectedMonitor != floatingDockMonitor_)
            {
                SyncPersistentDockHost(
                    previouslySelectedMonitor);
            }
            WriteDiagnosticLogEntry(
                L"Floating Dock summon deferred: frame not ready");
            return;
        }
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
    BeginFloatingDockKeyboardSession();
}

bool DesktopApp::
EnsureFloatingDockVisibleForAssociatedSurface(
    POINT anchorScreen)
{
    const HMONITOR monitor = MonitorFromPoint(
        anchorScreen, MONITOR_DEFAULTTONEAREST);
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

void DesktopApp::CloseFloatingDock(
    FloatingDockCloseFocusPolicy focusPolicy)
{
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
    host.passivelyRevealed = false;
    host.passiveRevealTick = 0;
    host.passiveLeaveStartTick = 0;
    RefreshFloatingDockVisibilityState();
    UpdatePersistentDockHostVisibility(host);
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
            host->passivelyRevealed = false;
            host->passiveRevealTick = 0;
            host->passiveLeaveStartTick = 0;
        }
    }
    RefreshFloatingDockVisibilityState();
    for (const auto& host : persistentDockHosts_)
    {
        if (!host)
            continue;
        UpdatePersistentDockHostVisibility(*host);
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
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const HMONITOR monitor = MonitorFromPoint(
        cursorScreen, MONITOR_DEFAULTTONEAREST);
    if (!SyncPersistentDockHost(monitor) ||
        !floatingDockHost_)
        return;
    if (IsPersistentDockHostPromoted(*floatingDockHost_))
        CloseFloatingDock(*floatingDockHost_);
    else
        ShowFloatingDock(monitor);
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
        if (immediate)
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
    PersistentDockHost& host)
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
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr))
        return hr;
    hr = host.dcompVisual->SetContent(
        surface.Get());
    if (FAILED(hr))
        return hr;
    CommitCompositionAnimationFrame();
    if (!FlushPendingCompositionCommit())
        return E_FAIL;
    host.dcompSurface = surface;
    host.compWidth = width;
    host.compHeight = height;
    return S_OK;
}

void DesktopApp::
RecoverFloatingDockCompositionFailure(
    PersistentDockHost& host,
    const wchar_t* stage, HRESULT hr)
{
    wchar_t message[192]{};
    wsprintfW(message,
        L"FloatingDock %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr));
    WriteDiagnosticLogEntry(message);
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
