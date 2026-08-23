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
    if (!CreateFloatingDockWindow())
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock CreateWindowEx failed");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    HideDockWindowPreview();

    // Layout rebuilding may replace every runtime DockContainer. Resolve the
    // pointer at the last possible moment and never retain it while hidden.
    floatingDockContainer_ = preferredMonitor
        ? SelectFloatingDockContainerForMonitor(
            preferredMonitor)
        : SelectFloatingDockContainerAtCursor();
    if (!floatingDockContainer_)
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock shortcut ignored: no Dock container");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // A completion notification from the previous close must not retire the
    // targets being reused by this reveal.
    ++floatingDockBackdropCommitToken_;
    floatingDockBackdropCleanupPending_ = false;
    floatingDockRevealCommitPending_ = false;
    floatingDockDesktopCommitPending_ = false;
    floatingDockClosePending_ = false;
    floatingDockCloseDesktopRect_ = {};
    floatingDockHoverHandoffPending_ = false;
    floatingDockHoverHandoffRect_ = {};
    const RECT selectedDockBounds =
        floatingDockContainer_->GetBounds();
    const POINT selectedDockCenterScreen{
        (selectedDockBounds.left +
            selectedDockBounds.right) / 2 +
            virtualLeft_,
        (selectedDockBounds.top +
            selectedDockBounds.bottom) / 2 +
            virtualTop_
    };
    floatingDockMonitor_ = MonitorFromPoint(
        selectedDockCenterScreen,
        MONITOR_DEFAULTTONEAREST);
    const RECT desktopDockRect =
        floatingDockContainer_->
            GetInteractiveBounds();
    const RECT desktopDockPanelRect =
        floatingDockContainer_->
            GetVisualPanelBounds(
                lastMousePoint_);
    floatingDockPersonalization_ =
        PersonalizationSettings::DarkPreset();
    if (settingsWindow_)
        floatingDockPersonalization_ =
            settingsWindow_->GetPersonalization();
    else
        LoadPersonalization(
            GetPersonalizationPath().c_str(),
            floatingDockPersonalization_);
    floatingDockVisible_ = true;
    floatingDockDesktopCopySuppressed_ = false;
    floatingDockLastPointerPresentTick_ = 0;
    floatingDockRevealPending_ = true;
    UpdateFloatingDockWindowBounds();
    if (!floatingDockVisible_ ||
        !floatingDockContainer_)
        return;

    // Render the replacement while the desktop copy is still owned by the
    // desktop surface. Commit() only queues DirectComposition work, so a
    // successfully returned paint is necessary but not yet sufficient to
    // retire that source copy.
    const bool firstFrameRendered =
        RenderFloatingDockCompositionFrame();
    if (!firstFrameRendered ||
        !floatingDockFrameReady_)
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock first frame unavailable; desktop copy retained");
        CloseFloatingDock();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // UpdateFloatingDockWindowBounds invalidated the hidden HWND while sizing
    // it. The frame was submitted directly because hidden windows do not
    // reliably receive synchronous WM_PAINT; consume that stale update now.
    ValidateRect(floatingDockHwnd_, nullptr);

    // Attach an already-rendered but fully transparent visual to the DWM
    // scene first. The next desktop paint can then remove the desktop copy
    // and reveal this visual in one DirectComposition transaction instead of
    // displaying either an overlap frame or a wallpaper frame.
    HRESULT stageHr = floatingDockDcompEffect_
        ? floatingDockDcompEffect_->SetOpacity(0.0f)
        : E_UNEXPECTED;
    if (SUCCEEDED(stageHr))
    {
        stageHr = floatingDockDesktopCacheEffect_
            ? floatingDockDesktopCacheEffect_->SetOpacity(0.0f)
            : E_UNEXPECTED;
    }
    if (SUCCEEDED(stageHr))
    {
        CommitCompositionAnimationFrame();
        stageHr = FlushPendingCompositionCommit()
            ? S_OK : E_FAIL;
    }
    if (SUCCEEDED(stageHr) &&
        floatingDockBackdropCompositor_.IsAvailable())
    {
        if (!floatingDockBackdropCompositor_.
                SetVisualOpacity(0.0f))
            stageHr = E_FAIL;
        else
            floatingDockBackdropCompositor_.
                CommitVisualChanges();
    }
    if (FAILED(stageHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock transparent staging FAILED hr=0x%08X",
            static_cast<unsigned>(stageHr));
        WriteDiagnosticLogEntry(message);
        CloseFloatingDock();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    SetWindowPos(
        floatingDockHwnd_,
        snowdesktop::floating_dock_rules::
                ShouldFloatingDockBeTopmost(
                    true,
                    shellPopupMenuLayerDepth_)
            ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE |
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    floatingDockBackdropCompositor_.
        Reattach(floatingDockHwnd_);
    floatingDockRevealPending_ = false;
    floatingDockLastPointerPresentTick_ = 0;

    // Showing the HWND and submitting its DComp surface live in different
    // timing domains. This barrier only installs the transparent target; the
    // currently visible desktop Dock remains the sole rendered copy.
    const bool presentationCompleted =
        WaitForCompositionPresentation(
            L"Floating Dock transparent staging");
    const bool retireDesktopCopy =
        snowdesktop::floating_dock_rules::
            ShouldRetireDesktopDockCopy(
                floatingDockFrameReady_,
                presentationCompleted);
    if (!retireDesktopCopy)
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock transparent staging did not complete; desktop copy retained");
        WriteDiagnosticLogEntry(message);
        CloseFloatingDock();
        MessageBeep(MB_ICONWARNING);
        return;
    }

    const bool wasDesktopCopySuppressed =
        floatingDockDesktopCopySuppressed_;
    floatingDockDesktopBackdropHandoffRect_ =
        desktopDockPanelRect;
    HRESULT cacheHr = floatingDockDesktopCacheEffect_
        ? floatingDockDesktopCacheEffect_->SetOpacity(1.0f)
        : E_UNEXPECTED;
    if (FAILED(cacheHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock desktop cache staging FAILED hr=0x%08X",
            static_cast<unsigned>(cacheHr));
        WriteDiagnosticLogEntry(message);
        CloseFloatingDock();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    floatingDockDesktopCopySuppressed_ = true;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasDesktopCopySuppressed,
                floatingDockDesktopCopySuppressed_))
    {
        // A drag frame caches the desktop Dock as part of its static layer.
        InvalidateDragStaticScene();
    }
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(
            hwnd_, &desktopDockRect, TRUE);
        UpdateWindow(hwnd_);
    }
    // The popup backdrop target is already staged at zero opacity. It can be
    // inserted now without sampling the retained desktop glass twice.
    floatingDockBackdropCompositor_.
        SetVisible(true);
    floatingDockBackdropCompositor_.
        Reattach(floatingDockHwnd_);
    // The desktop main surface no longer owns the Dock, but the exact surface
    // rendered for the floating host is now visible through the desktop cache
    // child. Exchange those two identical copies without asking either HWND
    // to repaint during the ownership switch.
    HRESULT revealHr =
        floatingDockDcompEffect_->SetOpacity(1.0f);
    if (SUCCEEDED(revealHr))
    {
        revealHr = floatingDockDesktopCacheEffect_->
            SetOpacity(0.0f);
    }
    if (SUCCEEDED(revealHr))
    {
        // Submit the content exchange, but do not wait for it in isolation.
        // The retained desktop glass is still the source underneath this
        // frame; queue the shared WinComp glass exchange immediately below
        // so DWM can present both ownership changes in one cycle.
        CommitCompositionAnimationFrame();
        revealHr = FlushPendingCompositionCommit()
            ? S_OK : E_FAIL;
    }
    if (FAILED(revealHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock atomic reveal FAILED hr=0x%08X",
            static_cast<unsigned>(revealHr));
        WriteDiagnosticLogEntry(message);
        floatingDockDesktopCopySuppressed_ = false;
        InvalidateDragStaticScene();
        CloseFloatingDock();
        MessageBeep(MB_ICONWARNING);
        return;
    }
    // Both backdrop targets share one Windows Composition compositor. Queue
    // their opacity exchange as one transaction and observe its asynchronous
    // completion; the retained desktop panel remains the compatibility source
    // until both composition queues settle.
    const bool floatingGlassReady =
        !floatingDockBackdropCompositor_.IsAvailable() ||
        floatingDockBackdropCompositor_.
            SetVisualOpacity(1.0f);
    if (floatingGlassReady)
    {
        desktopBackdropCompositor_.SetPanelOpacity(
            desktopDockPanelRect, 0.0f);
        const HWND notifyWindow =
            controlHwnd_ && IsWindow(controlHwnd_)
                ? controlHwnd_ : hwnd_;
        const UINT_PTR commitToken =
            ++floatingDockBackdropCommitToken_;
        floatingDockRevealCommitPending_ =
            floatingDockBackdropCompositor_.
                CommitVisualChangesAndNotify(
                    notifyWindow,
                    kFloatingDockBackdropCommitMessage,
                    commitToken);
        if (!floatingDockRevealCommitPending_)
        {
            // Older Windows Composition implementations do not expose a
            // completion callback. Preserve one compatibility barrier only
            // on that path.
            floatingDockBackdropCompositor_.
                CommitVisualChanges();
            DwmFlush();
        }
    }
    // Keep the now-transparent desktop panel alive while floating. This is a
    // deliberate standby source for the reverse transaction and prevents a
    // later desktop repaint from retiring it before the asynchronous shared
    // compositor commit has landed.
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
    if (floatingDockHoverTailToken_)
    {
        uiAnimationScheduler_.Cancel(
            floatingDockHoverTailToken_);
        floatingDockHoverTailToken_ = 0;
    }
    floatingDockHoverTargetOwner_ = nullptr;
    floatingDockHoverTargetIndex_ = 0;
    floatingDockHoverTargetKind_ = 0;
    if (floatingDockKeyboardSessionActive_)
        EndFloatingDockKeyboardSession(focusPolicy);
    if (floatingDockClosePending_)
        return;
    if (!floatingDockVisible_ &&
        (!floatingDockHwnd_ ||
            !IsWindowVisible(floatingDockHwnd_)))
        return;
    if (floatingDockRevealCommitPending_)
    {
        ++floatingDockBackdropCommitToken_;
        floatingDockRevealCommitPending_ = false;
    }
    // A preview that has already completed its hover dwell must not be armed
    // again merely because input ownership moves from the floating HWND back
    // to the desktop HWND. Suppress that same target until the real pointer
    // leaves it.
    DismissDockWindowPreviewUntilLeave();
    const RECT desktopDockRect =
        floatingDockContainer_
            ? floatingDockContainer_->
                GetInteractiveBounds()
            : floatingDockRect_;
    const RECT desktopDockPanelRect =
        floatingDockContainer_
            ? floatingDockContainer_->
                GetVisualPanelBounds(
                    lastMousePoint_)
            : floatingDockRect_;
    floatingDockCloseDesktopRect_ = desktopDockRect;

    POINT handoffPointer{};
    floatingDockHoverHandoffPending_ =
        TryGetDesktopHoverPointFromCursor(
            handoffPointer) &&
        PtInRect(&desktopDockRect,
            handoffPointer) != FALSE;
    floatingDockHoverHandoffRect_ =
        floatingDockHoverHandoffPending_
            ? desktopDockRect : RECT{};
    if (floatingDockHoverHandoffPending_)
        lastMousePoint_ = handoffPointer;

    // Freeze the hand-off cache from the final floating state after pointer
    // reconciliation. Popup state belongs to the independent shared popup
    // host and must not participate in this Dock surface transaction.
    if (!RenderFloatingDockCompositionFrame())
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock close cache refresh unavailable");
    }
    // The frame above is now the immutable hand-off cache. Consume any stale
    // WM_PAINT queued by the pointer press so it cannot redraw the shared
    // surface after the close transaction becomes pending.
    if (floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
        ValidateRect(floatingDockHwnd_, nullptr);

    // Stage the desktop glass target without presenting it. The content and
    // glass ownership exchanges are queued together below, matching the
    // reveal path and Quick Navigation's paired content/backdrop updates.
    const PersonalizationSettings& glassSettings =
        floatingDockPersonalization_;
    const float desktopPanelRadius =
        dockSettings_.edgeAttached
            ? 0.0f
            : glassSettings.cornerRadius;
    const bool canTransferGlass =
        glassSettings.glassEnabled &&
        desktopBackdropCompositor_.IsAvailable() &&
        floatingDockBackdropCompositor_.IsAvailable();
    bool stagedDesktopGlass = false;
    if (canTransferGlass)
    {
        desktopBackdropCompositor_.BeginFrame(false);
        stagedDesktopGlass =
            desktopBackdropCompositor_.AddPanel(
                desktopDockPanelRect,
                desktopPanelRadius,
                glassSettings.glassBlurRadius,
                reinterpret_cast<std::uintptr_t>(
                    floatingDockContainer_)) &&
            desktopBackdropCompositor_.SetPanelOpacity(
                desktopDockPanelRect, 0.0f);
        // This is only the invisible target staging step. Do not submit its
        // zero opacity on its own; the desktop/floating opacity exchange below
        // is the single shared-compositor commit for this hand-off.
        desktopBackdropCompositor_.EndFrame(false);
        if (!stagedDesktopGlass)
        {
            floatingDockBackdropCompositor_.
                SetVisible(false);
        }
    }
    else
    {
        floatingDockBackdropCompositor_.
            SetVisible(false);
    }

    // Queue the reverse of ShowFloatingDock's content exchange without
    // waiting for DComp in isolation. Waiting here used to make WinComp hide
    // the floating glass for a complete frame while its content HWND remained
    // visible. Both channels now receive one logical close state before any
    // asynchronous completion is observed.
    HRESULT concealHr = floatingDockDesktopCacheEffect_
        ? floatingDockDesktopCacheEffect_->SetOpacity(1.0f)
        : E_UNEXPECTED;
    if (SUCCEEDED(concealHr))
    {
        concealHr = floatingDockDcompEffect_
            ? floatingDockDcompEffect_->SetOpacity(0.0f)
            : E_UNEXPECTED;
    }
    if (SUCCEEDED(concealHr))
    {
        CommitCompositionAnimationFrame();
        concealHr = FlushPendingCompositionCommit()
            ? S_OK : E_FAIL;
    }
    if (FAILED(concealHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock paired conceal FAILED hr=0x%08X",
            static_cast<unsigned>(concealHr));
        WriteDiagnosticLogEntry(message);
    }

    bool deferBackdropHandoff = false;
    if (stagedDesktopGlass)
    {
        desktopBackdropCompositor_.SetPanelOpacity(
            desktopDockPanelRect, 1.0f);
        floatingDockBackdropCompositor_.
            SetVisualOpacity(0.0f);
        const HWND notifyWindow =
            controlHwnd_ && IsWindow(controlHwnd_)
                ? controlHwnd_ : hwnd_;
        const UINT_PTR commitToken =
            ++floatingDockBackdropCommitToken_;
        deferBackdropHandoff =
            floatingDockBackdropCompositor_.
                CommitVisualChangesAndNotify(
                    notifyWindow,
                    kFloatingDockBackdropCommitMessage,
                    commitToken);
        floatingDockBackdropCleanupPending_ =
            deferBackdropHandoff;
        if (!deferBackdropHandoff)
        {
            // Older compositors do not expose a completion callback. Both
            // ownership exchanges are already queued, so one compatibility
            // barrier covers the paired close state.
            floatingDockBackdropCompositor_.
                CommitVisualChanges();
            DwmFlush();
        }
    }

    floatingDockRevealPending_ = false;
    floatingDockLastPointerPresentTick_ = 0;
    if (deferBackdropHandoff)
    {
        // Both content and glass exchanges are already queued. The completion
        // message fences the content frame and hides the two popup-owned HWNDs
        // together; it does not initiate a second visible ownership phase.
        floatingDockClosePending_ = true;
        return;
    }
    CompleteFloatingDockCloseHandoff();
}

