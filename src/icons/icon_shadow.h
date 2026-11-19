#pragma once

#include "icon_shadow_settings.h"

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <dxgi.h>
#include <functional>
#include <wrl/client.h>

namespace snowdesktop::icon_shadow
{
inline constexpr GUID kSubjectMask{
    0x5f0e70b1, 0xe1c7, 0x4c0b, {0xba, 0x34, 0x75, 0xc8, 0xe0, 0x11, 0x1e, 0x21}};

inline void AttachSubjectMask(ID2D1Bitmap1* icon, ID2D1Bitmap1* mask)
{
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (SUCCEEDED(icon->GetSurface(&surface)))
        (void)surface->SetPrivateDataInterface(kSubjectMask, mask);
}

inline Microsoft::WRL::ComPtr<ID2D1Bitmap> FindSubjectMask(ID2D1Bitmap* bitmap)
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> uploaded;
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    Microsoft::WRL::ComPtr<IUnknown> resource;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> mask;
    UINT size = sizeof(IUnknown*);
    if (SUCCEEDED(bitmap->QueryInterface(IID_PPV_ARGS(&uploaded))) &&
        SUCCEEDED(uploaded->GetSurface(&surface)) &&
        SUCCEEDED(surface->GetPrivateData(kSubjectMask, &size, resource.GetAddressOf())) && resource)
        (void)resource.As(&mask);
    return mask;
}

class SubjectScope
{
public:
    SubjectScope(ID2D1RenderTarget* target, int strength, bool probe = false)
        : target_(target), strength_(strength), probe_(probe), previous_(current_)
    {
        if (strength_ > 0) current_ = this;
    }
    ~SubjectScope()
    {
        if (strength_ > 0) current_ = previous_;
    }
    SubjectScope(const SubjectScope&) = delete;
    SubjectScope& operator=(const SubjectScope&) = delete;
    bool Used() const noexcept { return used_; }

    static int Claim(ID2D1RenderTarget* target) noexcept
    {
        auto* scope = current_;
        if (!scope || scope->target_ != target) return 0;
        scope->used_ = true;
        return scope->probe_ ? 0 : scope->strength_;
    }

    static void Draw(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destination, float opacity)
    {
        auto* scope = current_;
        if (!scope || scope->target_ != target || opacity <= 0.0f) return;
        const auto mask = FindSubjectMask(bitmap);
        if (!mask) return;
        const int strength = Claim(target);
        if (strength <= 0) return;

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        const float alpha = opacity * IconShadowOpacity(strength);
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0, alpha), &brush))) return;
        const auto antialias = target->GetAntialiasMode();
        target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        target->FillOpacityMask(mask.Get(), brush.Get(), D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
            &destination, nullptr);
        target->SetAntialiasMode(antialias);
    }

private:
    inline static thread_local SubjectScope* current_ = nullptr;
    ID2D1RenderTarget* target_;
    int strength_;
    bool probe_;
    SubjectScope* previous_;
    bool used_ = false;
};

struct BitmapSource
{
    ID2D1Bitmap* bitmap = nullptr;
    float opacity = 1.0f;
    bool shortcut = false;
    bool operator==(const BitmapSource&) const = default;
};

struct BitmapKey
{
    BitmapSource source;
    LONG width;
    LONG height;
    float scale;
    int strength;
    bool operator==(const BitmapKey&) const = default;
};

struct Drawing
{
    Microsoft::WRL::ComPtr<ID2D1Bitmap> sourceBitmap;
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadow;
    Microsoft::WRL::ComPtr<ID2D1Image> image;
    D2D1_RECT_F bounds{};
};

struct Style
{
    float scale;
    float opacity;
    bool cached = false;
};

template<class DrawIcon>
bool Record(ID2D1DeviceContext* target, ID2D1DeviceContext* recorder,
    RECT icon, const Style& style, DrawIcon&& drawIcon, Drawing& drawing)
{
    if (FAILED(recorder->CreateCommandList(&drawing.commands)) ||
        FAILED(target->CreateEffect(CLSID_D2D1Shadow, &drawing.shadow)))
        return false;

    recorder->SetDpi(96, 96);
    recorder->SetTarget(drawing.commands.Get());
    recorder->SetTransform(D2D1::Matrix3x2F::Translation(
        -static_cast<float>(icon.left), -static_cast<float>(icon.top)));
    recorder->BeginDraw();
    drawIcon(recorder);
    const HRESULT recorded = recorder->EndDraw();
    recorder->SetTarget(nullptr);
    if (FAILED(recorded) || FAILED(drawing.commands->Close()))
        return false;

    drawing.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, kIconShadowBlur * style.scale);
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{0, 0, 0, style.opacity});
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED);
    if (style.cached)
        drawing.shadow->SetValue(D2D1_PROPERTY_CACHED, static_cast<BOOL>(TRUE));

    // The command list contains the actual translucent plate, icon and badge.
    // Its per-pixel alpha weights the blur and leaves transparent plates with a softer shadow.
    drawing.shadow->SetInput(0, drawing.commands.Get());
    drawing.shadow->GetOutput(&drawing.image);
    return SUCCEEDED(target->GetImageLocalBounds(drawing.image.Get(), &drawing.bounds));
}

inline void Draw(ID2D1DeviceContext* target, const Drawing& drawing, RECT icon, float scale)
{
    const auto offset = D2D1::Point2F(icon.left + drawing.bounds.left,
        icon.top + drawing.bounds.top + kIconShadowOffset * scale);
    target->DrawImage(drawing.image.Get(), &offset, &drawing.bounds);
}
} // namespace snowdesktop::icon_shadow

template<>
struct std::hash<snowdesktop::icon_shadow::BitmapKey>
{
    std::size_t operator()(const snowdesktop::icon_shadow::BitmapKey& key) const noexcept
    {
        std::size_t hash = std::hash<ID2D1Bitmap*>{}(key.source.bitmap);
        const auto mix = [&hash](std::size_t value) {
            hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        };
        mix(std::hash<LONG>{}(key.width));
        mix(std::hash<LONG>{}(key.height));
        mix(std::hash<float>{}(key.scale));
        mix(std::hash<int>{}(key.strength));
        mix(std::hash<float>{}(key.source.opacity));
        mix(std::hash<bool>{}(key.source.shortcut));
        return hash;
    }
};
