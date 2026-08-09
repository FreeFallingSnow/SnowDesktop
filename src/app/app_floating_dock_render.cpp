#include "app.h"

// Floating-Dock paint and window-message dispatch.

bool DesktopApp::RenderFloatingDockCompositionFrame()
{
    if (floatingDockCompositionPaintInProgress_)
        return false;
    floatingDockCompositionPaintInProgress_ = true;
    struct FloatingPaintScope final
    {
        bool& active;
        ~FloatingPaintScope() { active = false; }
    } paintScope{
        floatingDockCompositionPaintInProgress_
    };

    HRESULT hr =
        CreateOrResizeFloatingDockCompositionSurface();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            L"CreateOrResize", hr);
        return false;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    // IDCompositionSurface rejects partial BeginDraw rectangles on this
    // HWND-backed path with E_INVALIDARG. The surface allocation is stable
    // for the lifetime of the visible floating Dock, so redraw the existing
    // compact surface without recreating it.
    hr = floatingDockDcompSurface_->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverFloatingDockCompositionFailure(
            L"BeginDraw", hr);
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(
        D2D1::Matrix3x2F::Translation(
            static_cast<float>(
                updateOffset.x -
                floatingDockSourceRect_.left),
            static_cast<float>(
                updateOffset.y -
                floatingDockSourceRect_.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(
        D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    renderingFloatingDock_ = true;
    floatingDockBackdropCompositor_.BeginFrame(true);
    if (floatingDockContainer_)
    {
        floatingDockContainer_->DrawChrome(
            context.Get(), lastMousePoint_);
        floatingDockContainer_->DrawContents(
            context.Get());
    }
    DrawDynamicOverlays(context.Get());
    floatingDockBackdropCompositor_.EndFrame();
    renderingFloatingDock_ = false;

    context->SetTransform(
        D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = floatingDockDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            L"EndDraw", hr);
        return false;
    }
    if (!CommitCompositionAnimationFrame())
    {
        RecoverFloatingDockCompositionFailure(
            L"Queue Commit", E_FAIL);
        return false;
    }
    floatingDockFrameReady_ = true;
    floatingDockCompositionRenderRecoveryPending_ =
        false;
    return true;
}

void DesktopApp::PaintFloatingDockWindow(
    HWND hwnd)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (!dc)
        return;
    const bool mayRender =
        snowdesktop::floating_dock_rules::
            ShouldRenderFloatingDockFrame(
                floatingDockVisible_,
                floatingDockClosePending_);
    const bool rendered = mayRender &&
        RenderFloatingDockCompositionFrame();
    EndPaint(hwnd, &paint);
    if (mayRender && !rendered)
        InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT DesktopApp::HandleFloatingDockMessage(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto desktopLParam = [&]() {
        const POINT point = FloatingDockClientToDesktop(
            POINT{ GET_X_LPARAM(lp),
                GET_Y_LPARAM(lp) });
        return MAKELPARAM(point.x, point.y);
    };
    auto latestDesktopPointerLParam = [&]() {
        POINT point{};
        if (GetCursorPos(&point) &&
            hwnd_ && IsWindow(hwnd_))
        {
            ScreenToClient(hwnd_, &point);
            return MAKELPARAM(point.x, point.y);
        }
        return desktopLParam();
    };

    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return floatingDockVisible_ &&
                !floatingDockClosePending_
            ? HTCLIENT : HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFloatingDockWindow(hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        floatingDockHoverHandoffPending_ = false;
        floatingDockHoverHandoffRect_ = {};
        TRACKMOUSEEVENT tracking{ sizeof(tracking) };
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        handlingFloatingDockInput_ = true;
        OnMouseMove(wp,
            latestDesktopPointerLParam());
        handlingFloatingDockInput_ = false;
        // Passive hover is presented once below. Updating the title/input
        // region must not synchronously redraw the same large DComp surface.
        UpdateFloatingDockWindowBounds(false);
        PresentPointerInteractionFrame();
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // Hiding the floating HWND generates a synthetic leave before the
        // desktop HWND receives its hand-off move. Preserve the already-hot
        // Dock item across that window boundary instead of clearing and then
        // replaying the same hover transition.
        if (floatingDockHoverHandoffPending_ &&
            !floatingDockVisible_)
        {
            floatingDockHoverHandoffPending_ = false;
            floatingDockHoverHandoffRect_ = {};
            return 0;
        }
        if (floatingDockHoverHandoffPending_)
        {
            floatingDockHoverHandoffPending_ = false;
            floatingDockHoverHandoffRect_ = {};
        }
        POINT cursor{};
        if (GetCursorPos(&cursor))
        {
            ScreenToClient(hwnd, &cursor);
            cursor =
                FloatingDockClientToDesktop(
                    cursor);
            if (snowdesktop::
                    floating_dock_rules::
                        IsPointInVisibleLayer(
                            cursor,
                            floatingDockRect_,
                            floatingDockPopupRect_,
                            floatingDockTooltipRect_))
                return 0;
        }
        OnMouseLeave();
        InvalidateFloatingDockWindow(true);
        return 0;
    }
    case WM_LBUTTONDOWN:
        handlingFloatingDockInput_ = true;
        OnLeftButtonDown(wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_LBUTTONUP:
        handlingFloatingDockInput_ = true;
        OnLeftButtonUp(
            wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_LBUTTONDBLCLK:
        return HandleMessage(
            hwnd_, msg, wp, desktopLParam());
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        OnMiddleButtonDown(wp, desktopLParam());
        return 0;
    case WM_MBUTTONUP:
        OnMiddleButtonUp(wp, desktopLParam());
        return 0;
    case WM_RBUTTONUP:
        OnRightButtonUp(desktopLParam());
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_MOUSEWHEEL:
        handlingFloatingDockInput_ = true;
        OnMouseWheel(wp, lp);
        handlingFloatingDockInput_ = false;
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        CloseFloatingDock();
        return 0;
    case WM_CLOSE:
        CloseFloatingDock();
        return 0;
    case WM_DESTROY:
        if (floatingDockHwnd_ == hwnd)
            floatingDockHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
