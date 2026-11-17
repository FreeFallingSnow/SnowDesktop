#pragma once

#include "ui/render/scroll_content_clip.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace snowdesktop
{
// Records only coverage of authored highlighter draws. Replays the same
// transforms into a second app-owned surface for the compositor effect.
class HighlightCoverage
{
    struct Draw
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> mask;
        Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
        D2D1_RECT_F destination{};
        D2D1_MATRIX_3X2_F transform{};
        std::optional<IconBackdropClip> clip;
        std::shared_ptr<const ScrollContentOpacityMask> scrollOpacity;
        float stroke = 0.f;
    };

public:
    class Scope
    {
    public:
        explicit Scope(ID2D1RenderTarget* target, HighlightCoverage& coverage)
            : target_(target), coverage_(coverage), previous_(current_)
        { current_ = this; }
        ~Scope() { current_ = previous_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        friend class HighlightCoverage;
        ID2D1RenderTarget* target_;
        HighlightCoverage& coverage_;
        Scope* previous_;
        inline static thread_local Scope* current_ = nullptr;
    };

    static const HighlightCoverage* Current(ID2D1RenderTarget* target)
    {
        const auto* scope = Find(target);
        return scope ? &scope->coverage_ : nullptr;
    }

    static void RecordMask(ID2D1RenderTarget* target, ID2D1Bitmap* mask, const D2D1_RECT_F& bounds)
    {
        auto* scope = Find(target);
        if (!scope || !mask) return;
        Draw draw;
        draw.mask = mask;
        draw.destination = bounds;
        Append(*scope, std::move(draw));
    }

    static void RecordStroke(ID2D1RenderTarget* target, ID2D1Geometry* outline, float width)
    {
        auto* scope = Find(target);
        if (!scope || !outline || width <= 0.f) return;
        Draw draw;
        draw.geometry = outline;
        draw.stroke = width;
        Append(*scope, std::move(draw));
    }

    static void RecordLine(ID2D1RenderTarget* target, D2D1_POINT_2F from, D2D1_POINT_2F to, float width)
    {
        auto* scope = Find(target);
        if (!scope || width <= 0.f) return;
        Draw draw;
        draw.destination = {from.x, from.y, to.x, to.y};
        draw.stroke = width;
        Append(*scope, std::move(draw));
    }

    // The companion A8 bitmap shares the asset's lifetime and travels with
    // the original through static caches, snapshot draws and cloned visuals.
    static void SetBitmapMask(ID2D1Bitmap1* bitmap, ID2D1Bitmap1* mask)
    {
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (bitmap && SUCCEEDED(bitmap->GetSurface(&surface)))
            (void)surface->SetPrivateDataInterface(kCoverageKey, mask);
    }

    static void RecordBitmap(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& bounds)
    {
        if (!Find(target) || !bitmap) return;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> native;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(bitmap->QueryInterface(IID_PPV_ARGS(&native))) ||
            FAILED(native->GetSurface(&surface))) return;
        Microsoft::WRL::ComPtr<IUnknown> value;
        UINT size = sizeof(IUnknown*);
        if (FAILED(surface->GetPrivateData(kCoverageKey, &size, value.GetAddressOf()))) return;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> mask;
        if (value && SUCCEEDED(value.As(&mask))) RecordMask(target, mask.Get(), bounds);
    }

    bool Empty() const noexcept { return draws_.empty(); }

    // Keep a translucent background together when a highlight tile moves
    // some of its pixels into the compositor. This does not add mask coverage.
    static void RecordMaterialBounds(ID2D1RenderTarget* target, const D2D1_RECT_F& bounds)
    {
        if (auto* scope = Find(target))
        {
            D2D1::Matrix3x2F transform;
            target->GetTransform(&transform);
            scope->coverage_.materialBounds_.push_back(PixelBounds(TransformBounds(bounds, transform)));
        }
    }

    std::vector<RECT> Bounds(POINT offset) const
    {
        std::vector<RECT> result;
        result.reserve(draws_.size());
        for (const auto& draw : draws_)
        {
            D2D1_RECT_F bounds{};
            if (draw.geometry)
            {
                if (FAILED(draw.geometry->GetWidenedBounds(draw.stroke, nullptr,
                    &draw.transform, D2D1_DEFAULT_FLATTENING_TOLERANCE, &bounds)))
                    continue;
            }
            else
            {
                const auto& t = *D2D1::Matrix3x2F::ReinterpretBaseType(&draw.transform);
                auto area = draw.destination;
                if (!draw.mask)
                {
                    const float inset = draw.stroke * .5f;
                    area = {std::min(area.left, area.right) - inset, std::min(area.top, area.bottom) - inset,
                        std::max(area.left, area.right) + inset, std::max(area.top, area.bottom) + inset};
                }
                bounds = TransformBounds(area, t);
            }
            auto pixels = PixelBounds(bounds);
            OffsetRect(&pixels, offset.x, offset.y);
            result.push_back(pixels);
        }
        const auto highlightCount = result.size();
        for (auto material : materialBounds_)
        {
            OffsetRect(&material, offset.x, offset.y);
            const bool intersectsHighlight = std::any_of(result.begin(), result.begin() + highlightCount,
                [&](const RECT& highlight) {
                    RECT overlap{};
                    return IntersectRect(&overlap, &material, &highlight) != FALSE;
                });
            if (intersectsHighlight) result.push_back(material);
        }
        return result;
    }

    void AttachToSnapshot(ID2D1Device* device, ID2D1Bitmap1* snapshot) const
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> mask;
        if (!Empty())
        {
            Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)) &&
                SUCCEEDED(context->CreateBitmap(snapshot->GetPixelSize(),
                    nullptr, 0, &properties, &mask)))
            {
                context->SetTarget(mask.Get());
                context->BeginDraw();
                const auto draw = Paint(context.Get(), {}, {});
                const auto end = context->EndDraw();
                context->SetTarget(nullptr);
                if (FAILED(draw) || FAILED(end)) mask.Reset();
            }
        }
        SetBitmapMask(snapshot, mask.Get());
    }

    HRESULT Paint(ID2D1DeviceContext* target, POINT sourceOrigin, POINT outputOrigin) const
    {
        target->SetDpi(96.f, 96.f);
        target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        target->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        target->Clear(D2D1::ColorF(0, 0.f));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        const HRESULT hr = target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush);
        if (FAILED(hr)) return hr;

        const auto offset = D2D1::Matrix3x2F::Translation(
            static_cast<float>(outputOrigin.x - sourceOrigin.x),
            static_cast<float>(outputOrigin.y - sourceOrigin.y));
        std::unordered_map<const ScrollContentOpacityMask*, ScrollContentFadeCache> fadeBrushes;
        const ScrollContentOpacityMask* activeFade = nullptr;
        std::size_t fadeLayers = 0;
        for (const auto& draw : draws_)
        {
            if (activeFade != draw.scrollOpacity.get())
            {
                while (fadeLayers > 0)
                {
                    target->PopLayer();
                    --fadeLayers;
                }
                activeFade = draw.scrollOpacity.get();
                for (auto* fade = activeFade; fade; fade = fade->parent.get())
                {
                    auto& cache = fadeBrushes.try_emplace(fade).first->second;
                    auto* opacity = cache.Resolve(target, fade->bounds, fade->leading, fade->trailing, fade->axis);
                    if (!opacity) continue;
                    target->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&fade->transform) * offset);
                    target->PushLayer(D2D1::LayerParameters1(fade->bounds, nullptr,
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), 1.0f, opacity), nullptr);
                    ++fadeLayers;
                }
            }
            target->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&draw.transform) * offset);
            std::size_t layers = 0;
            if (draw.clip)
            {
                const auto& bounds = draw.clip->bounds;
                target->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(bounds.left),
                    static_cast<float>(bounds.top), static_cast<float>(bounds.right),
                    static_cast<float>(bounds.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
                for (auto node = draw.clip->geometry; node; node = node->parent)
                {
                    target->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(),
                        node->geometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Translation(static_cast<float>(draw.clip->geometryOffset.x),
                            static_cast<float>(draw.clip->geometryOffset.y))), nullptr);
                    ++layers;
                }
            }
            if (draw.mask)
            {
                target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                target->FillOpacityMask(draw.mask.Get(), brush.Get(), &draw.destination, nullptr);
            }
            else
            {
                target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                if (draw.geometry) target->DrawGeometry(draw.geometry.Get(), brush.Get(), draw.stroke);
                else target->DrawLine({draw.destination.left, draw.destination.top},
                    {draw.destination.right, draw.destination.bottom}, brush.Get(), draw.stroke);
            }
            for (std::size_t i = 0; i < layers; ++i) target->PopLayer();
            if (draw.clip) target->PopAxisAlignedClip();
        }
        for (std::size_t i = 0; i < fadeLayers; ++i) target->PopLayer();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }

