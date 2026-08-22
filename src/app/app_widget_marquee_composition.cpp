#include "app.h"
#include "../widget_visibility_rules.h"

namespace
{
constexpr UINT kMaximumMarqueeSurfaceDimension =
    D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;

bool IsFiniteMarquee(const LuaWidget::NativeMarqueeText& marquee)
{
    const float viewportWidth =
        marquee.viewport.right - marquee.viewport.left;
    const float viewportHeight =
        marquee.viewport.bottom - marquee.viewport.top;
    if (!marquee.layout || !std::isfinite(viewportWidth) ||
        !std::isfinite(viewportHeight) || viewportWidth <= 0.0f ||
        viewportHeight <= 0.0f ||
        viewportWidth > kMaximumMarqueeSurfaceDimension ||
        viewportHeight > kMaximumMarqueeSurfaceDimension ||
        !std::isfinite(marquee.originX) ||
        !std::isfinite(marquee.originY) ||
        !std::isfinite(marquee.textWidth) ||
        !std::isfinite(marquee.textHeight) ||
        !std::isfinite(marquee.speed) || !std::isfinite(marquee.gap) ||
        marquee.textWidth < 0.0f || marquee.textHeight < 0.0f ||
        marquee.speed <= 0.0f || marquee.gap < 0.0f)
        return false;

    const float contentWidth = marquee.scrolling
        ? marquee.textWidth * 2.0f + marquee.gap
        : marquee.textWidth;
    return std::isfinite(contentWidth) && contentWidth >= 0.0f &&
        std::ceil(contentWidth + 2.0f) <=
            kMaximumMarqueeSurfaceDimension;
}

D2D1_COLOR_F MarqueeColor(int rgb, float alpha)
{
    return D2D1::ColorF(
        static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
        static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
        static_cast<float>(rgb & 0xFF) / 255.0f,
        std::clamp(alpha, 0.0f, 1.0f));
}
}

// Each marquee is a clipped DirectComposition child visual. DirectComposition
// evaluates the repeating OffsetX animation in the compositor, so continuous
// movement does not schedule WM_PAINT or re-enter the Lua renderer.

bool DesktopApp::QueueWidgetMarqueeComposition(
    const std::wstring& widgetId,
    const std::vector<LuaWidget::NativeMarqueeText>& marquees,
    bool reducedMotion)
{
    if (widgetId.empty() || !dcompDevice_ || !dcompVisual_)
        return false;

    const bool supported = std::all_of(
        marquees.begin(), marquees.end(), IsFiniteMarquee);
    PendingWidgetMarqueeComposition pending;
    if (supported)
        pending.marquees = marquees;
    pending.reducedMotion = reducedMotion;
    pendingWidgetMarqueeCompositions_.insert_or_assign(
        widgetId, std::move(pending));

    if (!snowdesktop::widget_composition_layer_rules::
            ShouldDeferWidgetSurfaceDraw(
                compositionPaintInProgress_,
                floatingDockCompositionPaintInProgress_,
                floatingPopupCompositionPaintInProgress_))
    {
        if (!FlushPendingWidgetMarqueeComposition())
            return false;
        if (!SyncWidgetMarqueeCompositionVisibility())
            return false;
        if (!CommitCompositionAnimationFrame() ||
            !FlushPendingCompositionCommit())
            return false;
    }
    return supported;
}

