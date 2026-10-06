#pragma once

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace snowdesktop
{

// Each native scrolling widget reuses its mask while the viewport and edge
// distances are unchanged. A new D2D device must never reuse the old brush.
class ScrollContentFadeCache
{
    friend class ScrollContentClip;

    ID2D1LinearGradientBrush* Resolve(ID2D1DeviceContext* context,
        const D2D1_RECT_F& viewport, float top, float bottom)
    {
        if (top <= 0.0f && bottom <= 0.0f) return nullptr;
        const float height = viewport.bottom - viewport.top;
        Microsoft::WRL::ComPtr<ID2D1Device> device;
        context->GetDevice(&device);
        if (device.Get() != device_.Get() || height != height_ ||
            top != top_ || bottom != bottom_)
        {
            brush_.Reset();
            device_ = device;
            height_ = height;
            top_ = top;
            bottom_ = bottom;
        }
        if (!brush_)
        {
            const D2D1_GRADIENT_STOP stops[] = {
                { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    top > 0.0f ? 0.0f : 1.0f) },
                { top / height, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
                { 1.0f - bottom / height,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) },
                { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    bottom > 0.0f ? 0.0f : 1.0f) },
            };
            Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
            if (FAILED(context->CreateGradientStopCollection(stops, 4,
                    D2D1_GAMMA_1_0, D2D1_EXTEND_MODE_CLAMP, &collection)))
                return nullptr;
            if (FAILED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(viewport.left, viewport.top),
                        D2D1::Point2F(viewport.left, viewport.bottom)),
                    collection.Get(), &brush_)))
                return nullptr;
        }
        brush_->SetStartPoint(D2D1::Point2F(viewport.left, viewport.top));
        brush_->SetEndPoint(D2D1::Point2F(viewport.left, viewport.bottom));
        return brush_.Get();
    }

    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush_;
    float height_ = 0.0f;
    float top_ = 0.0f;
    float bottom_ = 0.0f;
};

// Scope only the scrolling items, after fixed search/tabs/details headers.
// Multiply content alpha rather than tinting the translucent panel background.
class ScrollContentClip
{
public:
    ScrollContentClip(ID2D1DeviceContext* context, ScrollContentFadeCache& cache,
        const RECT& viewport, int scrollOffset, int contentHeight, float fadeLength)
        : context_(context)
    {
        if (!context_) return;
        const auto bounds = D2D1::RectF(
            static_cast<float>(viewport.left), static_cast<float>(viewport.top),
            static_cast<float>((std::max)(viewport.left, viewport.right)),
            static_cast<float>((std::max)(viewport.top, viewport.bottom)));
        context_->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float height = bounds.bottom - bounds.top;
        if (height <= 0.0f || bounds.right <= bounds.left || contentHeight <= 0 ||
            !std::isfinite(fadeLength) || fadeLength <= 0.0f)
            return;

        // Keep the middle half clear even in a very short viewport. Grow the
        // fade with the hidden distance so the first scroll pixel cannot dim
        // a whole row. Trailing scroll padding is not hidden content.
        const float length = (std::min)(fadeLength, height * 0.25f);
        const float offset = static_cast<float>((std::max)(0, scrollOffset));
        const float top = (std::min)(length, offset);
        const float bottom = std::clamp(
            static_cast<float>(contentHeight) - height - offset, 0.0f, length);
        auto* brush = cache.Resolve(context_, bounds, top, bottom);
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
    bool layerPushed_ = false;
};

} // namespace snowdesktop
