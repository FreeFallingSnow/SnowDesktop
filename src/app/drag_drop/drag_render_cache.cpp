#include "app/render/drag_render_cache.h"
#include "desktop/display_color_mode.h"
#include "ui/render/highlight_coverage.h"
#include <d2d1_1helper.h>

// Cached off-screen surface used while rendering a drag preview.

void DragRenderCache::Reset()
{
    bitmap_.Reset();
    highlightMaterials_.Reset();
    highlightCoverage_.Reset();
    width_ = 0;
    height_ = 0;
    revision_ = 0;
    separatedHighlights_ = false;
    if (context_)
        context_->SetTarget(nullptr);
    context_.Reset();
    device_.Reset();
    contentValid_ = false;
}

void DragRenderCache::Invalidate()
{
    contentValid_ = false;
    // One retained allocation per cache; never retain an oversized desktop.
    const auto bytesPerPixel = highlightMaterials_ ? 9 : 4;
    if (static_cast<std::uint64_t>(width_) * height_ * bytesPerPixel > 16 * 1024 * 1024)
        Reset();
}

bool DragRenderCache::Ensure(ID2D1Device* device, D2D1_SIZE_U pixelSize,
    std::uint64_t revision, const std::function<void(ID2D1DeviceContext*)>& drawStatic)
{
    if (!device || pixelSize.width == 0 || pixelSize.height == 0 || !drawStatic)
        return false;

    if (device_.Get() != device) Reset();
    const bool separateHighlights = snowdesktop::display_color::UseHighlightCompensationOnDesktop();
    if (contentValid_ && bitmap_ && width_ == pixelSize.width && height_ == pixelSize.height &&
        revision_ == revision && separatedHighlights_ == separateHighlights)
        return true;

    if (!context_)
    {
        HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context_);
        if (FAILED(hr) || !context_)
            return false;
        device_ = device;
    }

    contentValid_ = false;
    highlightMaterials_.Reset();
    highlightCoverage_.Reset();
    if (!bitmap_ || width_ != pixelSize.width || height_ != pixelSize.height)
    {
        bitmap_.Reset();
        const auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT hr = context_->CreateBitmap(pixelSize, nullptr, 0, &bp, &bitmap_);
        if (FAILED(hr) || !bitmap_) return false;
        ++bitmapAllocationCount_;
    }

    context_->SetTarget(bitmap_.Get());
    context_->SetDpi(96.0f, 96.0f);
    context_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    context_->BeginDraw();
    context_->Clear(D2D1::ColorF(0, 0, 0, 0));
    snowdesktop::HighlightCoverage coverage;
    {
        snowdesktop::HighlightCoverage::Scope capture(context_.Get(), coverage, separateHighlights);
        drawStatic(context_.Get());
    }
    const HRESULT hr = context_->EndDraw();
    context_->SetTarget(nullptr);
    if (FAILED(hr))
    {
        bitmap_.Reset();
        return false;
    }
    if (!coverage.RenderMaterialSnapshot(device, pixelSize,
            highlightMaterials_.GetAddressOf(), highlightCoverage_.GetAddressOf()))
    {
        highlightMaterials_.Reset();
        highlightCoverage_.Reset();
        context_->SetTarget(bitmap_.Get());
        context_->BeginDraw();
        context_->SetTransform(D2D1::Matrix3x2F::Identity());
        context_->Clear(D2D1::ColorF(0, 0, 0, 0));
        drawStatic(context_.Get());
        const auto fallback = context_->EndDraw();
        context_->SetTarget(nullptr);
        if (FAILED(fallback))
        {
            bitmap_.Reset();
            return false;
        }
    }

    width_ = pixelSize.width;
    height_ = pixelSize.height;
    revision_ = revision;
    separatedHighlights_ = separateHighlights;
    contentValid_ = true;
    return true;
}

void DragRenderCache::Draw(ID2D1DeviceContext* ctx) const
{
    (void)DrawAt(ctx, {}, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

bool DragRenderCache::DrawAt(
    ID2D1DeviceContext* ctx,
    D2D1_POINT_2F destination,
    D2D1_INTERPOLATION_MODE interpolation) const
{
    if (!ctx || !bitmap_ || !contentValid_)
        return false;
    const D2D1_SIZE_F size = bitmap_->GetSize();
    const auto bounds = D2D1::RectF(destination.x, destination.y,
        destination.x + size.width, destination.y + size.height);
    ctx->DrawBitmap(
        bitmap_.Get(), bounds,
        1.0f,
        interpolation);
    if (highlightMaterials_ && !snowdesktop::HighlightCoverage::ExtractBitmap(
            ctx, highlightMaterials_.Get(), bounds, 1.f, highlightCoverage_.Get(), interpolation))
        ctx->DrawBitmap(highlightMaterials_.Get(), bounds, 1.f, interpolation);
    return true;
}