bool DesktopApp::FlushPendingWidgetMarqueeComposition()
{
    if (pendingWidgetMarqueeCompositions_.empty())
        return true;
    if (!dcompDevice_)
        return false;

    auto pending = std::move(pendingWidgetMarqueeCompositions_);
    pendingWidgetMarqueeCompositions_.clear();
    const auto now = std::chrono::steady_clock::now();
    for (auto& [widgetId, request] : pending)
    {
        const auto parent =
            desktopWidgetCompositionItems_.find(widgetId);
        if (parent == desktopWidgetCompositionItems_.end() ||
            !parent->second.visual)
        {
            widgetMarqueeCompositionItems_.erase(widgetId);
            continue;
        }
        auto& items = widgetMarqueeCompositionItems_[widgetId];
        std::unordered_set<std::string> liveKeys;
        liveKeys.reserve(request.marquees.size());

        for (const auto& marquee : request.marquees)
        {
            liveKeys.insert(marquee.key);
            auto [position, inserted] = items.try_emplace(marquee.key);
            auto& item = position->second;
            float phase = 0.0f;
            if (!inserted && item.scrolling && item.cycle > 0.0f &&
                item.phaseTime.time_since_epoch().count() != 0)
            {
                const float elapsed = static_cast<float>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - item.phaseTime).count()) / 1'000'000.0f;
                phase = std::fmod(
                    item.phase + std::max(0.0f, elapsed) * item.speed,
                    item.cycle);
            }

            const bool animate = marquee.scrolling &&
                !request.reducedMotion;
            const float cycle = marquee.textWidth + marquee.gap;
            if (animate && cycle > 0.0f)
                phase = std::fmod(phase, cycle);
            else
                phase = 0.0f;

            const float viewportWidth =
                marquee.viewport.right - marquee.viewport.left;
            const float viewportHeight =
                marquee.viewport.bottom - marquee.viewport.top;
            const float contentWidth = animate
                ? marquee.textWidth * 2.0f + marquee.gap
                : marquee.textWidth;
            const UINT surfaceWidth = static_cast<UINT>(std::max(
                1.0f, std::ceil(contentWidth + 2.0f)));
            const UINT surfaceHeight = static_cast<UINT>(std::max(
                1.0f, std::ceil(viewportHeight)));

            ComPtr<IDCompositionSurface> surface;
            HRESULT hr = dcompDevice_->CreateSurface(
                surfaceWidth, surfaceHeight,
                DXGI_FORMAT_B8G8R8A8_UNORM,
                DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
            if (FAILED(hr) || !surface)
                return false;

            ID2D1DeviceContext* rawContext = nullptr;
            POINT updateOffset{};
            hr = surface->BeginDraw(nullptr,
                __uuidof(ID2D1DeviceContext),
                reinterpret_cast<void**>(&rawContext), &updateOffset);
            if (FAILED(hr) || !rawContext)
                return false;
            ComPtr<ID2D1DeviceContext> context;
            context.Attach(rawContext);
            context->SetDpi(96.0f, 96.0f);
            context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
            context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->Clear(D2D1::ColorF(0, 0, 0, 0));
            ComPtr<ID2D1SolidColorBrush> brush;
            hr = context->CreateSolidColorBrush(
                MarqueeColor(marquee.color, marquee.alpha), &brush);
            if (SUCCEEDED(hr) && brush)
            {
                const float textY = static_cast<float>(updateOffset.y) +
                    marquee.originY - marquee.viewport.top;
                context->DrawTextLayout(D2D1::Point2F(
                        static_cast<float>(updateOffset.x), textY),
                    marquee.layout.Get(), brush.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
                if (animate)
                {
                    context->DrawTextLayout(D2D1::Point2F(
                            static_cast<float>(updateOffset.x) + cycle,
                            textY),
                        marquee.layout.Get(), brush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            context.Reset();
            const HRESULT endDrawHr = surface->EndDraw();
            if (FAILED(hr) || FAILED(endDrawHr))
                return false;

            if (inserted)
            {
                hr = dcompDevice_->CreateVisual(&item.clipVisual);
                if (SUCCEEDED(hr))
                    hr = dcompDevice_->CreateVisual(&item.textVisual);
                if (SUCCEEDED(hr))
                    hr = dcompDevice_->CreateRectangleClip(&item.clip);
                if (SUCCEEDED(hr))
                    hr = item.clipVisual->AddVisual(
                        item.textVisual.Get(), TRUE, nullptr);
                if (SUCCEEDED(hr))
                    hr = parent->second.visual->AddVisual(
                        item.clipVisual.Get(), TRUE, nullptr);
                if (FAILED(hr))
                {
                    items.erase(position);
                    return false;
                }
            }

            hr = item.clip->SetLeft(0.0f);
            if (SUCCEEDED(hr)) hr = item.clip->SetTop(0.0f);
            item.clipWidth = viewportWidth;
            item.clipHeight = viewportHeight;
            if (SUCCEEDED(hr))
                hr = item.clip->SetRight(
                    item.visible ? viewportWidth : 0.0f);
            if (SUCCEEDED(hr))
                hr = item.clip->SetBottom(
                    item.visible ? viewportHeight : 0.0f);
            if (SUCCEEDED(hr))
                hr = item.clipVisual->SetClip(item.clip.Get());
            if (SUCCEEDED(hr))
                hr = item.clipVisual->SetOffsetX(
                    marquee.viewport.left -
                    static_cast<float>(parent->second.bounds.left));
            if (SUCCEEDED(hr))
                hr = item.clipVisual->SetOffsetY(
                    marquee.viewport.top -
                    static_cast<float>(parent->second.bounds.top));
            if (SUCCEEDED(hr))
                hr = item.textVisual->SetOffsetY(0.0f);
            if (SUCCEEDED(hr))
                hr = item.textVisual->SetContent(surface.Get());

            const float originX =
                marquee.originX - marquee.viewport.left;
            if (SUCCEEDED(hr) && animate)
            {
                ComPtr<IDCompositionAnimation> animation;
                hr = dcompDevice_->CreateAnimation(&animation);
                const double duration = static_cast<double>(cycle) /
                    static_cast<double>(marquee.speed);
                if (SUCCEEDED(hr))
                    hr = animation->AddCubic(0.0,
                        originX - phase, -marquee.speed, 0.0f, 0.0f);
                if (SUCCEEDED(hr))
                    hr = animation->AddRepeat(duration, duration);
                if (SUCCEEDED(hr))
                    hr = item.textVisual->SetOffsetX(animation.Get());
            }
            else if (SUCCEEDED(hr))
            {
                hr = item.textVisual->SetOffsetX(originX);
            }
            if (FAILED(hr))
                return false;

            item.surface = std::move(surface);
            item.cycle = animate ? cycle : 0.0f;
            item.speed = animate ? marquee.speed : 0.0f;
            item.phase = phase;
            item.phaseTime = now;
            item.scrolling = animate;
        }

        for (auto item = items.begin(); item != items.end();)
        {
            if (liveKeys.contains(item->first))
            {
                ++item;
                continue;
            }
            if (item->second.clipVisual)
            {
                (void)parent->second.visual->RemoveVisual(
                    item->second.clipVisual.Get());
            }
            item = items.erase(item);
        }
        if (items.empty())
            widgetMarqueeCompositionItems_.erase(widgetId);
    }
    return true;
}

bool DesktopApp::SyncWidgetMarqueeCompositionVisibility()
{
    const bool hasPreviewSource =
        mouseDownWidgetIndex_ < widgets_.size();
    const std::wstring* previewSourceId = hasPreviewSource
        ? &widgets_[mouseDownWidgetIndex_].id
        : nullptr;
    for (auto& [widgetId, items] : widgetMarqueeCompositionItems_)
    {
        const auto parent =
            desktopWidgetCompositionItems_.find(widgetId);
        const bool isPreviewSource =
            previewSourceId && widgetId == *previewSourceId;
        const bool visible =
            parent != desktopWidgetCompositionItems_.end() &&
            parent->second.visible &&
            !snowdesktop::widget_visibility_rules::
                ShouldHideWidgetPreviewSource(
                    widgetAction_ == WidgetAction::Move,
                    widgetAction_ == WidgetAction::Resize,
                    isPreviewSource);
        for (auto& [key, item] : items)
        {
            (void)key;
            if (item.visible == visible)
                continue;
            if (item.clip &&
                (FAILED(item.clip->SetRight(
                    visible ? item.clipWidth : 0.0f)) ||
                 FAILED(item.clip->SetBottom(
                    visible ? item.clipHeight : 0.0f))))
                return false;
            item.visible = visible;
        }
    }
    return true;
}

void DesktopApp::ResetWidgetMarqueeComposition()
{
    pendingWidgetMarqueeCompositions_.clear();
    widgetMarqueeCompositionItems_.clear();
}
