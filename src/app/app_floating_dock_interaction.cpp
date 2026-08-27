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

    // The persistent Host already owns the only Dock visual. Summoning only
    // promotes the content HWND and its paired backdrop to the floating band.
    floatingDockVisible_ = true;
    floatingDockLastPointerPresentTick_ = 0;
    ApplyFloatingDockLayerPolicy();
    UpdatePersistentDockHostVisibility();
    InvalidateFloatingDockWindow(true);
    BeginFloatingDockKeyboardSession();
}

bool DesktopApp::
EnsureFloatingDockVisibleForAssociatedSurface(
    POINT anchorScreen)
{
    if (!snowdesktop::floating_dock_rules::
            ShouldSummonForDockSurface(
                true, floatingDockVisible_))
    {
        return floatingDockVisible_;
    }

    const HMONITOR monitor = MonitorFromPoint(
        anchorScreen, MONITOR_DEFAULTTONEAREST);
    ShowFloatingDock(monitor);
    return floatingDockVisible_;
}

void DesktopApp::CloseFloatingDock(
    FloatingDockCloseFocusPolicy focusPolicy)
{
    floatingDockHoverTargetOwner_ = nullptr;
    floatingDockHoverTargetIndex_ = 0;
    floatingDockHoverTargetKind_ = 0;
    if (floatingDockKeyboardSessionActive_)
        EndFloatingDockKeyboardSession(focusPolicy);

    if (floatingDockVisible_)
        DismissDockWindowPreviewUntilLeave();
    floatingDockVisible_ = false;
    floatingDockPointerPresentPending_ = false;
    floatingDockHoverHandoffPending_ = false;
    floatingDockHoverHandoffRect_ = {};

    // Demote the same content/backdrop pair beside WorkerW. The HWND, DComp
    // surface and native glass remain attached and continue rendering the
    // ordinary desktop Dock without any opacity or ownership exchange.
    ApplyFloatingDockLayerPolicy();
    UpdatePersistentDockHostVisibility();
    if (floatingDockHostActive_)
        InvalidateFloatingDockWindow(true);

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
    if (!floatingDockVisible_)
    {
        if (action)
            action();
        return;
    }

    floatingDockPostCloseAction_ = std::move(action);
    CloseFloatingDock(focusPolicy);
}

void DesktopApp::ToggleFloatingDock()
{
    if (floatingDockVisible_)
        CloseFloatingDock();
    else
        ShowFloatingDock();
}

void DesktopApp::InvalidateFloatingDockWindow(
    bool immediate)
{
    if (snowdesktop::floating_dock_rules::
            ShouldRenderFloatingDockFrame(
                floatingDockHostActive_) &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        InvalidateRect(
            floatingDockHwnd_, nullptr, FALSE);
        // WM_MOUSEMOVE 和 OLE DragOver 会持续占满输入队列，只 InvalidateRect
        // 会让放大/插入预览等队列空闲才绘制，快速扫过时明显落后指针。
        // immediate 必须同步 UpdateWindow。历史回归：f29a882 曾改成
        // floatingDockPointerPresentPending_ + EnsureUiAnimationFrame()，
        // 导致浮动 Dock hover 和拖放反馈晚一帧；仅当合成绘制重入时才允许兜底。
        if (immediate)
        {
            if (!floatingDockCompositionPaintInProgress_)
                UpdateWindow(floatingDockHwnd_);
            else
            {
                floatingDockPointerPresentPending_ = true;
                EnsureUiAnimationFrame();
            }
        }
    }
}

POINT DesktopApp::FloatingDockClientToDesktop(
    POINT point) const
{
    if (floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_) &&
        hwnd_ && IsWindow(hwnd_))
    {
        MapWindowPoints(
            floatingDockHwnd_, hwnd_,
            &point, 1);
        return point;
    }
    return snowdesktop::floating_dock_rules::
        WindowPointToDesktopPoint(
            point, floatingDockSourceRect_);
}

HRESULT DesktopApp::
CreateOrResizeFloatingDockCompositionSurface()
{
    if (!dcompDevice_ || !floatingDockHwnd_ ||
        !IsWindow(floatingDockHwnd_))
        return E_UNEXPECTED;
    RECT client{};
    GetClientRect(floatingDockHwnd_, &client);
    const UINT width = static_cast<UINT>(
        std::max<LONG>(1, client.right));
    const UINT height = static_cast<UINT>(
        std::max<LONG>(1, client.bottom));

    if (!floatingDockDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            floatingDockHwnd_, FALSE,
            &floatingDockDcompTarget_);
        if (FAILED(hr))
            return hr;
    }
    if (!floatingDockDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &floatingDockDcompVisual_);
        if (FAILED(hr) || !floatingDockDcompVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = floatingDockDcompTarget_->SetRoot(
            floatingDockDcompVisual_.Get());
        if (FAILED(hr))
            return hr;
    }
    if (floatingDockDcompSurface_ &&
        floatingDockCompWidth_ == width &&
        floatingDockCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr))
        return hr;
    hr = floatingDockDcompVisual_->SetContent(
        surface.Get());
    if (FAILED(hr))
        return hr;
    CommitCompositionAnimationFrame();
    if (!FlushPendingCompositionCommit())
        return E_FAIL;
    floatingDockDcompSurface_ = surface;
    floatingDockCompWidth_ = width;
    floatingDockCompHeight_ = height;
    return S_OK;
}

void DesktopApp::
RecoverFloatingDockCompositionFailure(
    const wchar_t* stage, HRESULT hr)
{
    wchar_t message[192]{};
    wsprintfW(message,
        L"FloatingDock %s FAILED hr=0x%08X; resetting composition surface",
        stage ? stage : L"Render",
        static_cast<unsigned>(hr));
    WriteDiagnosticLogEntry(message);
    ResetFloatingDockCompositionResources();
    if (!floatingDockCompositionRenderRecoveryPending_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        floatingDockCompositionRenderRecoveryPending_ = true;
        InvalidateRect(
            floatingDockHwnd_, nullptr, FALSE);
    }
}
