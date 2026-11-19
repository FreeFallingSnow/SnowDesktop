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
// Records authored highlight geometry and optionally its own colored pixels.
// Replays the original transforms and clips for composition.
class HighlightCoverage
{
    struct Draw
    {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> mask;
        Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
        D2D1_RECT_F destination{};
        D2D1_MATRIX_3X2_F transform{};
        std::optional<IconBackdropClip> clip;
        std::shared_ptr<const ContentOpacityLayer> opacityLayer;
        float stroke = 0.f;
        std::optional<D2D1_COLOR_F> materialColor;
        bool compensated = true;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> materialBitmap;
        float materialOpacity = 1.f;
        D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR;

        bool IsMaterial() const noexcept { return materialColor || materialBitmap; }
    };

    struct VisibilityClip
    {
        Microsoft::WRL::ComPtr<ID2D1Geometry> geometry;
        D2D1_MATRIX_3X2_F transform;
        std::size_t drawCount;
    };

public:
    class Scope
    {
    public:
        explicit Scope(ID2D1RenderTarget* target, HighlightCoverage& coverage,
            bool separateMaterials = false)
            : target_(target), coverage_(coverage), previous_(current_),
              separateMaterials_(separateMaterials)
        { current_ = this; }
        ~Scope() { current_ = previous_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        friend class HighlightCoverage;
        ID2D1RenderTarget* target_;
        HighlightCoverage& coverage_;
        Scope* previous_;
        bool separateMaterials_;
        inline static thread_local Scope* current_ = nullptr;
    };

    static const HighlightCoverage* Current(ID2D1RenderTarget* target)
    {
        const auto* scope = Find(target);
        return scope ? &scope->coverage_ : nullptr;
    }

    // An opaque foreground covers earlier highlights, including on fallback replay.
    static void OccludePrevious(ID2D1RenderTarget* target, ID2D1Geometry* opaque,
        const RECT& opaqueClip, const RECT& frame)
    {
        auto* scope = Find(target);
        if (!scope || scope->coverage_.Empty() || !opaque || IsRectEmpty(&opaqueClip) || IsRectEmpty(&frame)) return;

        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(&factory);
        const auto rectangle = [&](const RECT& rect) {
            Microsoft::WRL::ComPtr<ID2D1RectangleGeometry> result;
            (void)factory->CreateRectangleGeometry(D2D1::RectF(
                static_cast<float>(rect.left), static_cast<float>(rect.top),
                static_cast<float>(rect.right), static_cast<float>(rect.bottom)), &result);
            return result;
        };
        const auto clip = rectangle(opaqueClip);
        const auto bounds = rectangle(frame);
        if (!clip || !bounds) return;
        const auto covered = Combine(*factory.Get(), *opaque, *clip.Get(), D2D1_COMBINE_MODE_INTERSECT);
        if (!covered) return;
        auto visible = Combine(*factory.Get(), *bounds.Get(), *covered.Get(), D2D1_COMBINE_MODE_EXCLUDE);
        if (!visible) return;

        D2D1_MATRIX_3X2_F transform{};
        target->GetTransform(&transform);
        auto& coverage = scope->coverage_;
        coverage.visibilityClips_.push_back({std::move(visible), transform, coverage.draws_.size()});
    }

    static bool ExtractSolidMask(ID2D1RenderTarget* target, ID2D1Bitmap* mask,
        const D2D1_RECT_F& bounds, const D2D1_COLOR_F& color, bool compensate = true)
    {
        auto* scope = Find(target);
        if (!scope || !scope->separateMaterials_ || !mask) return false;
        Draw draw;
        draw.mask = mask;
        draw.destination = bounds;
        draw.materialColor = color;
        draw.compensated = compensate;
        Append(*scope, std::move(draw));
        return true;
    }

    static bool ExtractBitmap(ID2D1RenderTarget* target, ID2D1Bitmap* material,
        const D2D1_RECT_F& bounds, float opacity = 1.f, ID2D1Bitmap* coverage = nullptr,
        D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR)
    {
        auto* scope = Find(target);
        if (!scope || !scope->separateMaterials_ || !material) return false;
        Draw draw;
        draw.mask = coverage ? coverage : material;
        draw.destination = bounds;
        draw.materialBitmap = material;
        draw.materialOpacity = opacity;
        draw.interpolation = interpolation;
        Append(*scope, std::move(draw));
        return true;
    }

    static bool ExtractStroke(ID2D1RenderTarget* target, ID2D1Geometry* outline,
        float width, const D2D1_COLOR_F& color)
    {
        auto* scope = Find(target);
        if (!scope || !scope->separateMaterials_ || !outline || width <= 0.f) return false;
        Draw draw;
        draw.geometry = outline;
        draw.stroke = width;
        draw.materialColor = color;
        Append(*scope, std::move(draw));
        return true;
    }

