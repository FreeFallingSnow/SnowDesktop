#include "scroll_content_clip.h"

#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::ScrollContentClip;
using snowdesktop::ScrollContentFadeCache;

void Check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "FAILED: scroll content clip: " << message << '\n';
    std::exit(1);
}

void Ok(HRESULT result, const char* message)
{
    Check(SUCCEEDED(result), message);
}

struct DrawOptions
{
    RECT viewport{24, 32, 120, 128};
    int scrollOffset = 48;
    int contentHeight = 192;
    float fadeLength = 16.0f;
    float scale = 1.0f;
    float translationX = 0.0f;
    float translationY = 0.0f;
    float dpi = 96.0f;
    bool fixedChrome = false;
    bool translucentBackground = false;
};

struct Pixels
{
    static constexpr UINT32 Width = 320;
    static constexpr UINT32 Height = 320;
    std::vector<std::uint32_t> values;
    DrawOptions options;

    std::uint32_t At(float x, float y) const
    {
        const auto px = static_cast<UINT32>(std::floor(
            (x * options.scale + options.translationX) * options.dpi / 96.0f));
        const auto py = static_cast<UINT32>(std::floor(
            (y * options.scale + options.translationY) * options.dpi / 96.0f));
        Check(px < Width && py < Height, "pixel probe stays within its isolated target");
        return values[static_cast<std::size_t>(py) * Width + px];
    }

    unsigned Alpha(float x, float y) const { return At(x, y) >> 24; }
};

struct Raster
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;

    Raster()
    {
        Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr), "create WARP device");
        ComPtr<IDXGIDevice> dxgi;
        Ok(d3d.As(&dxgi), "query DXGI device");
        Ok(D2D1CreateDevice(dxgi.Get(), nullptr, &device), "create D2D device");
        Ok(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context),
            "create D2D context");
    }

    Pixels Draw(ScrollContentFadeCache& cache, const DrawOptions& o)
    {
        const auto size = D2D1::SizeU(Pixels::Width, Pixels::Height);
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), o.dpi, o.dpi);
        ComPtr<ID2D1Bitmap1> target;
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &target), "create target");
        context->SetTarget(target.Get());
        context->SetDpi(o.dpi, o.dpi);
        context->SetTransform(D2D1::Matrix3x2F::Scale(o.scale, o.scale) *
            D2D1::Matrix3x2F::Translation(o.translationX, o.translationY));
        ComPtr<ID2D1SolidColorBrush> brush;
        Ok(context->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush), "create brush");
        const float left = static_cast<float>(o.viewport.left);
        const float right = static_cast<float>(o.viewport.right);
        const float top = static_cast<float>(o.viewport.top);
        const float bottom = static_cast<float>(o.viewport.bottom);
        context->BeginDraw();
        context->Clear(o.translucentBackground ? D2D1::ColorF(0x204060, 0.4f)
            : D2D1::ColorF(0, 0.0f));
        if (o.fixedChrome)
        {
            brush->SetColor(D2D1::ColorF(0x00ff00));
            context->FillRectangle(D2D1::RectF(left, top - 8, right, top), brush.Get());
        }
        brush->SetColor(D2D1::ColorF(0xffffff));
        {
            // This is the production scope used around scrolling item drawing.
            // Deliberately overdraw horizontally to detect a lost viewport clip.
            ScrollContentClip clip(context.Get(), cache, o.viewport,
                o.scrollOffset, o.contentHeight, o.fadeLength);
            context->FillRectangle(D2D1::RectF(left - 8,
                top - static_cast<float>(o.scrollOffset),
                right + (o.translucentBackground ? -8.0f : 8.0f),
                top - static_cast<float>(o.scrollOffset) + static_cast<float>(o.contentHeight)),
                brush.Get());
        }
        if (o.fixedChrome)
        {
            brush->SetColor(D2D1::ColorF(0x0000ff));
            context->FillRectangle(D2D1::RectF(left, bottom, right, bottom + 8), brush.Get());
        }
        Ok(context->EndDraw(), "render scrolling content without resource or stack errors");
        context->SetTarget(nullptr);
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ComPtr<ID2D1Bitmap1> readable;
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &readable), "create readback");
        Ok(readable->CopyFromBitmap(nullptr, target.Get(), nullptr), "copy rendered pixels");
        D2D1_MAPPED_RECT mapped{};
        Ok(readable->Map(D2D1_MAP_OPTIONS_READ, &mapped), "map pixels");
        Pixels pixels{std::vector<std::uint32_t>(Pixels::Width * Pixels::Height), o};
        for (UINT32 row = 0; row < Pixels::Height; ++row)
            std::copy_n(reinterpret_cast<const std::uint32_t*>(mapped.bits + row * mapped.pitch),
                Pixels::Width, pixels.values.begin() + static_cast<std::size_t>(row) * Pixels::Width);
        Ok(readable->Unmap(), "unmap pixels");
        return pixels;
    }
};

