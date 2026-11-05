#include "widget_image_sampling.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::size_t MaxEntries = 32;
constexpr std::size_t MaxEntryBytes = 4 * 1024 * 1024;
constexpr std::size_t MaxBytes = 16 * 1024 * 1024;
}

WidgetImageSamplingCache::Result WidgetImageSamplingCache::Resolve(
    ID2D1DeviceContext* context, ID2D1Bitmap1* bitmap,
    const D2D1_RECT_F& destination, const D2D1_RECT_F& source,
    bool nearest) noexcept
{
    Result result;
    result.bitmap = bitmap;
    result.source = source;
    if (nearest) result.interpolation = D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    if (!context || !bitmap) return result;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    context->GetDevice(&device);
    if (device_.Get() != device.Get())
    {
        Clear();
        device_ = device;
    }
    if (nearest || !device) return result;
    const auto size = bitmap->GetSize();
    const auto pixels = bitmap->GetPixelSize();
    const double sourceWidth = double(source.right) - source.left;
    const double sourceHeight = double(source.bottom) - source.top;
    const double width = double(destination.right) - destination.left;
    const double height = double(destination.bottom) - destination.top;
    D2D1_MATRIX_3X2_F transform{};
    context->GetTransform(&transform);
    float dpiX = 96, dpiY = 96;
    if (context->GetUnitMode() == D2D1_UNIT_MODE_DIPS)
        context->GetDpi(&dpiX, &dpiY);
    const double scaleX = std::hypot(double(transform._11) * dpiX / 96,
        double(transform._12) * dpiY / 96);
    const double scaleY = std::hypot(double(transform._21) * dpiX / 96,
        double(transform._22) * dpiY / 96);
    const double requiredWidth = size.width * width / sourceWidth * scaleX;
    const double requiredHeight = size.height * height / sourceHeight * scaleY;
    if (!(sourceWidth > 0 && sourceHeight > 0 && width > 0 && height > 0 &&
          size.width > 0 && size.height > 0 && pixels.width && pixels.height &&
          std::isfinite(requiredWidth) && std::isfinite(requiredHeight) &&
          requiredWidth > 0 && requiredHeight > 0)) return result;
    // Ignore float transform roundoff at exact rotations/integer scales.
    const auto targetWidth = static_cast<UINT32>(std::ceil(
        std::clamp(requiredWidth * (1 - 1e-6), 1.0, double(pixels.width))));
    const auto targetHeight = static_cast<UINT32>(std::ceil(
        std::clamp(requiredHeight * (1 - 1e-6), 1.0, double(pixels.height))));
    if (targetWidth == pixels.width && targetHeight == pixels.height) return result;

    // Even a budget/allocation failure keeps the prefiltered direct fallback.
    result.interpolation = D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;
    const auto use = [&](const Entry& entry, Outcome outcome) {
        result.bitmap = entry.bitmap;
        const auto reduced = entry.bitmap->GetSize();
        result.source = D2D1::RectF(source.left / size.width * reduced.width,
            source.top / size.height * reduced.height,
            source.right / size.width * reduced.width,
            source.bottom / size.height * reduced.height);
        result.interpolation = D2D1_INTERPOLATION_MODE_LINEAR;
        result.outcome = outcome;
    };
    for (auto& entry : entries_)
    {
        if (entry.source.Get() == bitmap && entry.width == targetWidth &&
            entry.height == targetHeight)
        {
            entry.used = ++clock_;
            use(entry, Outcome::Hit);
            return result;
        }
    }
    const std::uint64_t pixelCount = std::uint64_t(targetWidth) * targetHeight;
    if (pixelCount > MaxEntryBytes / 4) return result;
    const auto byteCount = pixelCount * 4;
    try
    {
        if (!raster_ && FAILED(device->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &raster_))) return result;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> reduced;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(raster_->CreateBitmap(D2D1::SizeU(targetWidth, targetHeight),
                nullptr, 0, properties, &reduced))) return result;
        // A separate context never interrupts the caller's active BeginDraw or
        // command-list recording. HQ cubic prefilters a shrinking transform.
        raster_->SetDpi(96, 96);
        raster_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
        raster_->SetTransform(D2D1::Matrix3x2F::Scale(
            targetWidth / size.width, targetHeight / size.height));
        raster_->SetTarget(reduced.Get());
        raster_->BeginDraw();
        raster_->Clear(D2D1::ColorF(0, 0.0f));
        raster_->DrawBitmap(bitmap, D2D1::RectF(0, 0, size.width, size.height), 1,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        const HRESULT rendered = raster_->EndDraw();
        raster_->SetTarget(nullptr);
        if (FAILED(rendered))
        {
            raster_.Reset();
            return result;
        }
        const auto bytes = static_cast<std::size_t>(byteCount);
        while (!entries_.empty() &&
            (entries_.size() >= MaxEntries || bytes_ + bytes > MaxBytes))
        {
            const auto victim = std::min_element(entries_.begin(), entries_.end(),
                [](const Entry& a, const Entry& b) { return a.used < b.used; });
            bytes_ -= victim->bytes;
            entries_.erase(victim);
        }
        entries_.push_back({ bitmap, reduced, targetWidth, targetHeight, ++clock_, bytes });
        bytes_ += bytes;
        use(entries_.back(), Outcome::Miss);
    }
    catch (...)
    {
        // A cache is optional; allocation failure cannot escape a Lua draw call.
    }
    return result;
}

