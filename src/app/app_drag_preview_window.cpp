#include "app.h"
#include "../drag_visual_rules.h"
#include "popup_window_pair_z_order.h"

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
    dragPreviewWindowBoundsValid_ = false;

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
    dragPreviewItemIndices_.clear();
    dragPreviewItemBounds_.clear();
    dragPreviewRenderRevision_ = 0;
    dragPreviewContentBounds_ = {};
    dragPreviewWindowBoundsValid_ = false;
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

void DesktopApp::ApplyDragPreviewLayerPolicy()
{
    namespace zOrder = snowdesktop::popup_window_pair_z_order;
    const HWND visiblePreview = dragPreviewHwnd_ && IsWindowVisible(dragPreviewHwnd_)
        ? dragPreviewHwnd_ : nullptr;
    const HWND visibleHint = hintHwnd_ && IsWindowVisible(hintHwnd_) ? hintHwnd_ : nullptr;
    const HWND visiblePopup = floatingPopupHwnd_ && IsWindowVisible(floatingPopupHwnd_)
        ? floatingPopupHwnd_ : nullptr;
    if (!visiblePreview && !visibleHint) return;
    const bool previewOrdered = !visiblePreview ||
        (zOrder::IsTopmost(visiblePreview) &&
            (!visiblePopup || zOrder::IsAbove(visiblePreview, visiblePopup)));
    const bool hintOrdered = !visibleHint ||
        (zOrder::IsTopmost(visibleHint) &&
            (!visiblePreview || zOrder::IsAbove(visibleHint, visiblePreview)) &&
            (!visiblePopup || zOrder::IsAbove(visibleHint, visiblePopup)));
    if (previewOrdered && hintOrdered) return;
    const auto policy = snowdesktop::drag_visual_rules::ResolvePreviewWindowZOrderPolicy(true, visibleHint);
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;
    // Raise the pair together above a newly opened Dock popup. A fixed order
    // keeps the DComp ghost from alternating over the layered hint on refresh.
    HDWP batch = BeginDeferWindowPos((visibleHint ? 1 : 0) + (visiblePreview ? 1 : 0));
    if (batch && visibleHint) batch = DeferWindowPos(batch, visibleHint, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (batch && visiblePreview) batch = DeferWindowPos(batch, visiblePreview, policy.insertAfter, 0, 0, 0, 0, flags);
    if (batch) EndDeferWindowPos(batch);
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
    dragPreviewWindowBounds_ = {};
    dragPreviewWindowBoundsValid_ = false;
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
    const auto& dragItems = dragSession_.Items();
    const std::size_t previewItemCount = std::min(
        dragPreviewItemIndices_.size(),
        dragPreviewItemBounds_.size());
    for (std::size_t previewIndex = 0;
         previewIndex < previewItemCount; ++previewIndex)
    {
        const std::size_t itemIndex =
            dragPreviewItemIndices_[previewIndex];
        if (itemIndex >= dragItems.size())
            continue;
        Item* item = dragItems[itemIndex];
        if (!item)
            continue;
        const RECT destination = dragPreviewItemBounds_[previewIndex];
        auto* icon = dynamic_cast<DesktopIcon*>(item);
        const auto* data = icon ? icon->GetDesktopItem() : nullptr;
        if (dragFanIconsOnly_)
        {
            if (icon)
            {
                const auto* source = dynamic_cast<const WidgetContainer*>(item->GetContainer());
                icon->Draw(context.Get(), destination, 3, false, false, false,
                    source ? source->GetWidgetData() : nullptr, false, true,
                    static_cast<int>(destination.right - destination.left));
            }
            else
            {
                // FolderEntryIcon normally lays out a grid cell. Map only its
                // icon into the retained square; do not reuse the fan row AABB.
                const RECT natural = GetItemIconRect(destination);
                D2D1_MATRIX_3X2_F previous{};
                context->GetTransform(&previous);
                context->SetTransform(D2D1::Matrix3x2F::Translation(
                    -static_cast<float>(natural.left), -static_cast<float>(natural.top)) *
                    D2D1::Matrix3x2F::Scale(
                        static_cast<float>(destination.right - destination.left) / std::max(1L, natural.right - natural.left),
                        static_cast<float>(destination.bottom - destination.top) / std::max(1L, natural.bottom - natural.top)) *
                    D2D1::Matrix3x2F::Translation(
                        static_cast<float>(destination.left), static_cast<float>(destination.top)) * previous);
                item->Draw(context.Get(), destination, 3);
                context->SetTransform(previous);
            }
        }
        else if (data && data->largeIcon)
        {
            const RECT source = dragSession_.ResolveDraggedBounds(itemIndex, item->GetBounds(), dragSession_.CurrentPoint());
            DesktopWidget geometry; geometry.bounds = source; geometry.gridCell = data->gridCell;
            const RECT frame = GetStandaloneWidgetFrameRect(geometry);
            const float sx = static_cast<float>(destination.right - destination.left) / std::max(1L, frame.right - frame.left);
            const float sy = static_cast<float>(destination.bottom - destination.top) / std::max(1L, frame.bottom - frame.top);
            context->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(frame.left), -static_cast<float>(frame.top)) *
                D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(
                    static_cast<float>(destination.left + updateOffset.x - desktopBounds.left),
                    static_cast<float>(destination.top + updateOffset.y - desktopBounds.top)));
            item->Draw(context.Get(), source, 3);
            context->SetTransform(D2D1::Matrix3x2F::Translation(
                static_cast<float>(updateOffset.x - desktopBounds.left),
                static_cast<float>(updateOffset.y - desktopBounds.top)));
        }
        else item->Draw(context.Get(), destination, 3);
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

    const POINT current = dragSession_.CurrentPoint();
    std::size_t validItemCount = 0;
    std::size_t firstValidIndex = dragItems.size();
    RECT firstValidBounds{};
    std::size_t primaryItemIndex = dragItems.size();
    RECT primaryBounds{};
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
        ++validItemCount;
        if (firstValidIndex == dragItems.size())
        {
            firstValidIndex = itemIndex;
            firstValidBounds = draggedBounds;
        }
        if (primaryItemIndex == dragItems.size() &&
            PtInRect(&draggedBounds, current))
        {
            primaryItemIndex = itemIndex;
            primaryBounds = draggedBounds;
        }
    }
    if (validItemCount == 0)
    {
        HideDragPreviewWindow();
        return;
    }
    if (primaryItemIndex == dragItems.size())
    {
        primaryItemIndex = firstValidIndex;
        primaryBounds = firstValidBounds;
    }

    const auto compactLargeIcon = [&](std::size_t index, RECT bounds) {
        auto* icon = dynamic_cast<DesktopIcon*>(dragItems[index]);
        const auto* data = icon ? icon->GetDesktopItem() : nullptr;
        if (dragFanIconsOnly_ || !data || !data->largeIcon) return bounds;
        DesktopWidget geometry; geometry.gridCell = data->gridCell; geometry.bounds = bounds;
        const RECT sourceFrame = GetStandaloneWidgetFrameRect(geometry);
        geometry.bounds = GetGridRect(gridPages_, data->gridCell, {1, 1});
        const RECT cellFrame = GetStandaloneWidgetFrameRect(geometry);
        return snowdesktop::drag_visual_rules::FitPreviewInCell(sourceFrame,
            {cellFrame.right - cellFrame.left, cellFrame.bottom - cellFrame.top}, current);
    };
    firstValidBounds = compactLargeIcon(firstValidIndex, firstValidBounds);
    primaryBounds = compactLargeIcon(primaryItemIndex, primaryBounds);

    dragPreviewItemIndices_.clear();
    dragPreviewItemBounds_.clear();
    dragPreviewItemIndices_.reserve(
        snowdesktop::drag_visual_rules::
            kMaximumStackedPreviewItems);
    dragPreviewItemBounds_.reserve(
        snowdesktop::drag_visual_rules::
            kMaximumStackedPreviewItems);
    if (!snowdesktop::drag_visual_rules::ShouldCompactPreview(
            validItemCount))
    {
        dragPreviewItemIndices_.push_back(firstValidIndex);
        dragPreviewItemBounds_.push_back(firstValidBounds);
    }
    else
    {
        constexpr std::size_t kMaximumSecondaryItems =
            snowdesktop::drag_visual_rules::
                kMaximumStackedPreviewItems - 1;
        std::array<std::size_t,
            kMaximumSecondaryItems> secondaryIndices{};
        std::array<RECT,
            kMaximumSecondaryItems> secondaryBounds{};
        std::size_t secondaryCount = 0;
        for (std::size_t itemIndex = 0;
             itemIndex < dragItems.size() &&
             secondaryCount < kMaximumSecondaryItems;
             ++itemIndex)
        {
            Item* item = dragItems[itemIndex];
            if (!item || itemIndex == primaryItemIndex)
                continue;
            const RECT itemBounds = item->GetBounds();
            if (IsRectEmptyRect(itemBounds))
                continue;
            secondaryIndices[secondaryCount] = itemIndex;
            secondaryBounds[secondaryCount] =
                compactLargeIcon(itemIndex, dragSession_.ResolveDraggedBounds(
                    itemIndex, itemBounds, current));
            ++secondaryCount;
        }

        for (std::size_t stackIndex = 0;
             stackIndex < secondaryCount;
             ++stackIndex)
        {
            const RECT sourceBounds =
                secondaryBounds[stackIndex];
            const LONG depth = static_cast<LONG>(
                secondaryCount - stackIndex);
            const LONG left = primaryBounds.left -
                depth * snowdesktop::drag_visual_rules::
                    kStackedPreviewOffset;
            const LONG top = primaryBounds.top -
                depth * snowdesktop::drag_visual_rules::
                    kStackedPreviewOffset;
            dragPreviewItemIndices_.push_back(
                secondaryIndices[stackIndex]);
            dragPreviewItemBounds_.push_back({
                left,
                top,
                left + sourceBounds.right - sourceBounds.left,
                top + sourceBounds.bottom - sourceBounds.top });
        }
        dragPreviewItemIndices_.push_back(primaryItemIndex);
        dragPreviewItemBounds_.push_back(primaryBounds);
    }

    RECT contentBounds = dragPreviewItemBounds_.front();
    for (std::size_t index = 1;
         index < dragPreviewItemBounds_.size(); ++index)
    {
        const RECT& itemBounds =
            dragPreviewItemBounds_[index];
        contentBounds.left = std::min(
            contentBounds.left, itemBounds.left);
        contentBounds.top = std::min(
            contentBounds.top, itemBounds.top);
        contentBounds.right = std::max(
            contentBounds.right, itemBounds.right);
        contentBounds.bottom = std::max(
            contentBounds.bottom, itemBounds.bottom);
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
    const RECT requestedWindowBounds{
        screenOrigin.x,
        screenOrigin.y,
        screenOrigin.x + static_cast<LONG>(width),
        screenOrigin.y + static_cast<LONG>(height),
    };
    const bool previewWasVisible =
        IsWindowVisible(dragPreviewHwnd_) != FALSE;
    if (snowdesktop::drag_visual_rules::
            ShouldApplyPreviewWindowPlacement(
                previewWasVisible,
                dragPreviewWindowBoundsValid_,
                dragPreviewWindowBounds_,
                requestedWindowBounds))
    {
        const auto zOrder = snowdesktop::drag_visual_rules::
            ResolvePreviewWindowZOrderPolicy(
                previewWasVisible, hintHwnd_ && IsWindowVisible(hintHwnd_) ? hintHwnd_ : nullptr);
        const bool placementSizeChanged =
            !dragPreviewWindowBoundsValid_ ||
            dragPreviewWindowBounds_.right -
                    dragPreviewWindowBounds_.left !=
                static_cast<LONG>(width) ||
            dragPreviewWindowBounds_.bottom -
                    dragPreviewWindowBounds_.top !=
                static_cast<LONG>(height);
        const UINT placementFlags = zOrder.flags |
            (placementSizeChanged ? 0 : SWP_NOSIZE);
        if (!SetWindowPos(
                dragPreviewHwnd_, zOrder.insertAfter,
                screenOrigin.x, screenOrigin.y,
                static_cast<int>(width),
                static_cast<int>(height),
                placementFlags))
        {
            dragPreviewWindowBoundsValid_ = false;
            HideDragPreviewWindow();
            return;
        }
        dragPreviewWindowBounds_ = requestedWindowBounds;
        dragPreviewWindowBoundsValid_ = true;
    }
    else
    {
        // A Dock popup can enter the topmost band while the pointer is held
        // still for dwell activation. Reassert the ghost even without a
        // geometry update, while keeping the hint above the ghost.
        ApplyDragPreviewLayerPolicy();
    }

    // Native input stays captured and external-window classification already
    // resolves below this presentation-only HWND. While OLE owns routing the
    // custom preview is hidden before DoDragDrop starts and remains hidden
    // until that nested loop has fully unwound. A moving one-pixel window
    // region is therefore unnecessary; avoiding SetWindowRgn here also avoids
    // the synchronous WM_WINDOWPOSCHANGING/WM_WINDOWPOSCHANGED pair it emits
    // for every pointer pixel.

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
    if (needsRender)
    {
        // Submit the compact surface before any desktop/Dock feedback paint.
        // The outer pointer message may still have larger work to do, but the
        // compositor can start presenting the ghost immediately.
        FlushPendingCompositionCommit();
    }
    if (!previewWasVisible)
    {
        const auto policy = snowdesktop::drag_visual_rules::ResolvePreviewWindowZOrderPolicy(false,
            hintHwnd_ && IsWindowVisible(hintHwnd_) ? hintHwnd_ : nullptr);
        SetWindowPos(dragPreviewHwnd_, policy.insertAfter, 0, 0, 0, 0,
            policy.flags | SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
}

bool DesktopApp::IsDragPresentationOnlyWindow(HWND window) const
{
    if (!window) return false;
    if (window == dragPreviewHwnd_ ||
        window == hintHwnd_ ||
        desktopBackdropCompositor_.IsBackdropWindow(window) ||
        IsPersistentDockBackdropWindow(window) ||
        collectionPopupBackdropCompositor_.IsBackdropWindow(window) ||
        quickNavBackdropCompositor_.IsBackdropWindow(window) ||
        (dockWindowTransition_ &&
            dockWindowTransition_->IsPresentationWindow(window)))
        return true;

    const DWORD extendedStyle = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_EXSTYLE));
    return snowdesktop::drag_visual_rules::
        IsLayeredTransparentPresentationWindow(extendedStyle);
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
        if (!IsWindowVisible(candidate) ||
            !IsWindowEnabled(candidate))
            continue;
        RECT windowRect{};
        if (!GetWindowRect(candidate, &windowRect) ||
            !PtInRect(&windowRect, screenPoint))
            continue;

        const bool presentationOnly =
            IsDragPresentationOnlyWindow(candidate);
        DWORD cloaked = 0;
        const bool isCloaked =
            SUCCEEDED(DwmGetWindowAttribute(
                candidate, DWMWA_CLOAKED,
                &cloaked, sizeof(cloaked))) &&
            cloaked != 0;
        if (snowdesktop::drag_visual_rules::
                ShouldSkipPreviewFallbackCandidate(
                    true, true, isCloaked,
                    presentationOnly))
            continue;

        const POINT localPoint{
            screenPoint.x - windowRect.left,
            screenPoint.y - windowRect.top };
        RECT regionBounds{};
        const int regionType =
            GetWindowRgnBox(candidate, &regionBounds);
        if (!snowdesktop::drag_visual_rules::
                PreviewFallbackRegionContainsPoint(
                    regionType,
                    PtInRect(&regionBounds, localPoint) != FALSE))
            continue;
        if (snowdesktop::drag_visual_rules::
                PreviewFallbackNeedsExactRegionCheck(regionType))
        {
            HRGN region = CreateRectRgn(0, 0, 0, 0);
            if (!region)
                return nullptr;
            const int exactRegionType =
                GetWindowRgn(candidate, region);
            const bool pointInRegion =
                PtInRegion(region,
                    localPoint.x, localPoint.y) != FALSE;
            DeleteObject(region);
            if (!snowdesktop::drag_visual_rules::
                    PreviewFallbackRegionContainsPoint(
                        exactRegionType, pointInRegion))
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