void DesktopApp::CloseFloatingDockThen(
    std::function<void()> action,
    FloatingDockCloseFocusPolicy focusPolicy)
{
    const bool floatingWindowVisible =
        floatingDockHwnd_ &&
        IsWindowVisible(floatingDockHwnd_);
    if (snowdesktop::floating_dock_rules::
            CanRunPostCloseActionImmediately(
                floatingDockVisible_,
                floatingDockClosePending_,
                floatingWindowVisible))
    {
        if (action)
            action();
        return;
    }

    floatingDockPostCloseAction_ = std::move(action);
    CloseFloatingDock(focusPolicy);
}

void DesktopApp::CompleteFloatingDockCloseHandoff()
{
    const RECT desktopDockRect =
        floatingDockCloseDesktopRect_;

    // CloseFloatingDock queued the content exchange beside the WinComp glass
    // exchange. Fence that already-submitted DComp state before withdrawing
    // the popup-owned windows; do not create a second visible close phase.
    if (!WaitForCompositionPresentation(
            L"Floating Dock paired conceal"))
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock paired conceal did not complete");
    }

    floatingDockVisible_ = false;
    floatingDockPointerPresentPending_ = false;
    // The content HWND and its backdrop helper belong to this Dock lifecycle.
    // Hide them in one User32 transaction so a late WinComp presentation can
    // never expose a helper-only glass frame over the desktop cache.
    floatingDockBackdropCompositor_.HidePopupWindowPair(
        floatingDockHwnd_);
    floatingDockDesktopBackdropHandoffRect_ = {};

    // Mirror the reveal transaction on the same desktop target: retire the
    // cache property before painting the ordinary Dock, then let OnPaint's
    // single DComp commit publish both changes together. Presenting the main
    // Dock while the cache was still at opacity 1 guaranteed one darkened
    // overlap frame because both surfaces contain translucent pixels.
    const HRESULT cacheRetireHr =
        floatingDockDesktopCacheEffect_
            ? floatingDockDesktopCacheEffect_->SetOpacity(0.0f)
            : E_UNEXPECTED;
    if (FAILED(cacheRetireHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock desktop cache retire FAILED hr=0x%08X",
            static_cast<unsigned>(cacheRetireHr));
        WriteDiagnosticLogEntry(message);
    }
    const bool wasDesktopCopySuppressed =
        floatingDockDesktopCopySuppressed_;
    floatingDockDesktopCopySuppressed_ = false;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasDesktopCopySuppressed,
                floatingDockDesktopCopySuppressed_))
    {
        InvalidateDragStaticScene();
    }
    bool desktopFrameSubmitted = false;
    if (hwnd_ && IsWindow(hwnd_))
    {
        InvalidateRect(
            hwnd_,
            IsRectEmpty(&desktopDockRect)
                ? nullptr : &desktopDockRect,
            TRUE);
        if (!compositionPaintInProgress_)
        {
            UpdateWindow(hwnd_);
            desktopFrameSubmitted = true;
        }
    }
    if (desktopFrameSubmitted)
    {
        WaitForCompositionPresentation(
            L"Desktop Dock handoff");
    }

    // OnPaint submits the ordinary desktop Dock content through DComp and
    // its matching glass panel through Windows Composition. The DComp wait
    // above cannot fence that second queue. Keep the close transaction
    // pending until WinComp confirms the desktop glass frame, otherwise an
    // immediately following SetForegroundWindow/ShowWindow can reorder the
    // scene while the Dock still has no presented backdrop.
    if (desktopBackdropCompositor_.IsAvailable())
    {
        const HWND notifyWindow =
            controlHwnd_ && IsWindow(controlHwnd_)
                ? controlHwnd_ : hwnd_;
        const UINT_PTR commitToken =
            ++floatingDockBackdropCommitToken_;
        floatingDockClosePending_ = true;
        floatingDockDesktopCommitPending_ =
            desktopBackdropCompositor_.
                CommitVisualChangesAndNotify(
                    notifyWindow,
                    kFloatingDockBackdropCommitMessage,
                    commitToken);
        if (floatingDockDesktopCommitPending_)
            return;

        // Compatibility path for systems without an asynchronous commit
        // completion. Do not run the queued application command before the
        // shared backdrop transaction has crossed a DWM presentation fence.
        desktopBackdropCompositor_.CommitVisualChanges();
        DwmFlush();
    }
    FinishFloatingDockCloseHandoff();
}

