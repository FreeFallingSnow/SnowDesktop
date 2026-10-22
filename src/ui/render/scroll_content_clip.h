#pragma once

#include "icon_backdrop_clip.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace snowdesktop
{

enum class ScrollContentAxis { Vertical, Horizontal };

// Each native scrolling widget reuses its mask while the viewport and edge
// distances are unchanged. A new D2D device must never reuse the old brush.
class ScrollContentFadeCache
{
    friend class ScrollContentClip;

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
        const float length = (std::min)(fadeLength, extent * 0.25f);
        const float offset = static_cast<float>((std::max)(0, scrollOffset));
        const float leading = (std::min)(length, offset);
        const float trailing = std::clamp(
            static_cast<float>(contentExtent) - extent - offset, 0.0f, length);
        auto* brush = cache.Resolve(context_, bounds, leading, trailing, axis);
        if (!brush) return;
        context_->PushLayer(D2D1::LayerParameters1(bounds, nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(),
            1.0f, brush), nullptr);
        layerPushed_ = true;
    }

    ~ScrollContentClip()
    {
        if (!context_) return;
        if (layerPushed_) context_->PopLayer();
        context_->PopAxisAlignedClip();
    }

    ScrollContentClip(const ScrollContentClip&) = delete;
    ScrollContentClip& operator=(const ScrollContentClip&) = delete;

private:
    ID2D1DeviceContext* context_;
    ScopedIconBackdropClip backdropClip_;
    bool layerPushed_ = false;
};

} // namespace snowdesktop
