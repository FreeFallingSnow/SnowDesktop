#include "app.h"
#include "../realtime_widget_composition_rules.h"

namespace
{
constexpr UINT kMaximumRealtimeSurfaceDimension =
    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

bool SameRect(const RECT& left, const RECT& right)
{
    return EqualRect(&left, &right) != FALSE;
}
}

// High-frequency Lua widgets render into small child surfaces. The desktop
// root surface is cleared once behind each child and is not submitted again
// for subsequent data frames.

bool DesktopApp::QueueRealtimeWidgetComposition(
    const std::wstring& widgetId,
    bool refreshBackdrop)
{
    if (widgetId.empty() || !dcompDevice_ || !dcompVisual_ || !hwnd_)
        return false;

    DesktopWidget* widgetData = nullptr;
    for (auto& widget : widgets_)
    {
        if (widget.id == widgetId &&
            widget.type == DesktopWidgetType::LuaScript)
        {
            widgetData = &widget;
            break;
        }
    }
    if (!widgetData)
        return false;

    const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
    const bool desktopSurfaceVisible =
        customDesktopVisible_ &&
        !IsRectEmptyRect(bounds) &&
        (!desktopIconsHidden_ || widgetData->keepWhenDesktopHidden);
    const snowdesktop::realtime_widget_composition_rules::SceneState state{
        true,
        desktopSurfaceVisible,
        dragSession_.IsActive(),
        dragDropController_.IsExternalDragActive(),
        widgetAction_ == WidgetAction::Move ||
            widgetAction_ == WidgetAction::Resize,
        marqueeActive_,
        GetOpenPopupWidget() != nullptr,
        !luaWidgetPanelRequest_.widgetId.empty(),
    };
    if (!snowdesktop::realtime_widget_composition_rules::
            ShouldUseIndependentSurface(state))
    {
        SetRealtimeWidgetCompositionVisible(
            widgetId, false, bounds);
        return false;
    }

    if (!realtimeWidgetCompositionLayer_)
    {
        HRESULT hr = dcompDevice_->CreateVisual(
            &realtimeWidgetCompositionLayer_);
        if (FAILED(hr) || !realtimeWidgetCompositionLayer_)
            return false;
        // The realtime layer stays below all other independent overlays. Its
        // children still appear above the desktop root surface.
        hr = dcompVisual_->AddVisual(
            realtimeWidgetCompositionLayer_.Get(), FALSE, nullptr);
        if (FAILED(hr))
        {
            realtimeWidgetCompositionLayer_.Reset();
            return false;
        }
    }

    auto [position, inserted] =
        realtimeWidgetCompositionItems_.try_emplace(widgetId);
    auto& item = position->second;
    if (inserted)
    {
        HRESULT hr = dcompDevice_->CreateVisual(&item.visual);
        if (SUCCEEDED(hr))
            hr = dcompDevice_->CreateRectangleClip(&item.clip);
        if (SUCCEEDED(hr))
            hr = item.clip->SetLeft(0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetTop(0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetRight(0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetBottom(0.0f);
        if (SUCCEEDED(hr))
            hr = item.visual->SetClip(item.clip.Get());
        if (FAILED(hr) || !item.visual || !item.clip)
        {
            realtimeWidgetCompositionItems_.erase(position);
            return false;
        }
        hr = realtimeWidgetCompositionLayer_->AddVisual(
            item.visual.Get(), TRUE, nullptr);
        if (FAILED(hr))
        {
            realtimeWidgetCompositionItems_.erase(position);
            return false;
        }
        item.bounds = bounds;
        item.visible = true;
        item.rootCleared = false;
        RECT dirty = bounds;
        InflateRect(&dirty, 3, 3);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
    else
    {
        SetRealtimeWidgetCompositionVisible(
            widgetId, true, bounds);
    }

    auto [pending, pendingInserted] =
        pendingRealtimeWidgetCompositions_.try_emplace(
            widgetId, refreshBackdrop || inserted);
    if (!pendingInserted)
        pending->second = pending->second || refreshBackdrop;

    if (compositionPaintInProgress_)
        return true;
    if (!FlushPendingRealtimeWidgetComposition())
        return false;
    if (!FlushPendingWidgetMarqueeComposition() ||
        !SyncWidgetMarqueeCompositionVisibility())
        return false;
    if (!CommitCompositionAnimationFrame())
        return false;
    return FlushPendingCompositionCommit();
}

bool DesktopApp::FlushPendingRealtimeWidgetComposition()
{
    if (pendingRealtimeWidgetCompositions_.empty())
        return true;
    if (!dcompDevice_ || !realtimeWidgetCompositionLayer_)
        return false;

    auto pending = std::move(pendingRealtimeWidgetCompositions_);
    pendingRealtimeWidgetCompositions_.clear();
    const bool ownsPaintScope = !compositionPaintInProgress_;
    if (ownsPaintScope)
        compositionPaintInProgress_ = true;

    bool ok = true;
    for (const auto& [widgetId, refreshBackdrop] : pending)
    {
        auto composition =
            realtimeWidgetCompositionItems_.find(widgetId);
        if (composition == realtimeWidgetCompositionItems_.end())
            continue;

        DesktopWidget* widgetData = nullptr;
        LuaScript* luaWidget = nullptr;
        for (auto& widget : widgets_)
        {
            if (widget.id == widgetId &&
                widget.type == DesktopWidgetType::LuaScript)
            {
                widgetData = &widget;
                break;
            }
        }
        if (widgetData)
        {
            for (auto& item : items_oo_)
            {
                auto* candidate = dynamic_cast<LuaScript*>(item.get());
                if (candidate &&
                    candidate->GetWidgetData() == widgetData)
                {
                    luaWidget = candidate;
                    break;
                }
            }
        }
        if (!widgetData || !luaWidget)
        {
            ok = false;
            break;
        }

        const RECT bounds = GetStandaloneWidgetFrameRect(*widgetData);
        const LONG rawWidth = bounds.right - bounds.left;
        const LONG rawHeight = bounds.bottom - bounds.top;
        if (rawWidth <= 0 || rawHeight <= 0 ||
            rawWidth > static_cast<LONG>(kMaximumRealtimeSurfaceDimension) ||
            rawHeight > static_cast<LONG>(kMaximumRealtimeSurfaceDimension))
        {
            ok = false;
            break;
        }
        const UINT width = static_cast<UINT>(rawWidth);
        const UINT height = static_cast<UINT>(rawHeight);
        auto& item = composition->second;
        const RECT oldBounds = item.bounds;
        const bool boundsChanged = !SameRect(oldBounds, bounds);
        if (boundsChanged)
        {
            if (!IsRectEmptyRect(oldBounds))
            {
                desktopBackdropCompositor_.RemovePanel(oldBounds);
                RECT oldDirty = oldBounds;
                InflateRect(&oldDirty, 3, 3);
                InvalidateRect(hwnd_, &oldDirty, FALSE);
            }
            item.rootCleared = false;
            RECT newDirty = bounds;
            InflateRect(&newDirty, 3, 3);
            InvalidateRect(hwnd_, &newDirty, FALSE);
        }

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
                ok = false;
                break;
            }
            item.surface = std::move(surface);
            item.width = width;
            item.height = height;
        }

        ID2D1DeviceContext* rawContext = nullptr;
        POINT updateOffset{};
        HRESULT hr = item.surface->BeginDraw(
            nullptr, __uuidof(ID2D1DeviceContext),
            reinterpret_cast<void**>(&rawContext),
            &updateOffset);
        if (FAILED(hr) || !rawContext)
        {
            ok = false;
            break;
        }

        ComPtr<ID2D1DeviceContext> context;
        context.Attach(rawContext);
        context->SetDpi(96.0f, 96.0f);
        context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x - bounds.left),
            static_cast<float>(updateOffset.y - bounds.top)));
        context->Clear(D2D1::ColorF(0, 0, 0, 0));

        if (refreshBackdrop)
            desktopBackdropCompositor_.BeginFrame(false);
        realtimeCompositionDrawInProgress_ = true;
        realtimeBackdropRegisteredDuringDraw_ = false;
        luaWidget->DrawCompositionSurface(
            context.Get(), widgetData->bounds,
            widgetData->selected ? 2 : 0,
            refreshBackdrop);
        realtimeCompositionDrawInProgress_ = false;
        const bool registeredBackdrop =
            realtimeBackdropRegisteredDuringDraw_;
        realtimeBackdropRegisteredDuringDraw_ = false;

        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context.Reset();
        const HRESULT endDrawHr = item.surface->EndDraw();
        brushCache_.clear();
        brushCacheContext_ = nullptr;
        if (refreshBackdrop)
        {
            if (!registeredBackdrop)
                desktopBackdropCompositor_.RemovePanel(bounds);
            desktopBackdropCompositor_.EndFrame();
        }
        if (FAILED(endDrawHr))
        {
            ok = false;
            break;
        }

        item.bounds = bounds;
        hr = item.visual->SetContent(item.surface.Get());
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetX(
                static_cast<float>(bounds.left));
        if (SUCCEEDED(hr))
            hr = item.visual->SetOffsetY(
                static_cast<float>(bounds.top));
        if (SUCCEEDED(hr))
            hr = item.clip->SetRight(
                item.visible ? static_cast<float>(width) : 0.0f);
        if (SUCCEEDED(hr))
            hr = item.clip->SetBottom(
                item.visible ? static_cast<float>(height) : 0.0f);
        if (FAILED(hr))
        {
            ok = false;
            break;
        }
    }

    if (ownsPaintScope)
        compositionPaintInProgress_ = false;
    if (!ok)
    {
        for (const auto& [widgetId, _] : pending)
            RemoveRealtimeWidgetComposition(widgetId, false);
    }
    return ok;
}

