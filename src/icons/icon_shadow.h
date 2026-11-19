#pragma once

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <d2d1helper.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <array>
#include <cmath>

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

    static void Draw(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
        const D2D1_RECT_F& destination, float opacity)
    {
        auto* scope = current_;
        if (!scope || scope->target_ != target || opacity <= 0.0f) return;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> uploaded;
        Microsoft::WRL::ComPtr<IDXGISurface> surface;
        Microsoft::WRL::ComPtr<IUnknown> resource;
        UINT size = sizeof(IUnknown*);
        if (FAILED(bitmap->QueryInterface(IID_PPV_ARGS(&uploaded))) ||
            FAILED(uploaded->GetSurface(&surface)) ||
            FAILED(surface->GetPrivateData(kSubjectMask, &size, resource.GetAddressOf())) || !resource)
            return;

        Microsoft::WRL::ComPtr<ID2D1Bitmap> mask;
        if (FAILED(resource.As(&mask))) return;
        scope->used_ = true;
        if (scope->probe_) return;

        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        const float alpha = opacity * (scope->strength_ / 100.0f);
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

struct Drawing
{
    Microsoft::WRL::ComPtr<ID2D1CommandList> commands;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadow;
    Microsoft::WRL::ComPtr<ID2D1Effect> compensation;
    Microsoft::WRL::ComPtr<ID2D1Image> image;
    D2D1_RECT_F bounds{};
};

struct Style
{
    float scale;
    float opacity;
    bool compensate;
    bool cached = false;
};

inline const auto& WideColorAlphaTable()
{
    // Over black, an sRGB power-space blend leaves (1 - alpha) of the
    // encoded backdrop. Convert that attenuation to linear compositor alpha.
    static const auto table = [] {
        std::array<float, 256> values{};
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            const float alpha = static_cast<float>(i) / static_cast<float>(values.size() - 1);
            values[i] = 1.0f - std::pow(1.0f - alpha, 2.4f);
        }
        return values;
    }();
    return table;
}

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

    drawing.shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 2.5f * style.scale);
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1_VECTOR_4F{0, 0, 0, style.opacity});
    drawing.shadow->SetValue(D2D1_SHADOW_PROP_OPTIMIZATION, D2D1_SHADOW_OPTIMIZATION_BALANCED);
    if (style.cached)
        drawing.shadow->SetValue(D2D1_PROPERTY_CACHED, static_cast<BOOL>(TRUE));

    // The command list contains the actual translucent plate, icon and badge.
    // Its per-pixel alpha weights the blur before the strength and color correction.
    drawing.shadow->SetInput(0, drawing.commands.Get());
    drawing.shadow->GetOutput(&drawing.image);
    if (style.compensate &&
        SUCCEEDED(target->CreateEffect(CLSID_D2D1TableTransfer, &drawing.compensation)))
    {
        const auto& table = WideColorAlphaTable();
        if (SUCCEEDED(drawing.compensation->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_TABLE,
                D2D1_PROPERTY_TYPE_UNKNOWN, reinterpret_cast<const BYTE*>(table.data()),
                static_cast<UINT32>(table.size() * sizeof(float)))))
        {
            drawing.compensation->SetInput(0, drawing.image.Get());
            drawing.compensation->GetOutput(&drawing.image);
        }
    }
    return SUCCEEDED(target->GetImageLocalBounds(drawing.image.Get(), &drawing.bounds));
}

inline void Draw(ID2D1DeviceContext* target, const Drawing& drawing, RECT icon, float scale)
{
    const auto offset = D2D1::Point2F(icon.left + drawing.bounds.left,
        icon.top + drawing.bounds.top + 3.0f * scale);
    target->DrawImage(drawing.image.Get(), &offset, &drawing.bounds);
}
} // namespace snowdesktop::icon_shadow
