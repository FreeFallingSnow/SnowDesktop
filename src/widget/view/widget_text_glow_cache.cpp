#include "widget_text_glow_cache.h"

#include <d2d1_1helper.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
using Microsoft::WRL::ComPtr;

constexpr std::array<D2D1_POINT_2F, 8> GlowOffsets{{
    {1, 0}, {-1, 0}, {0, 1}, {0, -1},
    {0.707f, 0.707f}, {-0.707f, 0.707f},
    {0.707f, -0.707f}, {-0.707f, -0.707f},
}};
constexpr float RasterScale = 2.0f;
constexpr float MaximumExtent = 512.0f;
constexpr std::size_t MaximumEntries = 128;
constexpr std::size_t MaximumBytes = 16 * 1024 * 1024;

bool SameColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) noexcept
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

void DrawCopies(ID2D1DeviceContext& context, IDWriteTextLayout& layout,
    ID2D1Brush& brush, D2D1_POINT_2F origin, float radius)
{
    for (const auto offset : GlowOffsets)
        context.DrawTextLayout(D2D1::Point2F(origin.x + offset.x * radius,
            origin.y + offset.y * radius), &layout, &brush);
}

void DrawCached(ID2D1DeviceContext& context, ID2D1Bitmap1& bitmap,
    const D2D1_RECT_F& bounds, D2D1_POINT_2F origin)
{
    const auto destination = D2D1::RectF(origin.x + bounds.left,
        origin.y + bounds.top, origin.x + bounds.right, origin.y + bounds.bottom);
    context.DrawBitmap(&bitmap, &destination, 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
}
}

void WidgetTextGlowCache::Clear() noexcept
{
    entries_.clear();
    retainedBytes_ = 0;
    sequence_ = 0;
    raster_.Reset();
    device_.Reset();
}

bool WidgetTextGlowCache::Rasterize(IDWriteTextLayout& layout, Entry& entry)
{
    DWRITE_OVERHANG_METRICS ink{};
    if (FAILED(layout.GetOverhangMetrics(&ink))) return false;
    // Leave room for grid fitting and bilinear sampling around all eight copies.
    const float padding = entry.radius + 2.0f;
    entry.bounds = D2D1::RectF(std::floor(-ink.left - padding),
        std::floor(-ink.top - padding),
        std::ceil(layout.GetMaxWidth() + ink.right + padding),
        std::ceil(layout.GetMaxHeight() + ink.bottom + padding));
    const float width = (entry.bounds.right - entry.bounds.left) * RasterScale;
    const float height = (entry.bounds.bottom - entry.bounds.top) * RasterScale;
    if (!(width > 0.0f && height > 0.0f && width <= MaximumExtent &&
            height <= MaximumExtent)) return false;
    if (!raster_ && FAILED(device_->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &raster_))) return false;

    const auto size = D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(height));
    const float dpi = 96.0f * RasterScale;
    const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(raster_->CreateBitmap(size, nullptr, 0, properties, &entry.bitmap)) ||
        FAILED(raster_->CreateSolidColorBrush(entry.color, &brush))) return false;

    raster_->SetUnitMode(D2D1_UNIT_MODE_DIPS);
    raster_->SetDpi(dpi, dpi);
    raster_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    raster_->SetTarget(entry.bitmap.Get());
    raster_->BeginDraw();
    raster_->Clear(D2D1::ColorF(0, 0.0f));
    DrawCopies(*raster_.Get(), layout, *brush.Get(),
        D2D1::Point2F(-entry.bounds.left, -entry.bounds.top), entry.radius);
    const HRESULT result = raster_->EndDraw();
    raster_->SetTarget(nullptr);
    if (FAILED(result)) return false;
    entry.layout = &layout;
    entry.bytes = static_cast<std::size_t>(size.width) * size.height * 4;
    return true;
}

void WidgetTextGlowCache::Draw(ID2D1DeviceContext& context, IDWriteTextLayout& layout,
    ID2D1SolidColorBrush& brush, D2D1_POINT_2F origin, float radius)
{
    D2D1_COLOR_F color = brush.GetColor();
    color.a *= brush.GetOpacity();
    if (color.a <= 0.0f) return;

    ComPtr<ID2D1Device> device;
    context.GetDevice(&device);
    if (device.Get() != device_.Get())
    {
        Clear();
        device_ = std::move(device);
    }
    for (auto& entry : entries_)
    {
        if (entry.layout.Get() != &layout || entry.radius != radius ||
            !SameColor(entry.color, color)) continue;
        entry.used = ++sequence_;
        DrawCached(context, *entry.bitmap.Get(), entry.bounds, origin);
        return;
    }

    Entry entry;
    entry.radius = radius;
    entry.color = color;
    if (!device_ || !Rasterize(layout, entry))
    {
        DrawCopies(context, layout, brush, origin, radius);
        return;
    }
    DrawCached(context, *entry.bitmap.Get(), entry.bounds, origin);
    while (entries_.size() >= MaximumEntries || retainedBytes_ + entry.bytes > MaximumBytes)
    {
        const auto oldest = std::min_element(entries_.begin(), entries_.end(),
            [](const Entry& a, const Entry& b) { return a.used < b.used; });
        retainedBytes_ -= oldest->bytes;
        entries_.erase(oldest);
    }
    entry.used = ++sequence_;
    try
    {
        entries_.push_back(std::move(entry));
        retainedBytes_ += entries_.back().bytes;
    }
    catch (...)
    {
        // The bitmap has already been drawn even if bookkeeping fails.
    }
}
}