bool DesktopApp::HasRealtimeWidgetComposition(
    const std::wstring& widgetId) const
{
    return realtimeWidgetCompositionItems_.contains(widgetId);
}

void DesktopApp::SetRealtimeWidgetCompositionVisible(
    const std::wstring& widgetId,
    bool visible,
    const RECT& bounds)
{
    const auto position =
        realtimeWidgetCompositionItems_.find(widgetId);
    if (position == realtimeWidgetCompositionItems_.end())
        return;
    auto& item = position->second;
    if (!visible)
        item.rootCleared = false;
    if (item.visible == visible)
        return;
    item.visible = visible;
    if (item.clip)
    {
        (void)item.clip->SetRight(
            visible ? static_cast<float>(item.width) : 0.0f);
        (void)item.clip->SetBottom(
            visible ? static_cast<float>(item.height) : 0.0f);
    }
    if (visible && !item.rootCleared && hwnd_)
    {
        RECT dirty = IsRectEmptyRect(bounds) ? item.bounds : bounds;
        InflateRect(&dirty, 3, 3);
        InvalidateRect(hwnd_, &dirty, FALSE);
    }
}

void DesktopApp::MarkRealtimeWidgetCompositionRootCleared(
    const RECT* updateRect)
{
    for (auto& [_, item] : realtimeWidgetCompositionItems_)
    {
        if (!item.visible)
            continue;
        if (!updateRect)
        {
            item.rootCleared = true;
            continue;
        }
        RECT intersection{};
        if (IntersectRect(&intersection, updateRect, &item.bounds))
            item.rootCleared = true;
    }
}