void WidgetImageSamplingCache::Remove(ID2D1Bitmap* source) noexcept
{
    for (auto entry = entries_.begin(); entry != entries_.end();)
    {
        if (entry->source.Get() != source) { ++entry; continue; }
        bytes_ -= entry->bytes;
        entry = entries_.erase(entry);
    }
}

void WidgetImageSamplingCache::Clear() noexcept
{
    entries_.clear();
    roundedBrush_.Reset();
    roundedGeometry_.Reset();
    raster_.Reset();
    device_.Reset();
    bytes_ = 0;
    clock_ = 0;
}

bool WidgetImageSamplingCache::DrawRounded(ID2D1DeviceContext* context,
    ID2D1Bitmap1* bitmap, const D2D1_RECT_F& destination,
    const D2D1_RECT_F& source, float radius, float opacity,
    D2D1_INTERPOLATION_MODE interpolation) noexcept
{
    if (!context || !bitmap || !(radius > 0) || !std::isfinite(radius) ||
        !std::isfinite(opacity) || opacity < 0 || opacity > 1 ||
        context->GetPrimitiveBlend() != D2D1_PRIMITIVE_BLEND_SOURCE_OVER ||
        (interpolation != D2D1_INTERPOLATION_MODE_LINEAR &&
            interpolation != D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR)) return false;
    const float width = destination.right - destination.left;
    const float height = destination.bottom - destination.top;
    const float sourceWidth = source.right - source.left;
    const float sourceHeight = source.bottom - source.top;
    const auto size = bitmap->GetSize();
    if (!(width > 0 && height > 0 && sourceWidth > 0 && sourceHeight > 0) ||
        !std::isfinite(width) || !std::isfinite(height) ||
        !std::isfinite(destination.left) || !std::isfinite(destination.top) ||
        source.left < 0 || source.top < 0 || source.right > size.width ||
        source.bottom > size.height || !std::isfinite(sourceWidth) ||
        !std::isfinite(sourceHeight)) return false;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    context->GetDevice(&device);
    if (!device) return false;
    if (device_.Get() != device.Get()) { Clear(); device_ = device; }
    const float sx = width / sourceWidth, sy = height / sourceHeight;
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
    radius = std::min(radius, std::min(width, height) * 0.5f);
    if (!roundedGeometry_ || roundedShape_.radiusX != radius ||
        roundedShape_.rect.left != destination.left || roundedShape_.rect.top != destination.top ||
        roundedShape_.rect.right != destination.right || roundedShape_.rect.bottom != destination.bottom)
    {
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        context->GetFactory(&factory);
        const auto shape = D2D1::RoundedRect(destination, radius, radius);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
        if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(shape, &geometry))) return false;
        roundedGeometry_ = std::move(geometry);
        roundedShape_ = shape;
    }
    if (!roundedBrush_)
    {
        const D2D1_BITMAP_BRUSH_PROPERTIES1 properties{
            D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, interpolation };
        if (FAILED(context->CreateBitmapBrush(bitmap, &properties,
                nullptr, &roundedBrush_))) return false;
    }
    roundedBrush_->SetBitmap(bitmap);
    roundedBrush_->SetOpacity(opacity);
    roundedBrush_->SetInterpolationMode1(interpolation);
    roundedBrush_->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) *
        D2D1::Matrix3x2F::Translation(destination.left - source.left * sx,
            destination.top - source.top * sy));
    const auto antialias = context->GetAntialiasMode();
    context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    context->FillGeometry(roundedGeometry_.Get(), roundedBrush_.Get());
    context->SetAntialiasMode(antialias);
    // Command lists retain their recorded bitmap; the reusable brush must
    // not extend the lifetime of a source evicted from the image caches.
    roundedBrush_->SetBitmap(nullptr);
    return true;
}

bool WidgetImageSamplingCache::Contains(ID2D1Bitmap* bitmap) const noexcept
{
    return std::any_of(entries_.begin(), entries_.end(),
        [bitmap](const Entry& entry) { return entry.bitmap.Get() == bitmap; });
}
}