    static bool ExtractLine(ID2D1RenderTarget* target, D2D1_POINT_2F from,
        D2D1_POINT_2F to, float width, const D2D1_COLOR_F& color)
    {
        auto* scope = Find(target);
        if (!scope || !scope->separateMaterials_ || width <= 0.f) return false;
        Draw draw;
        draw.destination = {from.x, from.y, to.x, to.y};
        draw.stroke = width;
        draw.materialColor = color;
        Append(*scope, std::move(draw));
        return true;
    }

    static void SetBitmapMaterial(ID2D1Bitmap1* bitmap, ID2D1Bitmap1* body,
        ID2D1Bitmap1* material, ID2D1Bitmap1* coverage)
    {
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (!body || !material || !coverage || !bitmap || FAILED(bitmap->GetSurface(&surface))) return;
        (void)surface->SetPrivateDataInterface(kBodyKey, body);
        (void)surface->SetPrivateDataInterface(kMaterialKey, material);
        (void)surface->SetPrivateDataInterface(kMaterialCoverageKey, coverage);
    }

    static Microsoft::WRL::ComPtr<ID2D1Bitmap1> ExtractBitmapMaterial(ID2D1RenderTarget* target,
        ID2D1Bitmap* bitmap, const D2D1_RECT_F& bounds, float opacity,
        D2D1_INTERPOLATION_MODE interpolation)
    {
        const auto* scope = Find(target);
        if (!scope || !scope->separateMaterials_ || !bitmap || opacity <= 0.f) return {};
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> native;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        if (FAILED(bitmap->QueryInterface(IID_PPV_ARGS(&native))) ||
            FAILED(native->GetSurface(&surface))) return {};
        auto read = [&surface](const GUID& key) {
            Microsoft::WRL::ComPtr<IUnknown> value;
            Microsoft::WRL::ComPtr<ID2D1Bitmap1> result;
            UINT size = sizeof(IUnknown*);
            if (SUCCEEDED(surface->GetPrivateData(key, &size, value.GetAddressOf())) && value)
                (void)value.As(&result);
            return result;
        };
        auto body = read(kBodyKey);
        if (!body) return {};
        auto material = read(kMaterialKey);
        auto coverage = read(kMaterialCoverageKey);
        if (!coverage || !ExtractBitmap(target, material.Get(), bounds, opacity, coverage.Get(), interpolation))
            return {};
        return body;
    }

    bool Empty() const noexcept { return draws_.empty(); }

    bool HasExtractedMaterials() const noexcept
    {
        return std::any_of(draws_.begin(), draws_.end(),
            [](const Draw& value) { return value.IsMaterial(); });
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
        return result;
    }

    enum class Pass { Coverage, Materials, MaterialFallback };

