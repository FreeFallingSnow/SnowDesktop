#include "app.h"

namespace
{
constexpr UINT kMaximumWidgetSurfaceDimension =
    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

bool SameRect(const RECT& left, const RECT& right)
{
    return EqualRect(&left, &right) != FALSE;
}

struct DesktopWidgetSurfaceFailure
{
    std::wstring widgetId;
    const wchar_t* stage = L"Render";
    HRESULT hr = E_FAIL;
    bool retry = true;
};
}

// Every desktop widget owns a compact DirectComposition child surface. The
// root surface contains desktop icons only; a separate foreground surface
// covers widgets with Dock, popup, drag, marquee, and navigation content.

bool DesktopApp::QueueDesktopWidgetComposition(
    const std::wstring& widgetId)
{
    const auto fail = [&]() {
        desktopWidgetCompositionFailurePending_ = true;
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    };
    if (widgetId.empty() || !dcompDevice_ || !dcompVisual_ || !hwnd_)
        return fail();

    DesktopWidget* widgetData = nullptr;
    for (auto& widget : widgets_)
    {
        if (widget.id == widgetId)
        {
            widgetData = &widget;
            break;
        }
    }
    if (!widgetData)
        return fail();

    const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
    if (IsRectEmptyRect(bounds))
        return true;
    if (!customDesktopVisible_ ||
        (desktopIconsHidden_ && !widgetData->keepWhenDesktopHidden))
    {
        SetDesktopWidgetCompositionVisible(widgetId, false, bounds);
        return true;
    }

    const auto existing =
        desktopWidgetCompositionItems_.find(widgetId);
    if (existing != desktopWidgetCompositionItems_.end() &&
        !existing->second.visible)
    {
        return true;
    }

    if (!desktopWidgetCompositionLayer_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &desktopWidgetCompositionLayer_);
        if (FAILED(hr) || !desktopWidgetCompositionLayer_)
            return fail();
        hr = dcompVisual_->AddVisual(
            desktopWidgetCompositionLayer_.Get(), TRUE, nullptr);
        if (SUCCEEDED(hr))
            hr = SyncDesktopCompositionRootZOrder();
        if (FAILED(hr))
        {
            (void)dcompVisual_->RemoveVisual(
                desktopWidgetCompositionLayer_.Get());
            desktopWidgetCompositionLayer_.Reset();
            return fail();
        }
    }

    auto [position, inserted] =
        desktopWidgetCompositionItems_.try_emplace(widgetId);
    auto& item = position->second;
    if (inserted)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&item.visual);
        if (SUCCEEDED(hr))
            hr = dcompDevice_->CreateRectangleClip(&item.clip);
        if (SUCCEEDED(hr)) hr = item.clip->SetLeft(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetTop(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetRight(0.0f);
        if (SUCCEEDED(hr)) hr = item.clip->SetBottom(0.0f);
        if (SUCCEEDED(hr)) hr = item.visual->SetClip(item.clip.Get());
        if (FAILED(hr) || !item.visual || !item.clip)
        {
            desktopWidgetCompositionItems_.erase(position);
            return fail();
        }
        hr = desktopWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            desktopWidgetCompositionItems_.erase(position);
            return fail();
        }
        item.bounds = bounds;
        item.visible = true;
    }

    pendingDesktopWidgetCompositions_.insert(widgetId);
    if (snowdesktop::widget_composition_layer_rules::
            ShouldDeferWidgetSurfaceDraw(
                compositionPaintInProgress_,
                floatingDockCompositionPaintInProgress_,
                floatingPopupCompositionPaintInProgress_))
        return true;
    if (!FlushPendingDesktopWidgetComposition())
        return fail();
    if (!FlushPendingWidgetMarqueeComposition() ||
        !SyncWidgetMarqueeCompositionVisibility())
        return fail();
    if (!CommitCompositionAnimationFrame())
        return fail();
    return FlushPendingCompositionCommit();
}