void DesktopApp::FinishFloatingDockCloseHandoff()
{
    floatingDockDesktopCommitPending_ = false;
    floatingDockClosePending_ = false;
    FinalizeFloatingDockBackdropCleanup();
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
    floatingDockDesktopBackdropHandoffRect_ = {};
    floatingDockCloseDesktopRect_ = {};
    std::function<void()> action =
        std::move(floatingDockPostCloseAction_);
    floatingDockPostCloseAction_ = {};
    if (action)
        action();
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
                floatingDockVisible_,
                floatingDockClosePending_) &&
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
    if (!floatingDockDcompEffect_)
    {
        HRESULT hr = dcompDevice_->CreateEffectGroup(
            &floatingDockDcompEffect_);
        if (FAILED(hr) || !floatingDockDcompEffect_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = floatingDockDcompVisual_->SetEffect(
            floatingDockDcompEffect_.Get());
        if (FAILED(hr))
        {
            floatingDockDcompEffect_.Reset();
            return hr;
        }
    }
    HRESULT hr = EnsureFloatingDockDesktopCacheVisual();
    if (FAILED(hr))
        return hr;
    if (floatingDockDcompSurface_ &&
        floatingDockCompWidth_ == width &&
        floatingDockCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    hr = dcompDevice_->CreateSurface(
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
    hr = floatingDockDesktopCacheVisual_->SetContent(
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
