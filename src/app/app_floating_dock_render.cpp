#include "app.h"
#include "../drag_input_rules.h"

// Floating-Dock paint and window-message dispatch.

bool DesktopApp::RenderFloatingDockCompositionFrame(
    PersistentDockHost& host)
{
    if (host.compositionPaintInProgress)
        return false;
    host.compositionPaintInProgress = true;
    struct FloatingPaintScope final
    {
        bool& active;
        PersistentDockHost*& renderingHost;
        PersistentDockHost* previousHost;
        ~FloatingPaintScope()
        {
            active = false;
            renderingHost = previousHost;
        }
    } paintScope{
        host.compositionPaintInProgress,
        renderingPersistentDockHost_,
        renderingPersistentDockHost_
    };
    renderingPersistentDockHost_ = &host;

    const bool preserveExistingFrame =
        host.frameReady && host.dcompSurface &&
        host.dcompVisual;
    ComPtr<IDCompositionSurface> frameSurface;
    HRESULT hr =
        CreateOrResizeFloatingDockCompositionSurface(
            host, frameSurface);
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            host, L"CreateOrResize", hr,
            preserveExistingFrame);
        return false;
    }
    const bool replacingSurface =
        frameSurface.Get() != host.dcompSurface.Get();

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    // IDCompositionSurface rejects partial BeginDraw rectangles on this
    // HWND-backed path with E_INVALIDARG. Redraw the complete compact surface;
    // a resized replacement remains detached until this frame is finished.
    hr = frameSurface->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        RecoverFloatingDockCompositionFailure(
            host, L"BeginDraw",
            FAILED(hr) ? hr : E_FAIL,
            preserveExistingFrame && replacingSurface);
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
                host.sourceRect.left),
            static_cast<float>(
                updateOffset.y -
                host.sourceRect.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(
        D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    renderingFloatingDock_ = true;
    host.backdrop.BeginFrame(true);
    if (host.container)
    {
        host.container->DrawChrome(
            context.Get(), lastMousePoint_);
        host.container->DrawContents(
            context.Get());
    }
    DrawDynamicOverlays(context.Get());
    host.backdrop.EndFrame();
    renderingFloatingDock_ = false;

    context->SetTransform(
        D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = frameSurface->EndDraw();
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            host, L"EndDraw", hr,
            preserveExistingFrame && replacingSurface);
        return false;
    }
    // SetContent only after the replacement surface contains a complete
    // frame. The following composition commit then publishes the pixels and
    // the content switch atomically instead of exposing an empty surface.
    hr = host.dcompVisual->SetContent(
        frameSurface.Get());
    if (FAILED(hr))
    {
        RecoverFloatingDockCompositionFailure(
            host, L"SetContent", hr,
            preserveExistingFrame && replacingSurface);
        return false;
    }
    if (replacingSurface)
    {
        RECT client{};
        GetClientRect(host.hwnd, &client);
        host.dcompSurface = frameSurface;
        host.compWidth = static_cast<UINT>(
            std::max<LONG>(1, client.right - client.left));
        host.compHeight = static_cast<UINT>(
            std::max<LONG>(1, client.bottom - client.top));
    }
    const bool deferredWidgetsFlushed =
        FlushPendingDesktopWidgetComposition() &&
        FlushPendingWidgetMarqueeComposition() &&
        SyncWidgetMarqueeCompositionVisibility();
    if (!deferredWidgetsFlushed && hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    if (!CommitCompositionAnimationFrame())
    {
        RecoverFloatingDockCompositionFailure(
            host, L"Queue Commit", E_FAIL);
        return false;
    }
    host.frameReady = true;
    host.compositionRenderRecoveryPending =
        false;
    return true;
}

void DesktopApp::PaintFloatingDockWindow(
    PersistentDockHost& host,
    HWND hwnd)
{
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (!dc)
        return;
    const bool mayRender =
        snowdesktop::floating_dock_rules::
            ShouldRenderFloatingDockFrame(
                host.active);
    const bool rendered = mayRender &&
        RenderFloatingDockCompositionFrame(host);
    EndPaint(hwnd, &paint);
    if (mayRender && !rendered)
        InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT DesktopApp::HandleFloatingDockMessage(
    PersistentDockHost& host,
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct NativeMenuPresentationScope final
    {
        DesktopApp& app;
        ~NativeMenuPresentationScope()
        {
            app.FlushNativeMenuPresentation();
        }
    } nativeMenuPresentationScope{ *this };

    auto desktopPoint = [&]() {
        return FloatingDockClientToDesktop(
            host,
            POINT{ GET_X_LPARAM(lp),
                GET_Y_LPARAM(lp) });
    };
    auto desktopLParam = [&]() {
        const POINT point = desktopPoint();
        return MAKELPARAM(point.x, point.y);
    };
    auto latestDesktopPointer = [&]() {
        const bool nativeDragActive =
            snowdesktop::drag_input_rules::IsNativeDragActive(
                dragSession_.IsActive(),
                dragDropController_.IsTransportActive());
        const bool primaryButtonDown =
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        POINT point{};
        if (snowdesktop::drag_input_rules::
                ShouldSampleFloatingWindowPointer(
                    nativeDragActive, primaryButtonDown) &&
            GetCursorPos(&point) &&
            hwnd_ && IsWindow(hwnd_) &&
            ScreenToClient(hwnd_, &point))
        {
            return point;
        }
        return desktopPoint();
    };

    switch (msg)
    {
    case WM_WINDOWPOSCHANGING:
        if (lp)
            PreserveModernMenuHostZOrder(
                hwnd, *reinterpret_cast<WINDOWPOS*>(lp));
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
    {
        if (!host.active)
            return HTTRANSPARENT;
        POINT hitDesktopPoint{
            GET_X_LPARAM(lp),
            GET_Y_LPARAM(lp)
        };
        if (hwnd_ && IsWindow(hwnd_) &&
            ScreenToClient(hwnd_, &hitDesktopPoint) &&
            snowdesktop::floating_dock_rules::
                IsTooltipOnlyPoint(
                    hitDesktopPoint,
                    host.dockRect,
                    host.popupRect,
                    host.tooltipRect))
        {
            // The title chip is visual feedback, not an interaction surface.
            // Passing its hit through lets an upward exit reach the paired
            // desktop window and prevents an invisible stale title region
            // from intercepting left or right button input.
            return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        PaintFloatingDockWindow(host, hwnd);
        return 0;
    case WM_MOUSEMOVE:
    {
        SelectPersistentDockHost(&host);
        floatingDockHoverHandoffPending_ = false;
        floatingDockHoverHandoffRect_ = {};
        TRACKMOUSEEVENT tracking{ sizeof(tracking) };
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = hwnd;
        TrackMouseEvent(&tracking);
        handlingFloatingDockInput_ = true;
        handlingPersistentDockHost_ = &host;
        bool dragPreviewSynced = false;
        OnMouseMoveAt(
            wp, latestDesktopPointer(),
            &dragPreviewSynced);
        handlingFloatingDockInput_ = false;
        handlingPersistentDockHost_ = nullptr;
        // Passive hover is presented once below. Updating the title/input
        // region must not synchronously redraw the same large DComp surface.
        UpdateFloatingDockWindowBounds(host, false);
        PresentPointerInteractionFrame(
            dragPreviewSynced);
        return 0;
    }
    case WM_MOUSELEAVE:
    {
        // Hiding the floating HWND generates a synthetic leave before the
        // desktop HWND receives its hand-off move. Dynamic title/rounded HRGN
        // updates can also post a stale leave while User32 still resolves the
        // pointer to this HWND. Retain that sample, but never rearm tracking or
        // replay full input from inside WM_MOUSELEAVE: the region can change
        // again during presentation and otherwise create a posted-message loop.
        if (floatingDockHoverHandoffPending_ &&
            !host.active)
        {
            if (rightButtonDownDockHost_ == &host)
                rightButtonDownDockHost_ = nullptr;
            floatingDockHoverHandoffPending_ = false;
            floatingDockHoverHandoffRect_ = {};
            return 0;
        }
        if (floatingDockHoverHandoffPending_)
        {
            floatingDockHoverHandoffPending_ = false;
            floatingDockHoverHandoffRect_ = {};
        }
        POINT cursorScreen{};
        if (GetCursorPos(&cursorScreen))
        {
            if (WindowFromPoint(cursorScreen) == hwnd)
            {
                // A region update can consume the current leave subscription
                // while the pointer still resolves to this exact HWND. Restore
                // only that subscription. Mutating hover state, geometry or
                // presentation here would let the region update post another
                // leave and recreate the input-starving feedback loop.
                TRACKMOUSEEVENT tracking{ sizeof(tracking) };
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = hwnd;
                TrackMouseEvent(&tracking);
                return 0;
            }
        }
        if (rightButtonDownDockHost_ == &host)
            rightButtonDownDockHost_ = nullptr;
        OnMouseLeave();
        return 0;
    }
    case WM_LBUTTONDOWN:
        // Persistent Dock Hosts deliberately use WS_EX_NOACTIVATE, so a
        // pointer press on their empty surface does not deactivate the
        // foreground modern menu. Dismiss the current menu explicitly before
        // routing the same press through the normal Dock interaction path.
        DismissActiveContextMenuForPopupTransition();
        SelectPersistentDockHost(&host);
        handlingFloatingDockInput_ = true;
        handlingPersistentDockHost_ = &host;
        OnLeftButtonDown(wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        handlingPersistentDockHost_ = nullptr;
        InvalidateFloatingDockWindow(host, true);
        return 0;
    case WM_LBUTTONUP:
        SelectPersistentDockHost(&host);
        handlingFloatingDockInput_ = true;
        handlingPersistentDockHost_ = &host;
        OnLeftButtonUpAt(
            wp, desktopPoint());
        handlingFloatingDockInput_ = false;
        handlingPersistentDockHost_ = nullptr;
        UpdateFloatingDockWindowBounds(host);
        InvalidateFloatingDockWindow(host, true);
        return 0;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (msg == WM_CANCELMODE ||
            !IsOwnedPointerCaptureWindow(
                reinterpret_cast<HWND>(lp)))
        {
            if (rightButtonDownDockHost_ == &host)
                rightButtonDownDockHost_ = nullptr;
            if (CanCancelPointerPressAfterCaptureLoss())
            {
                CancelPointerPressWithoutCaptureRelease();
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
    {
        SelectPersistentDockHost(&host);
        handlingFloatingDockInput_ = true;
        handlingPersistentDockHost_ = &host;
        const LRESULT result = HandleMessage(
            hwnd_, msg, wp, desktopLParam());
        handlingFloatingDockInput_ = false;
        handlingPersistentDockHost_ = nullptr;
        InvalidateFloatingDockWindow(host, true);
        return result;
    }
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        SelectPersistentDockHost(&host);
        OnMiddleButtonDown(wp, desktopLParam());
        return 0;
    case WM_MBUTTONUP:
        SelectPersistentDockHost(&host);
        OnMiddleButtonUpAt(wp, desktopPoint());
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        SelectPersistentDockHost(&host);
        OnRightButtonDown(&host);
        return 0;
    case WM_RBUTTONUP:
        SelectPersistentDockHost(&host);
        handlingPersistentDockHost_ = &host;
        OnRightButtonUp(desktopLParam());
        handlingPersistentDockHost_ = nullptr;
        InvalidateFloatingDockWindow(host, true);
        return 0;
    case WM_MOUSEWHEEL:
        SelectPersistentDockHost(&host);
        handlingFloatingDockInput_ = true;
        handlingPersistentDockHost_ = &host;
        OnMouseWheel(wp, lp);
        handlingFloatingDockInput_ = false;
        handlingPersistentDockHost_ = nullptr;
        UpdateFloatingDockWindowBounds(host);
        InvalidateFloatingDockWindow(host, true);
        return 0;
    case WM_DPICHANGED:
        // Dock geometry is also already expressed in physical desktop pixels.
        // A cross-monitor handoff changes the reusable HWND's DPI but must not
        // turn that ordinary move into a Dock close.
        InvalidateFloatingDockWindow(host, false);
        return 0;
    case WM_DISPLAYCHANGE:
        CloseAllFloatingDocks();
        return 0;
    case WM_CLOSE:
        CloseFloatingDock(host);
        return 0;
    case WM_DESTROY:
        if (rightButtonDownDockHost_ == &host)
            rightButtonDownDockHost_ = nullptr;
        host.hwnd = nullptr;
        if (floatingDockHost_ == &host)
            SelectPersistentDockHost(nullptr);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
