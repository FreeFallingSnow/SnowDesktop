#pragma once
#include "taskbar_hook_protocol.h"
#include "../theme/flat_glass_rim.h"
#include "../platform/taskbar_monitor.h"
#include <d2d1.h>
#include <wrl/client.h>
#include <array>
#include <vector>

namespace snowdesktop::taskbar_hook
{
enum class TaskbarMaterialEdge { Top, Bottom, Left, Right };

inline TaskbarMaterialEdge ResolveTaskbarMaterialEdge(const RECT& taskbar, const RECT& monitor) noexcept
{
    // Like status bars, only the edge facing the desktop receives a rim.
    // Use the full monitor bounds: its work area already excludes the taskbar.
    const auto distance = [](LONG a, LONG b) { return std::abs(static_cast<double>(a) - b); };
    if (static_cast<double>(taskbar.right) - taskbar.left >=
        static_cast<double>(taskbar.bottom) - taskbar.top)
        return distance(taskbar.top, monitor.top) < distance(taskbar.bottom, monitor.bottom)
            ? TaskbarMaterialEdge::Bottom : TaskbarMaterialEdge::Top;
    return distance(taskbar.left, monitor.left) < distance(taskbar.right, monitor.right)
        ? TaskbarMaterialEdge::Right : TaskbarMaterialEdge::Left;
}

inline TaskbarMaterialEdge ResolveTaskbarMaterialEdge(HWND taskbar) noexcept
{
    RECT bounds{};
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetWindowRect(taskbar, &bounds) ||
        !GetMonitorInfoW(taskbar_monitor::Resolve(taskbar), &monitor)) return TaskbarMaterialEdge::Top;
    return ResolveTaskbarMaterialEdge(bounds, monitor.rcMonitor);
}

// Both taskbar implementations use the same physical-pixel rim. The native
// taskbar owns its geometry; only the global material's edge values are used.
template<class Appearance>
inline std::vector<std::uint8_t> MakeTaskbarEdgePixels(UINT width, UINT height,
    float scale, const Appearance& style, TaskbarMaterialEdge edge)
{
    if (!width || !height || width > 16384 || height > 16384 ||
        static_cast<std::uint64_t>(width) * height > 16 * 1024 * 1024) return {};
    scale = std::isfinite(scale) ? std::clamp(scale, .5f, 8.f) : 1.f;
    const auto finite = [](float value, float fallback, float minimum, float maximum) {
        return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
    };
    const float borderWidth = finite(style.edge.borderWidth, 1.f, .5f, 4.f) * scale;
    const float depth = finite(style.edge.highlightWidth, 1.5f, .5f, 4.f) * scale;
    const float strength = style.edge.highlightEnabled
        ? finite(style.edge.highlightStrength, .5f, 0.f, 1.f) : 0.f;
    const float borderAlpha = finite(style.borderAlpha, 0.f, 0.f, 1.f);
    const flat_glass_rim::Evaluator light(style.edge.light);
    const float support = std::max(borderWidth,
        std::max(light.InnerSupport(depth, false), light.InnerSupport(depth, true)));
    const float opacity = finite(style.alpha, 0.f, 0.f, 1.f);
    const auto reflect = [opacity, strength, finite](float color) {
        const float surface = 1.f + (finite(color, 0.f, 0.f, 1.f) - 1.f) * opacity;
        return surface + (1.f - surface) * (.42f + .16f * strength);
    };
    const std::array<float, 3> reflected{reflect(style.blue), reflect(style.green), reflect(style.red)};
    const std::array<float, 3> border{finite(style.borderBlue, 1.f, 0.f, 1.f),
        finite(style.borderGreen, 1.f, 0.f, 1.f), finite(style.borderRed, 1.f, 0.f, 1.f)};
    const auto distanceAt = [edge, width, height](float x, float y) {
        switch (edge)
        {
        case TaskbarMaterialEdge::Bottom: return static_cast<float>(height) - y;
        case TaskbarMaterialEdge::Left: return x;
        case TaskbarMaterialEdge::Right: return static_cast<float>(width) - x;
        default: return y;
        }
    };
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x)
    {
        const float distance = distanceAt(static_cast<float>(x) + .5f, static_cast<float>(y) + .5f);
        if (distance > support + .5f) continue;
        float alpha = 0.f;
        std::array<float, 3> color{};
        for (const float sy : {.25f, .75f}) for (const float sx : {.25f, .75f})
        {
            const float px = static_cast<float>(x) + sx, py = static_cast<float>(y) + sy;
            const float d = distanceAt(px, py);
            const float illumination = light.Lighting(px / static_cast<float>(width), py / static_cast<float>(height));
            const float outline = d <= borderWidth ? borderAlpha : 0.f;
            const float reflection = strength * light.Intensity(d, depth, illumination);
            const float shadow = strength * light.settings.shadowStrength * light.Occlusion(d, depth, illumination);
            const float combined = (outline + reflection * (1.f - outline)) * (1.f - shadow) + shadow;
            alpha += combined * .25f;
            for (std::size_t c = 0; c < color.size(); ++c)
                color[c] += ((border[c] * outline * (1.f - reflection) + reflected[c] * reflection) *
                    (1.f - shadow) + std::array<float, 3>{.035f, .025f, .015f}[c] * shadow) * .25f;
        }
        const auto index = (static_cast<std::size_t>(y) * width + x) * 4;
        for (std::size_t c = 0; c < color.size(); ++c)
            pixels[index + c] = static_cast<std::uint8_t>(std::lround(std::clamp(color[c], 0.f, alpha) * 255.f));
        pixels[index + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.f, 1.f) * 255.f));
    }
    return pixels;
}

template<class Appearance>
inline HRESULT DrawTaskbarEdges(ID2D1RenderTarget* target, UINT width, UINT height,
    float scale, const Appearance& style, TaskbarMaterialEdge edge)
{
    const auto pixels = MakeTaskbarEdgePixels(width, height, scale, style, edge);
    if (pixels.empty()) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const HRESULT hr = target->CreateBitmap(D2D1::SizeU(width, height), pixels.data(), width * 4, properties, &bitmap);
    if (SUCCEEDED(hr)) target->DrawBitmap(bitmap.Get(), D2D1::RectF(0, 0,
        static_cast<float>(width), static_cast<float>(height)), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return hr;
}
}