void DesktopApp::KeepRealtimeWidgetBackdropPanels()
{
    for (const auto& [_, item] : realtimeWidgetCompositionItems_)
    {
        if (item.visible)
            (void)desktopBackdropCompositor_.KeepPanel(item.bounds);
    }
}

void DesktopApp::RemoveRealtimeWidgetComposition(
    const std::wstring& widgetId,
    bool invalidateRoot)
{
    pendingRealtimeWidgetCompositions_.erase(widgetId);
    const auto position =
        realtimeWidgetCompositionItems_.find(widgetId);
    if (position == realtimeWidgetCompositionItems_.end())
        return;
    const RECT bounds = position->second.bounds;
    if (realtimeWidgetCompositionLayer_ && position->second.visual)
    {
        (void)realtimeWidgetCompositionLayer_->RemoveVisual(
            position->second.visual.Get());
    }
    realtimeWidgetCompositionItems_.erase(position);
    desktopBackdropCompositor_.RemovePanel(bounds);
    if (realtimeWidgetCompositionItems_.empty())
    {
        if (dcompVisual_ && realtimeWidgetCompositionLayer_)
        {
            (void)dcompVisual_->RemoveVisual(
                realtimeWidgetCompositionLayer_.Get());
        }
        realtimeWidgetCompositionLayer_.Reset();
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

void DesktopApp::ResetRealtimeWidgetComposition()
{
    pendingRealtimeWidgetCompositions_.clear();
    realtimeWidgetCompositionItems_.clear();
    if (dcompVisual_ && realtimeWidgetCompositionLayer_)
    {
        (void)dcompVisual_->RemoveVisual(
            realtimeWidgetCompositionLayer_.Get());
    }
    realtimeWidgetCompositionLayer_.Reset();
    realtimeCompositionDrawInProgress_ = false;
    realtimeBackdropRegisteredDuringDraw_ = false;
}