bool DesktopApp::FlushPendingDesktopWidgetComposition()
{
    if (pendingDesktopWidgetCompositions_.empty())
    {
        if (SyncDesktopWidgetCompositionZOrder())
            return true;
        desktopWidgetCompositionFailurePending_ = true;
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (!dcompDevice_ || !desktopWidgetCompositionLayer_)
    {
        desktopWidgetCompositionFailurePending_ = true;
        return false;
    }

    auto pending = std::move(pendingDesktopWidgetCompositions_);
    pendingDesktopWidgetCompositions_.clear();
    const bool ownsPaintScope = !compositionPaintInProgress_;
    if (ownsPaintScope)
    {
        compositionPaintInProgress_ = true;
        desktopBackdropCompositor_.BeginFrame(false);
    }

    bool structuralOk = true;
    std::vector<DesktopWidgetSurfaceFailure> surfaceFailures;
    for (const auto& widgetId : pending)
    {
        auto composition = desktopWidgetCompositionItems_.find(widgetId);
        if (composition == desktopWidgetCompositionItems_.end())
            continue;
        if (!composition->second.visible)
            continue;

        DesktopWidget* widgetData = nullptr;
        WidgetContainer* widgetContainer = nullptr;
        Widget* widgetItem = nullptr;
        for (auto& widget : widgets_)
        {
            if (widget.id == widgetId)
            {
                widgetData = &widget;
                break;
            }
        }
        if (widgetData)
        {
            for (auto& container : containers_)
            {
                auto* candidate =
                    dynamic_cast<WidgetContainer*>(container.get());
                if (candidate && candidate->GetWidgetData() == widgetData)
                {
                    widgetContainer = candidate;
                    break;
                }
            }
            if (!widgetContainer)
            {
                for (auto& ownedItem : items_oo_)
                {
                    auto* candidate =
                        dynamic_cast<Widget*>(ownedItem.get());
                    if (candidate && candidate->GetWidgetData() == widgetData)
                    {
                        widgetItem = candidate;
                        break;
                    }
                }
            }
        }
        if (!widgetData || (!widgetContainer && !widgetItem))
        {
            surfaceFailures.push_back({
                widgetId, L"Resolve owner", E_UNEXPECTED, false });
            continue;
        }

        const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
        const LONG rawWidth = bounds.right - bounds.left;
        const LONG rawHeight = bounds.bottom - bounds.top;
        constexpr LONG surfaceOverdraw = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                kWidgetSurfaceBorderOverdraw);
        constexpr LONG maximumLogicalSurfaceDimension =
            static_cast<LONG>(kMaximumWidgetSurfaceDimension) -
            surfaceOverdraw * 2;
        if (rawWidth <= 0 || rawHeight <= 0 ||
            rawWidth > maximumLogicalSurfaceDimension ||
            rawHeight > maximumLogicalSurfaceDimension)
        {
            surfaceFailures.push_back({
                widgetId, L"Validate bounds", E_INVALIDARG, false });
            continue;
        }
        const LONG surfaceLeft = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceOrigin(bounds.left));
        const LONG surfaceTop = static_cast<LONG>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceOrigin(bounds.top));
        const UINT width = static_cast<UINT>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceExtent(rawWidth));
        const UINT height = static_cast<UINT>(
            snowdesktop::widget_composition_layer_rules::
                WidgetSurfaceExtent(rawHeight));
        auto& item = composition->second;
        const RECT oldBounds = item.bounds;
        const bool boundsChanged = !SameRect(oldBounds, bounds);

        if (!item.surface || item.width != width || item.height != height)
        {
            ComPtr<IDCompositionSurface> surface;
            HRESULT hr = dcompDevice_->CreateSurface(
                width, height,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED,
                &surface);
            if (FAILED(hr) || !surface)
            {
                surfaceFailures.push_back({
                    widgetId, L"CreateSurface",
                    FAILED(hr) ? hr : E_FAIL, true });
                continue;
            }
            item.surface = std::move(surface);
            item.width = width;
            item.height = height;
        }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        HRESULT hr = item.surface->BeginDraw(
            nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext), &updateOffset);
        if (FAILED(hr) || !rawContext)
        {
            surfaceFailures.push_back({
                widgetId, L"BeginDraw",
                FAILED(hr) ? hr : E_FAIL, true });
            continue;
        }

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x - surfaceLeft),
            static_cast<float>(updateOffset.y - surfaceTop)));
        context->Clear(D2D1::ColorF(0, 0, 0, 0));

        desktopWidgetCompositionDrawInProgress_ = true;
        desktopWidgetBackdropRequestedDuringDraw_ = false;
        desktopWidgetBackdropCornerRadiusDuringDraw_ = 0.0f;
        desktopWidgetBackdropBlurRadiusDuringDraw_ = 0.0f;
        const POINT interactionPoint = lastMousePoint_;
        if (SuppressDesktopWidgetDragTargets() ||
            IsPointOccludedByOpenPopup(lastMousePoint_))
        {
            lastMousePoint_ = { LONG_MIN, LONG_MIN };
        }
        if (widgetContainer)
        {
            widgetContainer->DrawChrome(context.Get(), lastMousePoint_);
        }
        else if (auto* luaWidget = dynamic_cast<LuaScript*>(widgetItem))
        {
            luaWidget->DrawCompositionSurface(
                context.Get(), widgetData->bounds,
                widgetData->selected ? 2 : 0, false);
        }
        else
        {
            widgetItem->Draw(
                context.Get(), widgetData->bounds,
                widgetData->selected ? 2 : 0);
        }
        lastMousePoint_ = interactionPoint;
        desktopWidgetCompositionDrawInProgress_ = false;
        const bool backdropRequested =
            desktopWidgetBackdropRequestedDuringDraw_;
        const int backdropCornerRadius = std::max(
            0, static_cast<int>(std::lround(
                desktopWidgetBackdropCornerRadiusDuringDraw_)));
        const int backdropBlurRadius = std::clamp(
            static_cast<int>(std::lround(
                desktopWidgetBackdropBlurRadiusDuringDraw_)), 0, 48);
        desktopWidgetBackdropRequestedDuringDraw_ = false;

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        const HRESULT endDrawHr = item.surface->EndDraw();
        brushCache_.clear();
        brushCacheContext_ = nullptr;
        if (FAILED(endDrawHr))
        {
            surfaceFailures.push_back({
                widgetId, L"EndDraw", endDrawHr, true });
            continue;
        }

        if (boundsChanged && item.backdropRegistered)
        {
            (void)desktopBackdropCompositor_.RemovePanel(oldBounds);
            item.backdropRegistered = false;
        }
        if (backdropRequested)
        {
            const bool backdropChanged =
                !item.backdropRegistered || boundsChanged ||
                item.backdropCornerRadius != backdropCornerRadius ||
                item.backdropBlurRadius != backdropBlurRadius;
            if (backdropChanged)
            {
                item.backdropRegistered =
                    desktopBackdropCompositor_.AddPanel(
                        bounds,
                        static_cast<float>(backdropCornerRadius),
                        static_cast<float>(backdropBlurRadius));
            }
            item.backdropCornerRadius = backdropCornerRadius;
            item.backdropBlurRadius = backdropBlurRadius;
        }
        else if (item.backdropRegistered)
        {
            (void)desktopBackdropCompositor_.RemovePanel(bounds);
            item.backdropRegistered = false;
        }

        item.bounds = bounds;
        hr = item.visual->SetContent(item.surface.Get());
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetX(static_cast<float>(surfaceLeft));
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetY(static_cast<float>(surfaceTop));
        if (SUCCEEDED(hr))
            hr = item.clip->SetRight(
                item.visible ? static_cast<float>(width) : 0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetBottom(
                item.visible ? static_cast<float>(height) : 0.0f);
        if (FAILED(hr))
        {
            surfaceFailures.push_back({
                widgetId, L"Update visual", hr, true });
        }
    }

    if (!SyncDesktopWidgetCompositionZOrder())
    {
        desktopWidgetCompositionFailurePending_ = true;
        structuralOk = false;
    }

    // A child surface can fail independently (for example after a stale
    // BeginDraw). Recreate only that surface and preserve the desktop root,
    // foreground, sibling widgets and popup host. A device-wide or tree
    // failure still reaches the structural recovery path above/at Commit.
    for (const auto& failure : surfaceFailures)
    {
        wchar_t message[256]{};
        wsprintfW(message,
            L"Desktop widget %s FAILED hr=0x%08X; %s child surface",
            failure.stage,
            static_cast<unsigned>(failure.hr),
            failure.retry ? L"recreating" : L"dropping");
        WriteDiagnosticLogEntry(message);

        const auto position =
            desktopWidgetCompositionItems_.find(failure.widgetId);
        if (position == desktopWidgetCompositionItems_.end())
            continue;
        if (!failure.retry)
        {
            const RECT dirty = position->second.bounds;
            if (position->second.backdropRegistered)
            {
                (void)desktopBackdropCompositor_.RemovePanel(
                    position->second.bounds);
            }
            if (desktopWidgetCompositionLayer_ && position->second.visual)
            {
                (void)desktopWidgetCompositionLayer_->RemoveVisual(
                    position->second.visual.Get());
            }
            desktopWidgetCompositionItems_.erase(position);
            desktopWidgetCompositionZOrder_.clear();
            if (hwnd_ && IsWindow(hwnd_))
                InvalidateRect(hwnd_, &dirty, FALSE);
            continue;
        }

        auto& item = position->second;
        item.surface.Reset();
        item.width = 0;
        item.height = 0;
        pendingDesktopWidgetCompositions_.insert(failure.widgetId);
        if (hwnd_ && IsWindow(hwnd_))
        {
            RECT dirty = item.bounds;
            InflateRect(&dirty, 1, 1);
            InvalidateRect(hwnd_, &dirty, FALSE);
        }
    }
    if (ownsPaintScope)
    {
        KeepDesktopWidgetBackdropPanels();
        desktopBackdropCompositor_.EndFrame();
        compositionPaintInProgress_ = false;
    }
    if (!structuralOk && hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    return structuralOk;
}