void CheckBothEdges(const Pixels& pixels)
{
    const float x = static_cast<float>(pixels.options.viewport.left) + 12;
    const float top = static_cast<float>(pixels.options.viewport.top);
    const float bottom = static_cast<float>(pixels.options.viewport.bottom);
    const unsigned topEdge = pixels.Alpha(x, top + 1);
    const unsigned bottomEdge = pixels.Alpha(x, bottom - 2);
    Check(topEdge > 0 && topEdge < 80 && bottomEdge > 0 && bottomEdge < 80,
        "both clipped edges fade their actual alpha toward transparent");
    Check(pixels.Alpha(x, top + 12) > 160 && pixels.Alpha(x, top + 12) < 250 &&
            pixels.Alpha(x, bottom - 13) > 160 && pixels.Alpha(x, bottom - 13) < 250,
        "each edge transitions gradually rather than using a hard cutoff");
    Check(pixels.Alpha(x, (top + bottom) / 2) == 255,
        "the middle of scrollable content retains full opacity");
    const auto edgePixel = pixels.At(x, top + 1);
    Check((edgePixel & 0xffu) == topEdge && ((edgePixel >> 8) & 0xffu) == topEdge &&
            ((edgePixel >> 16) & 0xffu) == topEdge,
        "white content remains premultiplied white instead of receiving a dark overlay");
}
} // namespace

void RunScrollContentClipTests()
{
    Raster raster;
    ScrollContentFadeCache cache;
    DrawOptions options;
    auto pixels = raster.Draw(cache, options);
    CheckBothEdges(pixels);
    Check(pixels.Alpha(22, 80) == 0 && pixels.Alpha(122, 80) == 0 &&
            pixels.Alpha(48, 30) == 0 && pixels.Alpha(48, 130) == 0,
        "scrolling content cannot paint outside the viewport on any side");

    options.scrollOffset = 0;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 32) == 255 && pixels.Alpha(48, 126) < 80,
        "the first row stays clear at scroll start while hidden bottom content fades");
    options.scrollOffset = 96;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 33) < 80 && pixels.Alpha(48, 127) == 255,
        "the last row stays clear at the real content end while hidden top content fades");

    options.scrollOffset = 0;
    for (const int height : {60, 96})
    {
        options.contentHeight = height;
        pixels = raster.Draw(cache, options);
        Check(pixels.Alpha(48, 32) == 255 &&
                pixels.Alpha(48, static_cast<float>(31 + height)) == 255,
            "content that fits the viewport is fully readable at both boundaries");
    }
    options.contentHeight = 0;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 80) == 0, "empty content leaves no mask-colored rectangle");

    options.contentHeight = 192;
    options.scrollOffset = 112;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 33) < 80 && pixels.Alpha(48, 111) == 255 &&
            pixels.Alpha(48, 112) == 0,
        "extra scroll padding does not fade the final visible content or invent a bottom edge");

    options.scrollOffset = 1;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 32) > 0 && pixels.Alpha(48, 32) < 255 &&
            pixels.Alpha(48, 34) == 255,
        "one pixel of hidden top content does not dim a whole row");
    options.scrollOffset = 95;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 127) > 0 && pixels.Alpha(48, 127) < 255 &&
            pixels.Alpha(48, 125) == 255,
        "the bottom fade shrinks as the last content approaches the viewport");

    options.viewport = RECT{24, 32, 120, 48};
    options.scrollOffset = 48;
    pixels = raster.Draw(cache, options);
    Check(pixels.Alpha(48, 32) < 80 && pixels.Alpha(48, 47) < 80 &&
            pixels.Alpha(48, 36) == 255 && pixels.Alpha(48, 43) == 255,
        "a very short viewport keeps its middle half fully readable");

    options = {};
    options.fixedChrome = true;
    pixels = raster.Draw(cache, options);
    CheckBothEdges(pixels);
    Check(pixels.At(48, 28) == 0xff00ff00u && pixels.At(48, 132) == 0xff0000ffu,
        "fixed headers and footers retain their colors and opacity outside the scroll scope");

    options.fixedChrome = false;
    options.translucentBackground = true;
    pixels = raster.Draw(cache, options);
    Check(pixels.At(116, 33) == pixels.At(116, 80) &&
            pixels.At(116, 126) == pixels.At(116, 80) &&
            pixels.Alpha(116, 80) >= 101 && pixels.Alpha(116, 80) <= 103,
        "fading content leaves the translucent panel background unchanged");

    options = {};
    options.viewport = RECT{48, 52, 144, 148};
    pixels = raster.Draw(cache, options);
    CheckBothEdges(pixels);
    for (const float dpi : {96.0f, 144.0f, 192.0f})
    {
        options = {};
        options.dpi = dpi;
        if (dpi == 96.0f)
        {
            options.scale = 1.5f;
            options.translationX = 7;
            options.translationY = 9;
        }
        pixels = raster.Draw(cache, options);
        CheckBothEdges(pixels);
        Check(pixels.Alpha(22, 80) == 0 && pixels.Alpha(48, 30) == 0,
            "translated, scaled and high-DPI content stays clipped in the correct coordinates");
    }

    // Widgets retain their caches through graphics recovery. Exercise the same
    // cache with independently created devices; EndDraw detects stale resources.
    Raster replacement;
    options = {};
    CheckBothEdges(replacement.Draw(cache, options));
    CheckBothEdges(raster.Draw(cache, options));
}
