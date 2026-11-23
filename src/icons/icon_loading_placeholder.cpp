#include "icon_loading_placeholder.h"
#include "icon_bitmap_renderer.h"
#include "icon_image_codec.h"

#include <algorithm>
#include <cmath>

namespace snowdesktop::icon_loading_placeholder
{
namespace
{
const icon_image::Image& Source()
{
    // Rasterize the static mark once, without a Shell request or an external asset.
    static const auto source = [] {
        constexpr int size = 128;
        constexpr float radius = 0.195f;
        constexpr float halfStroke = 0.023f;
        constexpr std::uint32_t gray = 148;
        icon_image::Image image;
        image.width = image.height = size;
        image.pixels.resize(size * size);
        for (int row = 0; row < size; ++row)
        {
            for (int column = 0; column < size; ++column)
            {
                const float x = (column + 0.5f) / size - 0.5f;
                const float y = (row + 0.5f) / size - 0.5f;
                float arcDistance = std::abs(std::sqrt(x * x + y * y) - radius);
                if (x > 0.0f && y < 0.0f)
                {
                    // Round caps at twelve and three o'clock leave the leading gap open.
                    const float top = y + radius;
                    const float right = x - radius;
                    arcDistance = std::sqrt(std::min(x * x + top * top, right * right + y * y));
                }

                const float coverage = std::clamp((halfStroke - arcDistance) * size + 0.5f, 0.0f, 1.0f);
                const auto alpha = static_cast<std::uint32_t>(std::lround(coverage * 240.0f));
                const std::uint32_t channel = (gray * alpha + 127u) / 255u;
                image.pixels[static_cast<size_t>(row) * size + column] =
                    (alpha << 24) | (channel << 16) | (channel << 8) | channel;
            }
        }
        return image;
    }();
    return source;
}
}

HBITMAP SourceBitmap()
{
    struct Bitmap
    {
        HBITMAP handle = icon_image::CreateBitmap(Source());
        ~Bitmap() { if (handle) DeleteObject(handle); }
    };
    static const Bitmap bitmap;
    return bitmap.handle;
}

void DrawUnstyled(ID2D1RenderTarget* target, RECT bounds, float opacity)
{
    if (!target || IsRectEmpty(&bounds) || opacity <= 0.0f) return;
    const auto& source = Source();
    const auto properties = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    const auto size = D2D1::SizeU(static_cast<UINT32>(source.width), static_cast<UINT32>(source.height));
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    if (SUCCEEDED(target->CreateBitmap(size, source.pixels.data(),
            static_cast<UINT32>(source.width * sizeof(std::uint32_t)), properties, &bitmap)))
    {
        const auto plan = icon_bitmap_renderer::Prepare(source.width, source.height, bounds);
        icon_bitmap_renderer::Draw(target, bitmap.Get(), plan, opacity, true);
    }
}
}