bool DesktopApp::HasDesktopWidgetComposition(
    const std::wstring& widgetId) const
{
    return desktopWidgetCompositionItems_.contains(widgetId);
}

void DesktopApp::SetDesktopWidgetCompositionVisible(
    const std::wstring& widgetId,
    bool visible,
    const RECT& bounds)
{
    (void)bounds;
    const auto position = desktopWidgetCompositionItems_.find(widgetId);
    if (position == desktopWidgetCompositionItems_.end())
        return;
    auto& item = position->second;
    if (item.visible == visible)
        return;
    item.visible = visible;
    if (!visible && item.backdropRegistered)
    {
        (void)desktopBackdropCompositor_.RemovePanel(item.bounds);
        item.backdropRegistered = false;
    }
    if (item.clip)
    {
        (void)item.clip->SetRight(
            visible ? static_cast<float>(item.width) : 0.0f);
        (void)item.clip->SetBottom(
            visible ? static_cast<float>(item.height) : 0.0f);
    }
}

void DesktopApp::KeepDesktopWidgetBackdropPanels()
{
    for (const auto& [_, item] : desktopWidgetCompositionItems_)
    {
        if (item.visible && item.backdropRegistered)
            (void)desktopBackdropCompositor_.KeepPanel(item.bounds);
    }
}

