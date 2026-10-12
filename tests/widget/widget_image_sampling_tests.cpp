#include "widget/view/widget_image_sampling.h"
#include <d3d11.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Cache = snowdesktop::widget_runtime::WidgetImageSamplingCache;
void Check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "FAILED: image sampling: " << message << '\n';
    std::exit(1);
}
void Ok(HRESULT hr, const char* message)
{
    if (SUCCEEDED(hr)) return;
    std::cerr << "FAILED: image sampling: " << message << " (0x" << std::hex
        << static_cast<unsigned long>(hr) << ")\n";
    std::exit(1);
}
struct Graphics
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context;
    Graphics()
    {
        Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr), "create software D3D device");
        ComPtr<IDXGIDevice> dxgi;
        Ok(d3d.As(&dxgi), "query DXGI device");
        Ok(D2D1CreateDevice(dxgi.Get(), nullptr, &device), "create D2D device");
        Ok(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context),
            "create render context");
    }
    ComPtr<ID2D1Bitmap1> Source(bool transparent = false, bool colors = false,
        UINT side = 512)
    {
        std::vector<std::uint32_t> pixels(side * side);
        for (UINT y = 0; y < side; ++y)
            for (UINT x = 0; x < side; ++x)
                pixels[y * side + x] = colors ?
                    (x < side / 2 ? 0xffff0000u : 0xff00ff00u) :
                    (x % 8 == 0 ? 0xffffffffu : (transparent ? 0u : 0xff000000u));
        ComPtr<ID2D1Bitmap1> bitmap;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        Ok(context->CreateBitmap(D2D1::SizeU(side, side), pixels.data(), side * 4,
            properties, &bitmap), "create pattern source");
        return bitmap;
    }
    std::vector<std::uint8_t> Pixels(const Cache::Result& image, float opacity = 1)
    {
        constexpr UINT side = 32;
        ComPtr<ID2D1Bitmap1> target, cpu;
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        Ok(context->CreateBitmap(D2D1::SizeU(side, side), nullptr, 0, properties, &target),
            "create render target");
        context->SetDpi(96, 96);
        context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetTarget(target.Get());
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0.0f));
        context->DrawBitmap(image.bitmap.Get(), D2D1::RectF(0, 0, side, side), opacity,
            image.interpolation, image.source);
        Ok(context->EndDraw(), "draw sampled pixels");
        context->SetTarget(nullptr);
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        Ok(context->CreateBitmap(D2D1::SizeU(side, side), nullptr, 0, properties, &cpu),
            "create readback");
        Ok(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr), "copy pixels");
        D2D1_MAPPED_RECT mapped{};
        Ok(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped), "map pixels");
        std::vector<std::uint8_t> pixels(side * side * 4);
        for (UINT y = 0; y < side; ++y)
            std::copy_n(mapped.bits + y * mapped.pitch, side * 4, pixels.data() + y * side * 4);
        Ok(cpu->Unmap(), "unmap pixels");
        return pixels;
    }
};
void AreaAverage(const std::vector<std::uint8_t>& pixels, int alpha)
{
    // Independent area coverage: one white column in every eight source columns.
    // Direct bilinear samples only two columns and produces the wrong luminance.
    for (UINT y = 4; y < 28; ++y)
        for (UINT x = 4; x < 28; ++x)
        {
            const auto i = (y * 32 + x) * 4;
            Check(std::abs(int(pixels[i]) - 32) <= 4 &&
                std::abs(int(pixels[i + 1]) - 32) <= 4 &&
                std::abs(int(pixels[i + 2]) - 32) <= 4 &&
                std::abs(int(pixels[i + 3]) - alpha) <= 4,
                "shrinking preserves stripe area coverage and premultiplied alpha");
        }
}
}