    bool RenderMaterialSnapshot(ID2D1Device* device, D2D1_SIZE_U size,
        ID2D1Bitmap1** materials, ID2D1Bitmap1** coverage, POINT origin = {}) const
    {
        if (!HasExtractedMaterials()) return true;
        Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
        if (FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context))) return false;
        const auto render = [&](DXGI_FORMAT format, Pass pass, ID2D1Bitmap1** bitmap) {
            const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(context->CreateBitmap(size, nullptr, 0, &properties, bitmap))) return false;
            context->SetTarget(*bitmap);
            context->BeginDraw();
            const auto drawn = Paint(context.Get(), origin, {}, pass);
            const auto end = context->EndDraw();
            context->SetTarget(nullptr);
            return SUCCEEDED(drawn) && SUCCEEDED(end);
        };
        return render(DXGI_FORMAT_B8G8R8A8_UNORM, Pass::Materials, materials) &&
            render(DXGI_FORMAT_A8_UNORM, Pass::Coverage, coverage);
    }

    HRESULT Paint(ID2D1DeviceContext* target, POINT sourceOrigin, POINT outputOrigin,
        Pass pass = Pass::Coverage) const
    {
        target->SetDpi(96.f, 96.f);
        target->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        target->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        if (pass != Pass::MaterialFallback)
            target->Clear(D2D1::ColorF(0, 0.f));
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        const HRESULT hr = target->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush);
        if (FAILED(hr)) return hr;

        const auto offset = D2D1::Matrix3x2F::Translation(
            static_cast<float>(outputOrigin.x - sourceOrigin.x),
            static_cast<float>(outputOrigin.y - sourceOrigin.y));
        struct ActiveOpacityLayer { const ContentOpacityLayer* layer; bool pushed; };
        std::unordered_map<const ContentOpacityLayer*, ScrollContentFadeCache> fadeBrushes;
        std::vector<ActiveOpacityLayer> activeOpacity;
        std::vector<const ContentOpacityLayer*> requiredOpacity;
        const auto setOpacityLayers = [&](const ContentOpacityLayer* innerLayer) {
            if ((activeOpacity.empty() ? nullptr : activeOpacity.back().layer) == innerLayer) return;
            requiredOpacity.clear();
            for (auto* layer = innerLayer; layer; layer = layer->parent.get())
                requiredOpacity.push_back(layer);
            std::reverse(requiredOpacity.begin(), requiredOpacity.end());

            // Keep the shared outer groups so their opacity is applied once
            // to the combined draws, even when an inner fade changes.
            std::size_t shared = 0;
            while (shared < activeOpacity.size() && shared < requiredOpacity.size() &&
                activeOpacity[shared].layer == requiredOpacity[shared])
                ++shared;
            while (activeOpacity.size() > shared)
            {
                if (activeOpacity.back().pushed) target->PopLayer();
                activeOpacity.pop_back();
            }
            for (std::size_t index = shared; index < requiredOpacity.size(); ++index)
            {
                const auto* layer = requiredOpacity[index];
                auto* opacityBrush = layer->brush.Get();
                if (const auto& fade = layer->scrollFade)
                {
                    auto& cache = fadeBrushes.try_emplace(layer).first->second;
                    opacityBrush = cache.Resolve(target, layer->bounds, fade->leading, fade->trailing, fade->axis);
                }
                const bool push = !layer->scrollFade || opacityBrush;
                activeOpacity.push_back({layer, push});
                if (!push) continue;
                target->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&layer->transform) * offset);
                target->PushLayer(D2D1::LayerParameters1(layer->bounds, nullptr,
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::Matrix3x2F::Identity(), layer->opacity, opacityBrush), nullptr);
            }
        };
        // Each clip covers a prefix of draws. Push the longest prefix first.
        for (auto clip = visibilityClips_.rbegin(); clip != visibilityClips_.rend(); ++clip)
        {
            target->SetTransform(*D2D1::Matrix3x2F::ReinterpretBaseType(&clip->transform) * offset);
            target->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clip->geometry.Get()), nullptr);
        }
        auto nextClip = visibilityClips_.begin();
        for (std::size_t index = 0; index < draws_.size(); ++index)
        {
            while (nextClip != visibilityClips_.end() && nextClip->drawCount == index)
            {
                setOpacityLayers(nullptr);
                target->PopLayer();
                ++nextClip;
            }
            const auto& draw = draws_[index];
            if (pass == Pass::Coverage)
            {
                if (!draw.compensated) continue;
                brush->SetOpacity(draw.materialOpacity);
            }
            else if (draw.materialColor)
                brush->SetColor(*draw.materialColor);
            setOpacityLayers(draw.opacityLayer.get());
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
            if (draw.materialBitmap && (pass == Pass::Materials || pass == Pass::MaterialFallback))
            {
                target->DrawBitmap(draw.materialBitmap.Get(), &draw.destination,
                    draw.materialOpacity, draw.interpolation, nullptr, nullptr);
            }
            else if (draw.mask)
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
        setOpacityLayers(nullptr);
        for (; nextClip != visibilityClips_.end(); ++nextClip) target->PopLayer();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        return S_OK;
    }

private:
    static Microsoft::WRL::ComPtr<ID2D1PathGeometry> Combine(ID2D1Factory& factory,
        ID2D1Geometry& subject, ID2D1Geometry& mask, D2D1_COMBINE_MODE mode)
    {
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(factory.CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return {};
        const HRESULT combined = subject.CombineWithGeometry(&mask, mode, nullptr, sink.Get());
        const HRESULT closed = sink->Close();
        if (FAILED(combined) || FAILED(closed)) return {};
        return path;
    }

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

    inline static constexpr GUID kBodyKey{
        0x4fe938ee, 0x0615, 0x49ac, {0x96, 0x01, 0xa0, 0x84, 0x39, 0x39, 0x47, 0xc0}};
    inline static constexpr GUID kMaterialKey{
        0x6b55bee6, 0x274b, 0x4327, {0xa0, 0xbb, 0x31, 0x84, 0xef, 0x81, 0x8e, 0x02}};
    inline static constexpr GUID kMaterialCoverageKey{
        0xc488eb67, 0x3f69, 0x455e, {0x83, 0x87, 0xe0, 0x22, 0x39, 0x61, 0xc0, 0xe8}};
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
        draw.opacityLayer = ScopedContentOpacityLayer::Current(scope.target_);
        scope.coverage_.draws_.push_back(std::move(draw));
    }

    std::vector<Draw> draws_;
    std::vector<VisibilityClip> visibilityClips_;
};
}
