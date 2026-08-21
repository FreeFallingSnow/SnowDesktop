#include "app.h"

namespace
{
constexpr UINT kMaximumWidgetSurfaceDimension =
    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

bool SameRect(const RECT& left, const RECT& right)
{
    return EqualRect(&left, &right) != FALSE;
}
}

// Every desktop widget owns a compact DirectComposition child surface. The
// root surface contains desktop icons only; a separate foreground surface
// covers widgets with Dock, popup, drag, marquee, and navigation content.

bool DesktopApp::QueueDesktopWidgetComposition(
    const std::wstring& widgetId)
{
    if (widgetId.empty() || !dcompDevice_ || !dcompVisual_ || !hwnd_)
        return false;

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
        return false;

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
            return false;
        hr = dcompVisual_->AddVisual(
            desktopWidgetCompositionLayer_.Get(), FALSE, nullptr);
        if (FAILED(hr))
        {
            desktopWidgetCompositionLayer_.Reset();
            return false;
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
            return false;
        }
        hr = desktopWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            desktopWidgetCompositionItems_.erase(position);
            return false;
        }
        item.bounds = bounds;
        item.visible = true;
    }

    pendingDesktopWidgetCompositions_.insert(widgetId);
    desktopWidgetCompositionFallbackIds_.erase(widgetId);

    if (compositionPaintInProgress_)
        return true;
    if (!FlushPendingDesktopWidgetComposition())
        return false;
    if (!FlushPendingWidgetMarqueeComposition() ||
        !SyncWidgetMarqueeCompositionVisibility())
        return false;
    if (!CommitCompositionAnimationFrame())
        return false;
    return FlushPendingCompositionCommit();
}

bool DesktopApp::FlushPendingDesktopWidgetComposition()
{
    if (pendingDesktopWidgetCompositions_.empty())
    {
        if (SyncDesktopWidgetCompositionZOrder())
            return true;
        std::vector<std::wstring> failedWidgetIds;
        failedWidgetIds.reserve(desktopWidgetCompositionItems_.size());
        for (const auto& [widgetId, _] :
             desktopWidgetCompositionItems_)
        {
            failedWidgetIds.push_back(widgetId);
        }
        for (const auto& widgetId : failedWidgetIds)
        {
            RemoveDesktopWidgetComposition(widgetId, false);
            desktopWidgetCompositionFallbackIds_.insert(widgetId);
        }
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, FALSE);
        return false;
    }
    if (!dcompDevice_ || !desktopWidgetCompositionLayer_)
        return false;

    auto pending = std::move(pendingDesktopWidgetCompositions_);
    pendingDesktopWidgetCompositions_.clear();
    const bool ownsPaintScope = !compositionPaintInProgress_;
    if (ownsPaintScope)
    {
        compositionPaintInProgress_ = true;
        desktopBackdropCompositor_.BeginFrame(false);
    }

    bool ok = true;
    std::vector<std::wstring> failedWidgetIds;
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
            failedWidgetIds.push_back(widgetId);
            ok = false;
            continue;
        }

        const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
        const LONG rawWidth = bounds.right - bounds.left;
        const LONG rawHeight = bounds.bottom - bounds.top;
        if (rawWidth <= 0 || rawHeight <= 0 ||
            rawWidth > static_cast<LONG>(kMaximumWidgetSurfaceDimension) ||
            rawHeight > static_cast<LONG>(kMaximumWidgetSurfaceDimension))
        {
            failedWidgetIds.push_back(widgetId);
            ok = false;
            continue;
        }
        const UINT width = static_cast<UINT>(rawWidth);
        const UINT height = static_cast<UINT>(rawHeight);
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
                failedWidgetIds.push_back(widgetId);
                ok = false;
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
            failedWidgetIds.push_back(widgetId);
            ok = false;
            continue;
        }

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x - bounds.left),
            static_cast<float>(updateOffset.y - bounds.top)));
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
            failedWidgetIds.push_back(widgetId);
            ok = false;
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
            hr = item.visual->SetOffsetX(static_cast<float>(bounds.left));
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetY(static_cast<float>(bounds.top));
        if (SUCCEEDED(hr))
            hr = item.clip->SetRight(
                item.visible ? static_cast<float>(width) : 0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetBottom(
                item.visible ? static_cast<float>(height) : 0.0f);
        if (FAILED(hr))
        {
            failedWidgetIds.push_back(widgetId);
            ok = false;
        }
    }

    if (!SyncDesktopWidgetCompositionZOrder())
    {
        failedWidgetIds.clear();
        failedWidgetIds.reserve(desktopWidgetCompositionItems_.size());
        for (const auto& [widgetId, _] :
             desktopWidgetCompositionItems_)
        {
            failedWidgetIds.push_back(widgetId);
        }
        ok = false;
    }
    for (const auto& widgetId : failedWidgetIds)
    {
        RemoveDesktopWidgetComposition(widgetId, false);
        desktopWidgetCompositionFallbackIds_.insert(widgetId);
    }
    if (ownsPaintScope)
    {
        KeepDesktopWidgetBackdropPanels();
        desktopBackdropCompositor_.EndFrame();
        compositionPaintInProgress_ = false;
    }
    if (!ok && hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
    return ok;
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
    desktopWidgetCompositionFallbackIds_.erase(widgetId);
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
    std::erase_if(desktopWidgetCompositionFallbackIds_,
        [&](const std::wstring& widgetId) {
            return !liveWidgetIds.contains(widgetId);
        });
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
    pendingDesktopWidgetCompositions_.clear();
    desktopWidgetCompositionItems_.clear();
    desktopWidgetCompositionZOrder_.clear();
    desktopWidgetCompositionFallbackIds_.clear();
    if (dcompVisual_ && desktopWidgetCompositionLayer_)
    {
        (void)dcompVisual_->RemoveVisual(
            desktopWidgetCompositionLayer_.Get());
    }
    desktopWidgetCompositionLayer_.Reset();
    desktopWidgetCompositionDrawInProgress_ = false;
    desktopWidgetBackdropRequestedDuringDraw_ = false;
}
