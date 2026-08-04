#include "app.h"

// Floating-Dock visibility, pointer mapping and interaction state.

void DesktopApp::ShowFloatingDock()
{
    WriteDiagnosticLogEntry(
        L"Floating Dock shortcut received");
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
    floatingDockContainer_ =
        SelectFloatingDockContainerAtCursor();
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
        CloseFloatingDock(true, true);
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
        stageHr = dcompDevice_->Commit();
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
        CloseFloatingDock(true, true);
        MessageBeep(MB_ICONWARNING);
        return;
    }
    SetWindowPos(
        floatingDockHwnd_, HWND_NOTOPMOST,
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
        WaitForDCompCommitWithFallback(
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
        CloseFloatingDock(true, true);
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
        CloseFloatingDock(true, true);
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
        revealHr = dcompDevice_->Commit();
    if (FAILED(revealHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock atomic reveal FAILED hr=0x%08X",
            static_cast<unsigned>(revealHr));
        WriteDiagnosticLogEntry(message);
        floatingDockDesktopCopySuppressed_ = false;
        InvalidateDragStaticScene();
        CloseFloatingDock(true, true);
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
}

void DesktopApp::CloseFloatingDock(
    bool closeDockPopup,
    bool forceImmediate)
{
    if (floatingDockClosePending_)
    {
        if (!forceImmediate)
            return;
        ++floatingDockBackdropCommitToken_;
        floatingDockBackdropCleanupPending_ = false;
        floatingDockRevealCommitPending_ = false;
        floatingDockClosePending_ = false;
        CompleteFloatingDockCloseHandoff();
        return;
    }
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
    if (closeDockPopup && popupAnchoredToDock_ &&
        GetOpenPopupWidget())
    {
        // Clear the popup state directly through the shared close path, while
        // keeping this host marked visible until that path has rebuilt any
        // grouped runtime container.
        CloseCollectionPopup();
        // The floating host is hidden in this same call, so it cannot present
        // the remaining close frames. Finalize here instead of letting those
        // frames leak onto the desktop-hosted Dock copy.
        FinalizeCloseCollectionPopup();
    }
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

    // Freeze the hand-off cache from the final floating state after popup
    // teardown and pointer reconciliation. Both HWND targets will reference
    // this same surface until the desktop main surface has caught up.
    if (!RenderFloatingDockCompositionFrame())
    {
        WriteDiagnosticLogEntry(
            L"Floating Dock close cache refresh unavailable");
    }

    // Stage an invisible desktop glass panel, then exchange its opacity with
    // the floating root in the shared Windows Composition transaction. The
    // floating D2D content remains the only content copy during this glass
    // hand-off and sees one continuous backdrop underneath it.
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
    bool deferBackdropHandoff = false;
    if (canTransferGlass)
    {
        desktopBackdropCompositor_.BeginFrame(false);
        const bool stagedDesktopGlass =
            desktopBackdropCompositor_.AddPanel(
                desktopDockPanelRect,
                desktopPanelRadius,
                glassSettings.glassBlurRadius) &&
            desktopBackdropCompositor_.SetPanelOpacity(
                desktopDockPanelRect, 0.0f);
        desktopBackdropCompositor_.EndFrame();
        if (stagedDesktopGlass)
        {
            // Do not present the zero-opacity staging panel on its own. Queue
            // the glass ownership exchange now, then let the desktop D2D
            // paint below commit both hand-offs before the single final DWM
            // barrier. Separate barriers here made close visibly happen in
            // three steps: glass changed first, content followed later.
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
                // On systems without a completion fence, keep submitting the
                // opacity exchange but retain the zero-opacity helper until
                // the next reveal or final app teardown.
                floatingDockBackdropCompositor_.
                    CommitVisualChanges();
            }
        }
        else
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

    floatingDockRevealPending_ = false;
    floatingDockLastPointerPresentTick_ = 0;
    if (deferBackdropHandoff && !forceImmediate)
    {
        // Keep the floating content and its old glass paired until WinComp
        // confirms that the desktop glass target is live. The completion
        // message performs the DComp content exchange on the UI thread.
        floatingDockClosePending_ = true;
        return;
    }
    if (deferBackdropHandoff)
    {
        ++floatingDockBackdropCommitToken_;
        floatingDockBackdropCleanupPending_ = false;
    }
    CompleteFloatingDockCloseHandoff();
}

void DesktopApp::CompleteFloatingDockCloseHandoff()
{
    const RECT desktopDockRect =
        floatingDockCloseDesktopRect_;

    // First move the exact floating surface to the desktop child visual. If
    // DWM presents the two HWND targets on adjacent frames, both copies are
    // pixel-identical (including the current hover state), so no second Dock
    // or empty panel can be perceived.
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
        concealHr = dcompDevice_->Commit();
    if (FAILED(concealHr))
    {
        wchar_t message[176]{};
        wsprintfW(message,
            L"Floating Dock cached conceal FAILED hr=0x%08X",
            static_cast<unsigned>(concealHr));
        WriteDiagnosticLogEntry(message);
    }
    else
        WaitForDCompCommitWithFallback(
            L"Floating Dock cached conceal");

    floatingDockVisible_ = false;
    floatingDockPointerPresentPending_ = false;
    if (floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        ShowWindow(floatingDockHwnd_, SW_HIDE);
    }
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
        WaitForDCompCommitWithFallback(
            L"Desktop Dock handoff");
    }
    FinalizeFloatingDockBackdropCleanup();
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
    floatingDockDesktopBackdropHandoffRect_ = {};
    floatingDockCloseDesktopRect_ = {};
}

void DesktopApp::ToggleFloatingDock()
{
    if (floatingDockVisible_)
        // A hotkey toggle changes only the Dock host. Keep an anchored popup
        // alive so the reverse hand-off mirrors ShowFloatingDock instead of
        // turning a surface transition into a popup-close command.
        CloseFloatingDock(false);
    else
        ShowFloatingDock();
}

void DesktopApp::InvalidateFloatingDockWindow(
    bool immediate)
{
    if (floatingDockVisible_ &&
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
    hr = dcompDevice_->Commit();
    if (FAILED(hr))
        return hr;
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
