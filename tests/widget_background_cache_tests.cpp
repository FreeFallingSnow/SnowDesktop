#include "widget_background_cache.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1effects.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Cache = snowdesktop::widget_runtime::WidgetBackgroundCache;
void Check(bool value, const char* message)
{
    if (value) return;
    std::cerr << "FAILED: background cache: " << message << '\n';
    std::exit(1);
}
void Ok(HRESULT hr, const char* message)
{
    if (SUCCEEDED(hr)) return;
    std::cerr << "FAILED: background cache: " << message << " (0x" << std::hex
        << static_cast<unsigned long>(hr) << ")\n";
    std::exit(1);
}
struct Graphics
{
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1Device> device;
    ComPtr<ID2D1DeviceContext> context, recording;
    Graphics()
    {
        Ok(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr), "create software D3D device");
        ComPtr<IDXGIDevice> dxgi;
        Ok(d3d.As(&dxgi), "query DXGI device");
        Ok(D2D1CreateDevice(dxgi.Get(), nullptr, &device), "create D2D device");
        Ok(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context), "create render context");
        Ok(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &recording), "create recording context");
    }
    ComPtr<ID2D1Bitmap1> Cover(bool alternate)
    {
        std::array<std::uint32_t, 16 * 16> pixels;
        for (std::size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = i % 16 < 8 ? (alternate ? 0xff00cc44u : 0xffbb1133u) : 0xff2244aau;
        ComPtr<ID2D1Bitmap1> result;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        Ok(context->CreateBitmap(D2D1::SizeU(16, 16), pixels.data(), 64, properties, &result), "create immutable cover");
        return result;
    }
    ComPtr<ID2D1CommandList> Commands(ID2D1Bitmap* cover, float dpi,
        D2D1_UNIT_MODE unit, bool alternate = false, bool unsupported = false)
    {
        ComPtr<ID2D1CommandList> result;
        Ok(recording->CreateCommandList(&result), "create commands");
        recording->SetDpi(dpi, dpi);
        recording->SetUnitMode(unit);
        recording->SetTransform(D2D1::Matrix3x2F::Identity());
        recording->SetTarget(result.Get());
        recording->BeginDraw();
        recording->DrawBitmap(cover, D2D1::RectF(4, 6, 84, 70), 0.92f,
            D2D1_INTERPOLATION_MODE_LINEAR);
        const D2D1_GRADIENT_STOP stops[] = {
            { 0, D2D1::ColorF(alternate ? 0xccaa11u : 0x070a17u, 0.62f) },
            { 1, D2D1::ColorF(0x11152au, 0.62f) }
        };
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> brush;
        Ok(recording->CreateGradientStopCollection(stops, 2, &collection), "create gradient stops");
        Ok(recording->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(4, 6), D2D1::Point2F(84, 70)), collection.Get(), &brush), "create gradient");
        recording->FillRectangle(D2D1::RectF(4, 6, 84, 70), brush.Get());
        if (unsupported)
            recording->FillEllipse(D2D1::Ellipse(D2D1::Point2F(30, 30), 10, 10), brush.Get());
        Ok(recording->EndDraw(), "record background");
        recording->SetTarget(nullptr);
        Ok(result->Close(), "close commands");
        return result;
    }
    std::vector<std::uint8_t> Pixels(ID2D1CommandList* commands, ID2D1Bitmap1* cached,
        float dpi, D2D1_UNIT_MODE unit, float blurRadius, float opacity = 0.83f, float corner = 8)
    {
        ComPtr<ID2D1Bitmap1> target, cpu;
        const auto size = D2D1::SizeU(192, 144);
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &target), "create pixel target");
        context->SetDpi(dpi, dpi);
        context->SetUnitMode(unit);
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->SetTarget(target.Get());
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0x314159u));
        ComPtr<ID2D1Factory> factory;
        context->GetFactory(&factory);
        ComPtr<ID2D1RoundedRectangleGeometry> clip;
        const auto bounds = D2D1::RectF(4, 6, 84, 70);
        Ok(factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(bounds, corner, corner), &clip), "create final clip");
        context->PushLayer(D2D1::LayerParameters(bounds, clip.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Identity(), opacity), nullptr);
        if (cached) context->DrawImage(cached, D2D1::Point2F(4, 6));
        else
        {
            ComPtr<ID2D1Effect> blur;
            Ok(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur), "create reference blur");
            blur->SetInput(0, commands);
            Ok(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, blurRadius), "set reference blur");
            Ok(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED), "set reference optimization");
            Ok(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD), "set reference border");
            context->DrawImage(blur.Get());
        }
        context->PopLayer();
        Ok(context->EndDraw(), "render reference or cache pixels");
        context->SetTarget(nullptr);
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        Ok(context->CreateBitmap(size, nullptr, 0, properties, &cpu), "create readback");
        Ok(cpu->CopyFromBitmap(nullptr, target.Get(), nullptr), "copy readback");
        D2D1_MAPPED_RECT mapped{};
        Ok(cpu->Map(D2D1_MAP_OPTIONS_READ, &mapped), "map readback");
        std::vector<std::uint8_t> pixels(std::size_t(size.width) * size.height * 4);
        for (UINT32 row = 0; row < size.height; ++row)
            std::copy_n(mapped.bits + row * mapped.pitch, size.width * 4,
                pixels.data() + std::size_t(row) * size.width * 4);
        Ok(cpu->Unmap(), "unmap readback");
        return pixels;
    }
};
void SamePixels(const std::vector<std::uint8_t>& expected, const std::vector<std::uint8_t>& actual)
{
    Check(expected.size() == actual.size(), "pixel extents match");
    int maximum = 0;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < actual.size(); ++i)
    {
        const int delta = std::abs(int(expected[i]) - int(actual[i]));
        maximum = std::max(maximum, delta);
        changed += delta != 0;
    }
    if (maximum > 1)
        std::cerr << "pixel difference: max=" << maximum << ", changed bytes=" << changed << '\n';
    Check(maximum <= 1, "cached blur preserves RGBA pixels within one 8-bit rounding step");
}
}

