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
        std::shared_ptr<const ScrollContentOpacityMask> scrollOpacity;
        float stroke = 0.f;
        std::optional<D2D1_COLOR_F> materialColor;
        bool compensated = true;
        Microsoft::WRL::ComPtr<ID2D1Bitmap> materialBitmap;
        float materialOpacity = 1.f;
        D2D1_INTERPOLATION_MODE interpolation = D2D1_INTERPOLATION_MODE_LINEAR;

        bool IsMaterial() const noexcept { return materialColor || materialBitmap; }
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
        ID2D1Bitmap1** materials, ID2D1Bitmap1** coverage) const
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
            const auto drawn = Paint(context.Get(), {}, {}, pass);
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
        std::unordered_map<const ScrollContentOpacityMask*, ScrollContentFadeCache> fadeBrushes;
        const ScrollContentOpacityMask* activeFade = nullptr;
        std::size_t fadeLayers = 0;
        for (const auto& draw : draws_)
        {
            if (pass == Pass::Coverage)
            {
                if (!draw.compensated) continue;
                brush->SetOpacity(draw.materialOpacity);
            }
            else if (draw.materialColor)
                brush->SetColor(*draw.materialColor);
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
        draw.scrollOpacity = ScrollContentClip::CurrentOpacityMask(scope.target_);
        scope.coverage_.draws_.push_back(std::move(draw));
    }

    std::vector<Draw> draws_;
};
}