void RunWidgetImageSamplingTests()
{
    Graphics graphics;
    auto source = graphics.Source();
    Cache cache;
    const auto destination = D2D1::RectF(0, 0, 32, 32);
    const auto full = D2D1::RectF(0, 0, 512, 512);
    const auto resolve = [&](bool nearest = false) {
        return cache.Resolve(graphics.context.Get(), source.Get(), destination, full, nearest);
    };
    auto first = resolve();
    Check(first.outcome == Cache::Outcome::Miss && first.bitmap.Get() != source.Get(),
        "first shrink creates a derived immutable image");
    AreaAverage(graphics.Pixels(first), 255);
    const auto second = resolve();
    Check(second.outcome == Cache::Outcome::Hit && second.bitmap.Get() == first.bitmap.Get(),
        "unchanged source and physical size reuse the reduced texture");
    Check(graphics.Pixels(first) == graphics.Pixels(second), "cached reuse preserves actual pixels");

    Cache::Result old;
    old.bitmap = source;
    old.source = full;
    const auto linear = graphics.Pixels(old);
    Check(std::abs(int(linear[(16 * 32 + 16) * 4]) - 32) > 20,
        "negative control: old direct linear sampling fails the same area coverage input");
    const auto nearest = resolve(true);
    Check(nearest.bitmap.Get() == source.Get() && nearest.outcome == Cache::Outcome::Bypass,
        "nearest retains the original texture");
    const auto point = graphics.Pixels(nearest);
    for (std::size_t i = 0; i < point.size(); i += 4)
        Check(point[i] == 0 || point[i] == 255, "nearest introduces no blended colors");

    graphics.context->SetDpi(192, 192);
    auto highDpi = resolve();
    Check(highDpi.bitmap->GetPixelSize().width == 64,
        "high DPI uses enough physical source pixels");
    graphics.context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    Check(resolve().bitmap->GetPixelSize().width == 32, "pixel unit mode ignores DPI scaling");
    graphics.context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    graphics.context->SetDpi(96, 96);
    graphics.context->SetTransform(D2D1::Matrix3x2F::Scale(2, 3));
    const auto scaled = resolve();
    Check(scaled.bitmap->GetPixelSize().width == 64 && scaled.bitmap->GetPixelSize().height == 96,
        "transforms select independent physical axis resolutions");
    graphics.context->SetTransform(D2D1::Matrix3x2F::Rotation(90));
    Check(resolve().bitmap->GetPixelSize().width == 32, "rotation retains the physical resolution");
    graphics.context->SetTransform(D2D1::Matrix3x2F::Identity());

    auto colors = graphics.Source(false, true);
    auto crop = cache.Resolve(graphics.context.Get(), colors.Get(), destination,
        D2D1::RectF(256, 0, 512, 512));
    const auto cropped = graphics.Pixels(crop);
    const auto middle = (16 * 32 + 16) * 4;
    Check(cropped[middle] < 4 && cropped[middle + 1] > 250 && cropped[middle + 2] < 4,
        "cropped source coordinates continue selecting the green half");
    auto transparent = graphics.Source(true);
    auto alpha = cache.Resolve(graphics.context.Get(), transparent.Get(), destination, full);
    AreaAverage(graphics.Pixels(alpha), 32);
    const auto faded = graphics.Pixels(alpha, 0.5f);
    Check(std::abs(int(faded[middle]) - 16) <= 3 &&
        std::abs(int(faded[middle + 3]) - 16) <= 3, "opacity preserves premultiplied transparent edges");

    Check(cache.Contains(first.bitmap.Get()), "finished derived images can enter a background signature");
    cache.Remove(source.Get());
    Check(!cache.Contains(first.bitmap.Get()) && resolve().outcome == Cache::Outcome::Miss,
        "source release invalidates all dependent sizes");
    for (int side = 100; side < 140; ++side)
        cache.Resolve(graphics.context.Get(), source.Get(), D2D1::RectF(0, 0,
            static_cast<float>(side), static_cast<float>(side)), full);
    Check(cache.Size() <= 32 && cache.RetainedBytes() <= 16 * 1024 * 1024,
        "continuous resizing keeps retained textures bounded");
    Check(resolve().outcome == Cache::Outcome::Miss, "least recently used variants are evicted");
    Check(cache.Resolve(graphics.context.Get(), source.Get(),
        D2D1::RectF(0, 0, 1024, 1024), full).bitmap.Get() == source.Get(),
        "upscaling does not allocate a reduced image");
    Check(cache.Resolve(graphics.context.Get(), source.Get(), destination,
        D2D1::RectF(0, 0, 0, 0)).outcome == Cache::Outcome::Bypass,
        "empty source rectangles bypass safely");
    auto large = graphics.Source(false, false, 2048);
    const auto largeRect = D2D1::RectF(0, 0, 2048, 2048);
    for (int side = 1018; side < 1024; ++side)
        cache.Resolve(graphics.context.Get(), large.Get(), D2D1::RectF(0, 0,
            static_cast<float>(side), static_cast<float>(side)), largeRect);
    Check(cache.Size() < 6 && cache.RetainedBytes() <= 16 * 1024 * 1024,
        "byte pressure evicts textures before the entry count limit");
    const auto oversized = cache.Resolve(graphics.context.Get(), large.Get(),
        D2D1::RectF(0, 0, 2047, 2047), largeRect);
    Check(oversized.bitmap.Get() == large.Get() &&
        oversized.interpolation == D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        "oversized variants retain a high-quality direct fallback without allocation");
    AreaAverage(graphics.Pixels(oversized), 255);
    Graphics replacement;
    auto replacementSource = replacement.Source();
    cache.Resolve(replacement.context.Get(), replacementSource.Get(), destination, full);
    Check(cache.Size() == 1 && !cache.Contains(first.bitmap.Get()),
        "device reconstruction drops old textures and raster context");
    cache.Clear();
    Check(cache.Size() == 0 && cache.RetainedBytes() == 0, "clear releases derived textures");
}