private:
    static D2D1_RECT_F TransformBounds(const D2D1_RECT_F& bounds, const D2D1::Matrix3x2F& transform)
    {
        const auto lt = transform.TransformPoint({bounds.left, bounds.top});
        const auto rt = transform.TransformPoint({bounds.right, bounds.top});
        const auto lb = transform.TransformPoint({bounds.left, bounds.bottom});
        const auto rb = transform.TransformPoint({bounds.right, bounds.bottom});
        return {std::min({lt.x, rt.x, lb.x, rb.x}), std::min({lt.y, rt.y, lb.y, rb.y}),
            std::max({lt.x, rt.x, lb.x, rb.x}), std::max({lt.y, rt.y, lb.y, rb.y})};
    }

    static RECT PixelBounds(const D2D1_RECT_F& bounds)
    {
        return {static_cast<LONG>(std::floor(bounds.left)), static_cast<LONG>(std::floor(bounds.top)),
            static_cast<LONG>(std::ceil(bounds.right)), static_cast<LONG>(std::ceil(bounds.bottom))};
    }

    inline static constexpr GUID kCoverageKey{
        0x49c3bafd, 0x8b1e, 0x47bd, {0x8e, 0xd1, 0xb4, 0x37, 0x40, 0x18, 0x89, 0xad}};
    static Scope* Find(ID2D1RenderTarget* target)
    {
        for (auto* scope = Scope::current_; scope; scope = scope->previous_)
            if (scope->target_ == target) return scope;
        return nullptr;
    }

    static void Append(Scope& scope, Draw draw)
    {
        scope.target_->GetTransform(&draw.transform);
        draw.clip = ScopedIconBackdropClip::Current(scope.target_);
        if (draw.clip && IsRectEmpty(&draw.clip->bounds)) return;
        draw.scrollOpacity = ScrollContentClip::CurrentOpacityMask(scope.target_);
        scope.coverage_.draws_.push_back(std::move(draw));
    }

    std::vector<Draw> draws_;
    std::vector<RECT> materialBounds_;
};
}