void DesktopApp::RemoveDesktopWidgetComposition(
    const std::wstring& widgetId,
    bool invalidateRoot)
{
    pendingDesktopWidgetCompositions_.erase(widgetId);
    pendingWidgetMarqueeCompositions_.erase(widgetId);
    widgetMarqueeCompositionItems_.erase(widgetId);
    const auto position = desktopWidgetCompositionItems_.find(widgetId);
    if (position == desktopWidgetCompositionItems_.end())
        return;
    const RECT bounds = position->second.bounds;
    if (position->second.backdropRegistered)
        (void)desktopBackdropCompositor_.RemovePanel(bounds);
    if (desktopWidgetCompositionLayer_ && position->second.visual)
    {
        (void)desktopWidgetCompositionLayer_->RemoveVisual(
            position->second.visual.Get());
    }
    desktopWidgetCompositionItems_.erase(position);
    desktopWidgetCompositionZOrder_.clear();
    if (desktopWidgetCompositionItems_.empty())
    {
        if (dcompVisual_ && desktopWidgetCompositionLayer_)
        {
            (void)dcompVisual_->RemoveVisual(
                desktopWidgetCompositionLayer_.Get());
        }
        desktopWidgetCompositionLayer_.Reset();
    }
    if (invalidateRoot && hwnd_ && IsWindow(hwnd_))
    {
        RECT dirty = bounds;
        InflateRect(&dirty, 3, 3);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    if (!compositionPaintInProgress_ && dcompDevice_)
    {
        (void)CommitCompositionAnimationFrame();
        (void)FlushPendingCompositionCommit();
    }
}

void DesktopApp::PruneDesktopWidgetCompositions()
{
    std::unordered_set<std::wstring> liveWidgetIds;
    liveWidgetIds.reserve(widgets_.size());
    for (const auto& widget : widgets_)
        liveWidgetIds.insert(widget.id);

    std::vector<std::wstring> removedWidgetIds;
    for (const auto& [widgetId, _] : desktopWidgetCompositionItems_)
    {
        if (!liveWidgetIds.contains(widgetId))
            removedWidgetIds.push_back(widgetId);
    }
    for (const auto& widgetId : removedWidgetIds)
        RemoveDesktopWidgetComposition(widgetId, false);
}

bool DesktopApp::SyncDesktopWidgetCompositionZOrder()
{
    if (!desktopWidgetCompositionLayer_)
        return desktopWidgetCompositionItems_.empty();
    std::vector<std::wstring> desired;
    desired.reserve(desktopWidgetCompositionItems_.size());
    for (const auto& widget : widgets_)
    {
        if (desktopWidgetCompositionItems_.contains(widget.id))
            desired.push_back(widget.id);
    }
    if (desired == desktopWidgetCompositionZOrder_)
        return true;

    for (const auto& [_, item] : desktopWidgetCompositionItems_)
    {
        if (item.visual)
            (void)desktopWidgetCompositionLayer_->RemoveVisual(
                item.visual.Get());
    }
    for (const auto& widgetId : desired)
    {
        auto& item = desktopWidgetCompositionItems_.at(widgetId);
        const HRESULT hr = desktopWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
            return false;
    }
    desktopWidgetCompositionZOrder_ = std::move(desired);
    return true;
}

void DesktopApp::ResetDesktopWidgetComposition()
{
    presentedWidgetDragFeedback_ = {};
    pendingDesktopWidgetCompositions_.clear();
    desktopWidgetCompositionItems_.clear();
    desktopWidgetCompositionZOrder_.clear();
    if (dcompVisual_ && desktopWidgetCompositionLayer_)
    {
        (void)dcompVisual_->RemoveVisual(
            desktopWidgetCompositionLayer_.Get());
    }
    desktopWidgetCompositionLayer_.Reset();
    desktopWidgetCompositionDrawInProgress_ = false;
    desktopWidgetBackdropRequestedDuringDraw_ = false;
    desktopWidgetCompositionFailurePending_ = false;
}