void RunWidgetBackgroundCacheTests()
{
    Graphics graphics;
    auto cover = graphics.Cover(false), otherCover = graphics.Cover(true);
    const auto immutable = [&](ID2D1Bitmap* bitmap) { return bitmap == cover.Get() || bitmap == otherCover.Get(); };
    const auto bounds = D2D1::RectF(4, 6, 84, 70);
    Cache cache;
    // Real D2D software rendering compares the finished pixels, including the
    // caller's rounded clip/opacity, instead of only testing a key comparison.
    for (const float dpi : { 96.0f, 144.0f, 192.0f })
    {
        for (const auto unit : { D2D1_UNIT_MODE_DIPS, D2D1_UNIT_MODE_PIXELS })
        {
            graphics.context->SetDpi(dpi, dpi);
            graphics.context->SetUnitMode(unit);
            auto commands = graphics.Commands(cover.Get(), dpi, unit);
            auto first = cache.Resolve(L"player", graphics.context.Get(), commands.Get(), bounds, 8, immutable);
            Check(first.bitmap && first.outcome == Cache::Outcome::Miss, "DPI/unit change regenerates the blur");
            auto repeated = graphics.Commands(cover.Get(), dpi, unit);
            auto second = cache.Resolve(L"player", graphics.context.Get(), repeated.Get(), bounds, 8, immutable);
            Check(second.bitmap && second.outcome == Cache::Outcome::Hit, "equivalent new brushes and commands reuse pixels");
            SamePixels(graphics.Pixels(repeated.Get(), nullptr, dpi, unit, 8),
                graphics.Pixels(repeated.Get(), second.bitmap.Get(), dpi, unit, 8));
            SamePixels(graphics.Pixels(repeated.Get(), nullptr, dpi, unit, 8, 0.41f, 18),
                graphics.Pixels(repeated.Get(), second.bitmap.Get(), dpi, unit, 8, 0.41f, 18));
        }
    }
    graphics.context->SetDpi(96, 96);
    graphics.context->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    auto commands = graphics.Commands(cover.Get(), 96, D2D1_UNIT_MODE_DIPS);
    auto original = cache.Resolve(L"player", graphics.context.Get(), commands.Get(), bounds, 8, immutable);
    auto recolored = graphics.Commands(cover.Get(), 96, D2D1_UNIT_MODE_DIPS, true);
    auto recoloredResult = cache.Resolve(L"player", graphics.context.Get(), recolored.Get(), bounds, 8, immutable);
    Check(recoloredResult.outcome == Cache::Outcome::Miss,
        "changing only gradient colors must invalidate the same artwork");
    SamePixels(graphics.Pixels(recolored.Get(), nullptr, 96, D2D1_UNIT_MODE_DIPS, 8),
        graphics.Pixels(recolored.Get(), recoloredResult.bitmap.Get(), 96, D2D1_UNIT_MODE_DIPS, 8));
    auto altered = graphics.Commands(otherCover.Get(), 96, D2D1_UNIT_MODE_DIPS, true);
    auto updated = cache.Resolve(L"player", graphics.context.Get(), altered.Get(), bounds, 8, immutable);
    Check(updated.outcome == Cache::Outcome::Miss && updated.bitmap.Get() != original.bitmap.Get(),
        "changing only artwork cannot reuse stale background pixels");
    SamePixels(graphics.Pixels(altered.Get(), nullptr, 96, D2D1_UNIT_MODE_DIPS, 8),
        graphics.Pixels(altered.Get(), updated.bitmap.Get(), 96, D2D1_UNIT_MODE_DIPS, 8));
    auto blurChanged = cache.Resolve(L"player", graphics.context.Get(), altered.Get(), bounds, 4, immutable);
    Check(blurChanged.outcome == Cache::Outcome::Miss, "blur radius change rebuilds");
    SamePixels(graphics.Pixels(altered.Get(), nullptr, 96, D2D1_UNIT_MODE_DIPS, 4),
        graphics.Pixels(altered.Get(), blurChanged.bitmap.Get(), 96, D2D1_UNIT_MODE_DIPS, 4));
    auto resized = cache.Resolve(L"player", graphics.context.Get(), altered.Get(),
        D2D1::RectF(4, 6, 100, 86), 4, immutable);
    Check(resized.outcome == Cache::Outcome::Miss && resized.bitmap &&
        resized.bitmap->GetPixelSize().width == 96 && resized.bitmap->GetPixelSize().height == 80,
        "resizing an existing widget rebuilds its full pixel extent");
    Check(!cache.Resolve(L"player", graphics.context.Get(), altered.Get(), bounds, 0, immutable).bitmap && cache.Size() == 0,
        "disabling blur releases the previous cache");
    Check(!cache.Resolve(L"untrusted", graphics.context.Get(), altered.Get(), bounds, 8,
        [](ID2D1Bitmap*) { return false; }).bitmap, "mutable or unrecognized images bypass caching");
    auto complex = graphics.Commands(cover.Get(), 96, D2D1_UNIT_MODE_DIPS, false, true);
    Check(!cache.Resolve(L"complex", graphics.context.Get(), complex.Get(), bounds, 8, immutable).bitmap,
        "unsupported geometry must fall back rather than partially compare commands");
    graphics.context->SetTransform(D2D1::Matrix3x2F::Scale(1.1f, 1.1f));
    Check(!cache.Resolve(L"scaled", graphics.context.Get(), commands.Get(), bounds, 8, immutable).bitmap,
        "transforms that change rasterization must fall back");
    graphics.context->SetTransform(D2D1::Matrix3x2F::Identity());
    for (int i = 0; i < 40; ++i)
    {
        Check(cache.Resolve(L"small-" + std::to_wstring(i), graphics.context.Get(), commands.Get(), bounds, 8, immutable).bitmap != nullptr,
            "bounded cache admits small backgrounds");
        Check(cache.Size() <= Cache::MaximumEntries && cache.RetainedBytes() <= Cache::MaximumBytes,
            "entry count and retained memory remain bounded");
    }
    for (int i = 0; i < 12; ++i)
    {
        Check(cache.Resolve(L"large-" + std::to_wstring(i), graphics.context.Get(), commands.Get(),
            D2D1::RectF(0, 0, 1024, 1024), 8, immutable).bitmap != nullptr, "large backgrounds admit within budget");
        Check(cache.RetainedBytes() <= Cache::MaximumBytes, "large backgrounds evict old retained storage");
    }
    Check(!cache.Resolve(L"oversized", graphics.context.Get(), commands.Get(),
        D2D1::RectF(0, 0, 2048, 2048), 8, immutable).bitmap, "oversized backgrounds do not allocate cached pixels");
    cache.Erase(L"large-11");
    Check(cache.Resolve(L"large-11", graphics.context.Get(), commands.Get(),
        D2D1::RectF(0, 0, 1024, 1024), 8, immutable).outcome == Cache::Outcome::Miss,
        "hide/unload eviction requires fresh pixels");
    cache.Clear();
    Check(cache.Size() == 0 && cache.RetainedBytes() == 0, "device reset or shutdown releases all retained entries");
    Graphics otherDevice;
    auto replacementCover = otherDevice.Cover(false);
    auto replacementCommands = otherDevice.Commands(replacementCover.Get(), 96, D2D1_UNIT_MODE_DIPS);
    (void)cache.Resolve(L"old-device", graphics.context.Get(), commands.Get(), bounds, 8, immutable);
    Check(cache.Resolve(L"new-device", otherDevice.context.Get(), replacementCommands.Get(), bounds, 8,
        [&](ID2D1Bitmap* bitmap) { return bitmap == replacementCover.Get(); }).outcome == Cache::Outcome::Miss && cache.Size() == 1,
        "switching Direct2D devices cannot retain old device resources");
    std::cout << "widget background cache pixel and invalidation tests passed\n";
}
