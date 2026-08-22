#include "app.h"
#include "../drag_visual_rules.h"

// Compact top-level DComp surface used only for the custom drag ghost. The
// desktop, floating Dock and popup surfaces keep rendering drop guidance, but
// no longer submit a second copy of the dragged item on every pointer move.

bool DesktopApp::CreateDragPreviewWindow()
{
    if (dragPreviewHwnd_ && IsWindow(dragPreviewHwnd_))
        return true;

    dragPreviewHwnd_ = CreateWindowExW(
        snowdesktop::drag_visual_rules::kPreviewWindowExStyle,
        kDragPreviewWindowClassName,
        L"SnowDesktop drag preview",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!dragPreviewHwnd_)
        return false;

    const BOOL disableTransitions = TRUE;
    DwmSetWindowAttribute(
        dragPreviewHwnd_, DWMWA_TRANSITIONS_FORCEDISABLED,
        &disableTransitions, sizeof(disableTransitions));
    return true;
}

void DesktopApp::ResetDragPreviewCompositionResources()
{
    if (dragPreviewDcompVisual_)
        dragPreviewDcompVisual_->SetContent(nullptr);
    dragPreviewDcompSurface_.Reset();
    dragPreviewCompWidth_ = 0;
    dragPreviewCompHeight_ = 0;
    dragPreviewRenderRevision_ = 0;
    dragPreviewContentBounds_ = {};
}

void DesktopApp::HideDragPreviewWindow()
{
    if (dragPreviewHwnd_ &&
        IsWindow(dragPreviewHwnd_) &&
        IsWindowVisible(dragPreviewHwnd_))
    {
        ShowWindow(dragPreviewHwnd_, SW_HIDE);
    }
}

void DesktopApp::DestroyDragPreviewWindow()
{
    HideDragPreviewWindow();
    ResetDragPreviewCompositionResources();
    if (dragPreviewDcompTarget_)
        dragPreviewDcompTarget_->SetRoot(nullptr);
    dragPreviewDcompVisual_.Reset();
    dragPreviewDcompTarget_.Reset();
    if (dragPreviewHwnd_ && IsWindow(dragPreviewHwnd_))
        DestroyWindow(dragPreviewHwnd_);
    dragPreviewHwnd_ = nullptr;
}

HRESULT DesktopApp::CreateOrResizeDragPreviewCompositionSurface(
    UINT width, UINT height)
{
    if (!dcompDevice_ || !dragPreviewHwnd_ ||
        !IsWindow(dragPreviewHwnd_))
        return E_UNEXPECTED;

    if (!dragPreviewDcompTarget_)
    {
        HRESULT hr = dcompDevice_->CreateTargetForHwnd(
            dragPreviewHwnd_, FALSE,
            &dragPreviewDcompTarget_);
        if (FAILED(hr))
            return hr;
    }
    if (!dragPreviewDcompVisual_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &dragPreviewDcompVisual_);
        if (FAILED(hr) || !dragPreviewDcompVisual_)
            return FAILED(hr) ? hr : E_FAIL;
        hr = dragPreviewDcompTarget_->SetRoot(
            dragPreviewDcompVisual_.Get());
        if (FAILED(hr))
            return hr;
    }
    if (dragPreviewDcompSurface_ &&
        dragPreviewCompWidth_ == width &&
        dragPreviewCompHeight_ == height)
        return S_OK;

    ComPtr<IDCompositionSurface> surface;
    HRESULT hr = dcompDevice_->CreateSurface(
        width, height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &surface);
    if (FAILED(hr) || !surface)
        return FAILED(hr) ? hr : E_FAIL;
    hr = dragPreviewDcompVisual_->SetContent(surface.Get());
    if (FAILED(hr))
        return hr;

    dragPreviewDcompSurface_ = std::move(surface);
    dragPreviewCompWidth_ = width;
    dragPreviewCompHeight_ = height;
    dragPreviewRenderRevision_ = 0;
    return S_OK;
}

bool DesktopApp::RenderDragPreviewCompositionFrame(
    const RECT& desktopBounds)
{
    if (dragPreviewCompositionPaintInProgress_)
        return false;
    dragPreviewCompositionPaintInProgress_ = true;
    struct PaintScope final
    {
        bool& active;
        ~PaintScope() { active = false; }
    } paintScope{ dragPreviewCompositionPaintInProgress_ };

    const UINT width = static_cast<UINT>(std::max<LONG>(
        1, desktopBounds.right - desktopBounds.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1, desktopBounds.bottom - desktopBounds.top));
    HRESULT hr = CreateOrResizeDragPreviewCompositionSurface(
        width, height);
    if (FAILED(hr) || !dragPreviewDcompSurface_)
    {
        ResetDragPreviewCompositionResources();
        return false;
    }

    ID2D1DeviceContext* rawContext = nullptr;
    POINT updateOffset{};
    hr = dragPreviewDcompSurface_->BeginDraw(
        nullptr, __uuidof(ID2D1DeviceContext),
        reinterpret_cast<void**>(&rawContext),
        &updateOffset);
    if (FAILED(hr) || !rawContext)
    {
        ResetDragPreviewCompositionResources();
        return false;
    }

    ComPtr<ID2D1DeviceContext> context;
    context.Attach(rawContext);
    context->SetDpi(96.0f, 96.0f);
    context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(updateOffset.x - desktopBounds.left),
        static_cast<float>(updateOffset.y - desktopBounds.top)));
    context->SetAntialiasMode(
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->Clear(D2D1::ColorF(0, 0, 0, 0));

    brushCache_.clear();
    brushCacheContext_ = context.Get();
    const POINT current = dragSession_.CurrentPoint();
    const auto& dragItems = dragSession_.Items();
    for (std::size_t itemIndex = 0;
         itemIndex < dragItems.size(); ++itemIndex)
    {
        Item* item = dragItems[itemIndex];
        if (!item)
            continue;
        const RECT itemBounds = item->GetBounds();
        if (IsRectEmptyRect(itemBounds))
            continue;
        const RECT draggedBounds =
            dragSession_.ResolveDraggedBounds(
                itemIndex, itemBounds, current);
        item->Draw(context.Get(), draggedBounds, 3);
    }
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context.Reset();
    brushCache_.clear();
    brushCacheContext_ = nullptr;

    hr = dragPreviewDcompSurface_->EndDraw();
    if (FAILED(hr))
    {
        ResetDragPreviewCompositionResources();
        return false;
    }
    if (!CommitCompositionAnimationFrame())
    {
        ResetDragPreviewCompositionResources();
        return false;
    }
    dragPreviewContentBounds_ = desktopBounds;
    dragPreviewRenderRevision_ =
        dragSession_.StaticSceneRevision();
    return true;
}

