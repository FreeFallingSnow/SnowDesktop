#pragma once

#include "icon_backdrop_clip.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <memory>

namespace snowdesktop
{

enum class ScrollContentAxis { Vertical, Horizontal };

struct ScrollContentFade
{
    float leading;
    float trailing;
    ScrollContentAxis axis;
};

inline ScrollContentFade ResolveScrollContentFade(float extent, float offset,
    float maximum, float fadeLength, ScrollContentAxis axis)
{
    const float length = (std::min)(fadeLength, extent * 0.25f);
    return {std::clamp(offset, 0.0f, length),
        std::clamp(maximum - offset, 0.0f, length), axis};
}

struct ContentOpacityLayer
{
    D2D1_RECT_F bounds;
    D2D1_MATRIX_3X2_F transform;
    float opacity;
    Microsoft::WRL::ComPtr<ID2D1Brush> brush;
    std::optional<ScrollContentFade> scrollFade;
    std::shared_ptr<const ContentOpacityLayer> parent;
};

// The original draw and the highlight replay use the same opacity layer.
class ScopedContentOpacityLayer
{
public:
    ScopedContentOpacityLayer(ID2D1DeviceContext* context, const D2D1_RECT_F& bounds,
        float opacity, ID2D1Brush* brush = nullptr, std::optional<ScrollContentFade> scrollFade = {})
        : context_(context), previous_(current_)
    {
        if (!context_) return;
        D2D1_MATRIX_3X2_F transform{};
        context_->GetTransform(&transform);
        // The scrolling brush is cached and can move before replay. Keep its
        // fade distances and rebuild it in the replay context instead.
        layer_ = std::make_shared<ContentOpacityLayer>(ContentOpacityLayer{bounds, transform,
            opacity, Microsoft::WRL::ComPtr<ID2D1Brush>(scrollFade ? nullptr : brush),
            scrollFade, Current(context_)});
        current_ = this;
        context_->PushLayer(D2D1::LayerParameters1(bounds, nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), opacity, brush), nullptr);
    }

    ~ScopedContentOpacityLayer()
    {
        if (!context_) return;
        context_->PopLayer();
        current_ = previous_;
    }

    ScopedContentOpacityLayer(const ScopedContentOpacityLayer&) = delete;
    ScopedContentOpacityLayer& operator=(const ScopedContentOpacityLayer&) = delete;

    static std::shared_ptr<const ContentOpacityLayer> Current(ID2D1RenderTarget* target)
    {
        for (auto* scope = current_; scope; scope = scope->previous_)
            if (scope->context_ == target) return scope->layer_;
        return {};
    }

private:
    ID2D1DeviceContext* context_;
    const ScopedContentOpacityLayer* previous_;
    std::shared_ptr<const ContentOpacityLayer> layer_;
    inline static thread_local const ScopedContentOpacityLayer* current_ = nullptr;
};

// Each native scrolling widget reuses its mask while the viewport and edge
// distances are unchanged. A new D2D device must never reuse the old brush.
class ScrollContentFadeCache
{
public:

    ID2D1LinearGradientBrush* Resolve(ID2D1DeviceContext* context,
        const D2D1_RECT_F& viewport, float leading, float trailing,
        ScrollContentAxis axis)
    {
        if (leading <= 0.0f && trailing <= 0.0f) return nullptr;
        const bool horizontal = axis == ScrollContentAxis::Horizontal;
        const float extent = horizontal ? viewport.right - viewport.left
            : viewport.bottom - viewport.top;
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        context->GetDevice(&device);
        if (device.Get() != device_.Get() || extent != extent_ ||
            leading != leading_ || trailing != trailing_)
        {
            brush_.Reset();
            device_ = device;
            extent_ = extent;
            leading_ = leading;
            trailing_ = trailing;
        }
        if (!brush_)
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    leading > 0.0f ? 0.0f : 1.0f) },
                { leading / extent, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
                { 1.0f - trailing / extent,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
                { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    trailing > 0.0f ? 0.0f : 1.0f) },
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (FAILED(context->CreateGradientStopCollection(stops, 4,
                    D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, &collection)))
                return nullptr;
            if (FAILED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(viewport.left, viewport.top),
                        D2D1::Point2F(horizontal ? viewport.right : viewport.left,
                            horizontal ? viewport.top : viewport.bottom)),
                    collection.Get(), &brush_)))
                return nullptr;
        }
        brush_->SetStartPoint(D2D1::Point2F(viewport.left, viewport.top));
        brush_->SetEndPoint(D2D1::Point2F(horizontal ? viewport.right : viewport.left,
            horizontal ? viewport.top : viewport.bottom));
        return brush_.Get();
    }

private:
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush_;
    float extent_ = 0.0f;
    float leading_ = 0.0f;
    float trailing_ = 0.0f;
};

// Scope scrolling items or a horizontal tab strip, excluding fixed chrome.
// Multiply content alpha rather than tinting the translucent panel background.
class ScrollContentClip
{
public:
    ScrollContentClip(ID2D1DeviceContext* context, ScrollContentFadeCache& cache,
        const RECT& viewport, int scrollOffset, int contentExtent, float fadeLength,
        ScrollContentAxis axis = ScrollContentAxis::Vertical)
        : context_(context), backdropClip_(context, viewport)
    {
        if (!context_) return;
        const auto bounds = D2D1::RectF(
            static_cast<float>(viewport.left), static_cast<float>(viewport.top),
            static_cast<float>((std::max)(viewport.left, viewport.right)),
            static_cast<float>((std::max)(viewport.top, viewport.bottom)));
        context_->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float extent = axis == ScrollContentAxis::Horizontal
            ? bounds.right - bounds.left : bounds.bottom - bounds.top;
        if (bounds.bottom <= bounds.top || bounds.right <= bounds.left || contentExtent <= 0 ||
            !std::isfinite(fadeLength) || fadeLength <= 0.0f)
            return;

        // Keep the middle half clear even in a very small viewport. Grow the
        // fade with the hidden distance so the first scroll pixel cannot dim
        // a whole row. Trailing scroll padding is not hidden content.
        const float offset = static_cast<float>((std::max)(0, scrollOffset));
        const auto fade = ResolveScrollContentFade(extent, offset,
            static_cast<float>(contentExtent) - extent, fadeLength, axis);
        auto* brush = cache.Resolve(context_, bounds, fade.leading, fade.trailing, axis);
        if (!brush) return;
        opacityLayer_.emplace(context_, bounds, 1.0f, brush, fade);
    }

    ~ScrollContentClip()
    {
        if (!context_) return;
        opacityLayer_.reset();
        context_->PopAxisAlignedClip();
    }

    ScrollContentClip(const ScrollContentClip&) = delete;
    ScrollContentClip& operator=(const ScrollContentClip&) = delete;

private:
    ID2D1DeviceContext* context_;
    ScopedIconBackdropClip backdropClip_;
    std::optional<ScopedContentOpacityLayer> opacityLayer_;
};

} // namespace snowdesktop
