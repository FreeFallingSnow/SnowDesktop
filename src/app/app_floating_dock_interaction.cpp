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
    const bool wasFloatingDockVisible =
        floatingDockVisible_;
    floatingDockVisible_ = true;
    floatingDockLastPointerPresentTick_ = 0;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasFloatingDockVisible,
                floatingDockVisible_))
    {
        // A drag frame caches the desktop Dock as part of its static layer.
        // Remove both that bitmap and its independent backdrop panel before
        // presenting the top-level copy, otherwise both Docks remain visible.
        InvalidateDragStaticScene();
        desktopBackdropCompositor_.RemovePanel(
            desktopDockPanelRect);
    }
    floatingDockRevealPending_ = true;
    UpdateFloatingDockWindowBounds();
    // Commit the floating copy before removing the corresponding desktop
    // copy. A one-frame overlap is visually stable; the reverse order exposes
    // the wallpaper and causes the switch flash reported by users.
    InvalidateFloatingDockWindow(true);
    floatingDockBackdropCompositor_.
        SetVisible(true);
    SetWindowPos(
        floatingDockHwnd_, HWND_NOTOPMOST,
        0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE |
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
    floatingDockBackdropCompositor_.
        Reattach(floatingDockHwnd_);
    floatingDockRevealPending_ = false;
    floatingDockLastPointerPresentTick_ = 0;
    if (hwnd_)
    {
        InvalidateRect(
            hwnd_, &desktopDockRect, TRUE);
        UpdateWindow(hwnd_);
    }
}

void DesktopApp::CloseFloatingDock(
    bool closeDockPopup)
{
    if (!floatingDockVisible_ &&
        (!floatingDockHwnd_ ||
            !IsWindowVisible(floatingDockHwnd_)))
        return;
    HideDockWindowPreview();
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
    // Restore the desktop copy first, then hide the floating copy. This keeps
    // the hand-off free of a transparent intermediate frame.
    const bool wasFloatingDockVisible =
        floatingDockVisible_;
    floatingDockVisible_ = false;
    if (snowdesktop::floating_dock_rules::
            FloatingVisibilityChangesStaticScene(
                wasFloatingDockVisible,
                floatingDockVisible_))
    {
        InvalidateDragStaticScene();
    }
    floatingDockRevealPending_ = false;
    floatingDockLastPointerPresentTick_ = 0;
    if (hwnd_)
    {
        InvalidateRect(
            hwnd_,
            IsRectEmpty(&desktopDockRect)
                ? nullptr : &desktopDockRect,
            TRUE);
        UpdateWindow(hwnd_);
    }
    if (floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        ShowWindow(floatingDockHwnd_, SW_HIDE);
    }
    floatingDockBackdropCompositor_.Reset();
    const bool hadCompositionSurface =
        floatingDockDcompSurface_ != nullptr;
    ResetFloatingDockCompositionResources();
    if (hadCompositionSurface && dcompDevice_)
    {
        // The hidden host and its lightweight target/visual are reusable, but
        // retaining the last full-size transparent surface needlessly pins
        // GPU memory. Commit the null content while the HWND is hidden so the
        // compositor can retire that allocation before the next reveal.
        const HRESULT hr = dcompDevice_->Commit();
        if (FAILED(hr))
        {
            wchar_t message[160]{};
            wsprintfW(message,
                L"FloatingDock release surface commit FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
            WriteDiagnosticLogEntry(message);
        }
    }
    floatingDockContainer_ = nullptr;
    floatingDockMonitor_ = nullptr;
    floatingDockSourceRect_ = {};
    floatingDockRect_ = {};
    floatingDockPopupRect_ = {};
    floatingDockTooltipRect_ = {};
}

void DesktopApp::ToggleFloatingDock()
{
    if (floatingDockVisible_)
        CloseFloatingDock();
    else
        ShowFloatingDock();
}

void DesktopApp::InvalidateFloatingDockWindow(
    bool immediate) const
{
    if (floatingDockVisible_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
    {
        InvalidateRect(
            floatingDockHwnd_, nullptr, FALSE);
        // WM_MOUSEMOVE and OLE DragOver can keep the UI thread's input queue
        // continuously busy. A plain invalidation is then painted only after
        // the queue becomes idle, which makes Dock magnification and insertion
        // previews appear frozen. Force only the already-invalid floating
        // surface to paint; do not resize or rebuild its composition resources.
        if (immediate &&
            !floatingDockCompositionPaintInProgress_)
            UpdateWindow(floatingDockHwnd_);
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
