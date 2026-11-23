#include "icon_loading_placeholder.h"
#include "icon_bitmap_renderer.h"
#include "icon_image_codec.h"

#include <algorithm>

namespace snowdesktop::icon_loading_placeholder
{
namespace
{
struct RoundedRect
{
    float left, top, right, bottom, radius;

    bool Contains(float x, float y) const
    {
        const float dx = x - std::clamp(x, left + radius, right - radius);
        const float dy = y - std::clamp(y, top + radius, bottom - radius);
        return dx * dx + dy * dy <= radius * radius;
    }
};

unsigned SampleOpacity(float x, float y)
{
    constexpr RoundedRect back{24.0f, 12.0f, 50.0f, 43.0f, 4.0f};
    constexpr RoundedRect front{14.0f, 20.0f, 41.0f, 51.0f, 4.0f};
    constexpr RoundedRect upperSlot{19.0f, 27.0f, 33.0f, 30.0f, 1.5f};
    constexpr RoundedRect lowerSlot{19.0f, 34.0f, 28.0f, 37.0f, 1.5f};
    if (front.Contains(x, y))
        return upperSlot.Contains(x, y) || lowerSlot.Contains(x, y) ? 0u : 224u;
    return back.Contains(x, y) ? 112u : 0u;
}

icon_image::Image MakeSource(float scale)
{
    constexpr int size = 128;
    constexpr int samplesPerAxis = 2;
    constexpr unsigned sampleCount = samplesPerAxis * samplesPerAxis;
    constexpr std::uint32_t gray = 154;
    const float unitsPerPixel = 64.0f / (size * scale);
    const auto coordinate = [&](int pixel, int sample) {
        return 32.0f + (pixel + (sample + 0.5f) / samplesPerAxis - size * 0.5f) * unitsPerPixel;
    };

    icon_image::Image image;
    image.width = image.height = size;
    image.pixels.resize(size * size);
    for (int row = 0; row < size; ++row)
    {
        for (int column = 0; column < size; ++column)
        {
            unsigned opacity = 0;
            for (int subRow = 0; subRow < samplesPerAxis; ++subRow)
            {
                const float y = coordinate(row, subRow);
                for (int subColumn = 0; subColumn < samplesPerAxis; ++subColumn)
                    opacity += SampleOpacity(coordinate(column, subColumn), y);
            }
            const std::uint32_t alpha = (opacity + sampleCount / 2) / sampleCount;
            const std::uint32_t channel = (gray * alpha + 127u) / 255u;
            image.pixels[static_cast<size_t>(row) * size + column] =
                (alpha << 24) | (channel << 16) | (channel << 8) | channel;
        }
    }
    return image;
}

const icon_image::Image& Source(bool beautified)
{
    if (beautified)
    {
        static const auto inset = MakeSource(0.85f);
        return inset;
    }
    static const auto standalone = MakeSource(1.0f);
    return standalone;
}
}

HBITMAP SourceBitmap(bool beautified)
{
    struct Bitmap
    {
        explicit Bitmap(bool beautified) : handle(icon_image::CreateBitmap(Source(beautified))) {}
        ~Bitmap() { if (handle) DeleteObject(handle); }
        HBITMAP handle;
    };
    if (beautified)
    {
        static const Bitmap inset(true);
        return inset.handle;
    }
    static const Bitmap standalone(false);
    return standalone.handle;
}

void DrawUnstyled(ID2D1RenderTarget* target, RECT bounds, float opacity)
{
    if (!target || IsRectEmpty(&bounds) || opacity <= 0.0f) return;
    const auto& source = Source(false);
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