void DesktopApp::SyncDragPreviewWindow()
{
    const auto& dragItems = dragSession_.Items();
    if (!snowdesktop::drag_visual_rules::ShouldShowPreview(
            dragSession_.IsActive(),
            dragSession_.IsVisualVisible(),
            !dragItems.empty()))
    {
        HideDragPreviewWindow();
        return;
    }

    RECT contentBounds{};
    bool hasContent = false;
    const POINT current = dragSession_.CurrentPoint();
    for (std::size_t itemIndex = 0;
         itemIndex < dragItems.size(); ++itemIndex)
    {
        Item* item = dragItems[itemIndex];
        if (!item)
            continue;
        const RECT itemBounds = item->GetBounds();
        if (IsRectEmptyRect(itemBounds))
            continue;
        const RECT draggedBounds =
            dragSession_.ResolveDraggedBounds(
                itemIndex, itemBounds, current);
        if (!hasContent)
        {
            contentBounds = draggedBounds;
            hasContent = true;
        }
        else
        {
            contentBounds.left = std::min(
                contentBounds.left, draggedBounds.left);
            contentBounds.top = std::min(
                contentBounds.top, draggedBounds.top);
            contentBounds.right = std::max(
                contentBounds.right, draggedBounds.right);
            contentBounds.bottom = std::max(
                contentBounds.bottom, draggedBounds.bottom);
        }
    }
    if (!hasContent)
    {
        HideDragPreviewWindow();
        return;
    }

    // Preserve icon shadows and Dock bounce overdraw without allocating a
    // desktop-sized transparent target.
    constexpr LONG kOverdraw = 8;
    InflateRect(&contentBounds, kOverdraw, kOverdraw);
    if (!CreateDragPreviewWindow())
        return;

    const UINT width = static_cast<UINT>(std::max<LONG>(
        1, contentBounds.right - contentBounds.left));
    const UINT height = static_cast<UINT>(std::max<LONG>(
        1, contentBounds.bottom - contentBounds.top));
    POINT screenOrigin{ contentBounds.left, contentBounds.top };
    if (!hwnd_ || !IsWindow(hwnd_) ||
        !ClientToScreen(hwnd_, &screenOrigin))
    {
        HideDragPreviewWindow();
        return;
    }

    const bool geometryChanged =
        width != dragPreviewCompWidth_ ||
        height != dragPreviewCompHeight_;
    SetWindowPos(
        dragPreviewHwnd_, HWND_TOPMOST,
        screenOrigin.x, screenOrigin.y,
        static_cast<int>(width),
        static_cast<int>(height),
        SWP_NOACTIVATE |
            (geometryChanged ? 0 : SWP_NOSIZE));

    const bool needsRender =
        !dragPreviewDcompSurface_ ||
        geometryChanged ||
        dragPreviewRenderRevision_ !=
            dragSession_.StaticSceneRevision();
    if (needsRender &&
        !RenderDragPreviewCompositionFrame(contentBounds))
    {
        HideDragPreviewWindow();
        return;
    }
    if (!IsWindowVisible(dragPreviewHwnd_))
        ShowWindow(dragPreviewHwnd_, SW_SHOWNOACTIVATE);
}

HWND DesktopApp::ResolveWindowBelowDragPreviewAt(
    POINT screenPoint) const
{
    HWND hit = WindowFromPoint(screenPoint);
    if (!dragPreviewHwnd_ || !IsWindow(dragPreviewHwnd_) ||
        (hit != dragPreviewHwnd_ &&
         GetAncestor(hit, GA_ROOT) != dragPreviewHwnd_))
        return hit;

    for (HWND candidate = GetWindow(
             dragPreviewHwnd_, GW_HWNDNEXT);
         candidate;
         candidate = GetWindow(candidate, GW_HWNDNEXT))
    {
        if (!IsWindowVisible(candidate))
            continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(
                candidate, DWMWA_CLOAKED,
                &cloaked, sizeof(cloaked))) &&
            cloaked != 0)
            continue;
        RECT windowRect{};
        if (!GetWindowRect(candidate, &windowRect) ||
            !PtInRect(&windowRect, screenPoint))
            continue;

        HRGN region = CreateRectRgn(0, 0, 0, 0);
        if (region)
        {
            const int regionType = GetWindowRgn(candidate, region);
            const POINT localPoint{
                screenPoint.x - windowRect.left,
                screenPoint.y - windowRect.top };
            const bool outsideRegion =
                regionType != ERROR &&
                regionType != NULLREGION &&
                !PtInRegion(region,
                    localPoint.x, localPoint.y);
            DeleteObject(region);
            if (outsideRegion)
                continue;
        }
        return candidate;
    }
    return nullptr;
}

LRESULT DesktopApp::HandleDragPreviewMessage(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
