#pragma once

#include "icon_bitmap_renderer.h"
#include "desktop/display_color_mode.h"
#include <d2d1effects.h>
#include <array>
#include <cmath>

namespace snowdesktop::icon_glass_color
{
// Material opacity is authored in the SDR/sRGB drawing space. WCG composition
// uses linear coverage; retain the material's perceptual intensity there.
inline float LinearCoverage(float opacity)
{
    opacity = std::clamp(opacity, 0.f, 1.f);
    return opacity <= .04045f ? opacity / 12.92f : std::pow((opacity + .055f) / 1.055f, 2.4f);
}

inline bool NeedsConversion(display_color::Mode mode) { return mode == display_color::Mode::WideColor; }

namespace detail
{
struct Binding { ID2D1RenderTarget* target = nullptr; POINT screenOrigin{}; };
inline thread_local Binding binding;

struct Resources
{
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    Microsoft::WRL::ComPtr<ID2D1Effect> scale, transfer;
    float coverageOpacity = -1.f;
};
inline thread_local Resources resources;
}

// Bind only the desktop foreground paint. Retained components, popups and
// previews do not inherit a monitor decision from a different drawing target.
class ScopedDesktopPaint
{
public:
    ScopedDesktopPaint(ID2D1RenderTarget* target, POINT screenOrigin) : previous_(detail::binding)
        { detail::binding = {target, screenOrigin}; }
    ~ScopedDesktopPaint() { detail::binding = previous_; }
    ScopedDesktopPaint(const ScopedDesktopPaint&) = delete;
    ScopedDesktopPaint& operator=(const ScopedDesktopPaint&) = delete;
private:
    detail::Binding previous_;
};

inline void ResetResources() { detail::resources = {}; }

inline bool DrawConverted(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
    const icon_bitmap_renderer::DrawPlan& plan, float opacity)
{
    if (!target || !bitmap || IsRectEmpty(&plan.bounds)) return false;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> context;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(&context))) || !context) return false;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    context->GetDevice(&device);
    auto& resources = detail::resources;
    if (resources.device.Get() != device.Get())
    {
        resources = {};
        resources.device = device;
        if (FAILED(context->CreateEffect(CLSID_D2D12DAffineTransform, &resources.scale)) ||
            FAILED(context->CreateEffect(CLSID_D2D1TableTransfer, &resources.transfer)))
            { resources.scale.Reset(); resources.transfer.Reset(); return false; }
        if (FAILED(resources.scale->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR)) ||
            FAILED(resources.transfer->SetValue(D2D1_TABLETRANSFER_PROP_RED_DISABLE, TRUE)) ||
            FAILED(resources.transfer->SetValue(D2D1_TABLETRANSFER_PROP_GREEN_DISABLE, TRUE)) ||
            FAILED(resources.transfer->SetValue(D2D1_TABLETRANSFER_PROP_BLUE_DISABLE, TRUE)) ||
            FAILED(resources.transfer->SetValue(D2D1_TABLETRANSFER_PROP_CLAMP_OUTPUT, TRUE)))
            { resources.scale.Reset(); resources.transfer.Reset(); return false; }
        resources.transfer->SetInputEffect(0, resources.scale.Get());
    }
    if (!resources.scale || !resources.transfer) return false;
    const auto source = bitmap->GetSize();
    if (source.width <= 0.f || source.height <= 0.f) return false;
    const auto& frame = plan.bounds;
    const auto matrix = D2D1::Matrix3x2F::Scale(
        static_cast<float>(frame.right - frame.left) / source.width,
        static_cast<float>(frame.bottom - frame.top) / source.height);
    if (resources.coverageOpacity != opacity)
    {
        std::array<float, 256> coverage;
        for (size_t i = 0; i < coverage.size(); ++i)
            coverage[i] = LinearCoverage(static_cast<float>(i) / 255.f * opacity);
        if (FAILED(resources.transfer->SetValue(D2D1_TABLETRANSFER_PROP_ALPHA_TABLE,
            reinterpret_cast<const BYTE*>(coverage.data()), static_cast<UINT32>(sizeof(coverage))))) return false;
        resources.coverageOpacity = opacity;
    }
    if (FAILED(resources.scale->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, matrix))) return false;
    resources.scale->SetInput(0, bitmap);
    const auto sourceFrame = D2D1::RectF(0.f, 0.f, static_cast<float>(frame.right - frame.left),
        static_cast<float>(frame.bottom - frame.top));
    context->DrawImage(resources.transfer.Get(),
        D2D1::Point2F(static_cast<float>(frame.left), static_cast<float>(frame.top)), sourceFrame);
    return true;
}

inline bool TryDrawDesktop(ID2D1RenderTarget* target, ID2D1Bitmap* bitmap,
    const icon_bitmap_renderer::DrawPlan& plan, float opacity)
{
    if (detail::binding.target != target) return false;
    RECT screen = plan.bounds;
    OffsetRect(&screen, detail::binding.screenOrigin.x, detail::binding.screenOrigin.y);
    if (!NeedsConversion(display_color::ModeForMonitor(MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST)))) return false;
    return DrawConverted(target, bitmap, plan, opacity);
}
}
