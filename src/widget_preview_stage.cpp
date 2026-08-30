#include "widget_preview_stage.h"

#include <d2d1effects.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>

namespace snowdesktop::widget_preview
{
namespace
{
using Microsoft::WRL::ComPtr;

struct ScopedCom
{
    ScopedCom()
        : result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
    {
    }

    ~ScopedCom()
    {
        if (result == S_OK || result == S_FALSE)
            CoUninitialize();
    }

    HRESULT result = E_FAIL;
};

Wallpaper DecodeWallpaperFile(const std::filesystem::path& path)
{
    Wallpaper result;
    if (path.empty()) return result;

    ScopedCom com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return result;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr,
            GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        !decoder ||
        FAILED(decoder->GetFrame(0, &frame)) || !frame ||
        FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(),
            GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
            0.0, WICBitmapPaletteTypeCustom)))
    {
        return result;
    }
    UINT width = 0;
    UINT height = 0;
    if (FAILED(converter->GetSize(&width, &height)) ||
        width == 0 || height == 0 ||
        width > static_cast<UINT>(std::numeric_limits<int>::max()) ||
        height > static_cast<UINT>(std::numeric_limits<int>::max()))
    {
        return result;
    }
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.pixels.resize(static_cast<std::size_t>(width) * height);
    const UINT stride = width * sizeof(std::uint32_t);
    const std::size_t byteCount = result.pixels.size() *
        sizeof(std::uint32_t);
    if (byteCount > std::numeric_limits<UINT>::max() ||
        FAILED(converter->CopyPixels(nullptr, stride,
            static_cast<UINT>(byteCount),
            reinterpret_cast<BYTE*>(result.pixels.data()))))
    {
        return {};
    }
    // Preview PNGs are intentionally opaque. Composite transparent author
    // images over a restrained blue-gray instead of preserving alpha.
    constexpr std::uint8_t backdropB = 45;
    constexpr std::uint8_t backdropG = 39;
    constexpr std::uint8_t backdropR = 32;
    for (std::uint32_t& pixel : result.pixels)
    {
        const unsigned alpha = (pixel >> 24) & 0xffu;
        const auto composite = [alpha](unsigned foreground,
                                   unsigned background) {
            return (foreground * alpha + background * (255u - alpha) +
                127u) / 255u;
        };
        const unsigned blue = composite(pixel & 0xffu, backdropB);
        const unsigned green = composite((pixel >> 8) & 0xffu, backdropG);
        const unsigned red = composite((pixel >> 16) & 0xffu, backdropR);
        pixel = 0xff000000u | blue | (green << 8) | (red << 16);
    }
    return result;
}

Wallpaper DefaultWallpaperSource()
{
    Wallpaper result;
    result.width = 640;
    result.height = 360;
    result.pixels.resize(static_cast<std::size_t>(result.width) *
        result.height);
    for (int y = 0; y < result.height; ++y)
    {
        const float ny = static_cast<float>(y) /
            static_cast<float>(result.height - 1);
        for (int x = 0; x < result.width; ++x)
        {
            const float nx = static_cast<float>(x) /
                static_cast<float>(result.width - 1);
            const auto glow = [](float xValue, float yValue,
                                  float centerX, float centerY,
                                  float spread) {
                const float dx = xValue - centerX;
                const float dy = yValue - centerY;
                return std::exp(-(dx * dx + dy * dy) / spread);
            };
            const float blueGlow = glow(nx, ny, 0.20f, 0.18f, 0.15f);
            const float tealGlow = glow(nx, ny, 0.84f, 0.72f, 0.20f);
            const float warmGlow = glow(nx, ny, 0.72f, 0.12f, 0.10f);
            const auto channel = [](float value) {
                return static_cast<unsigned>(std::lround(
                    std::clamp(value, 0.0f, 255.0f)));
            };
            const unsigned red = channel(24.0f + 20.0f * blueGlow +
                14.0f * warmGlow + 4.0f * nx);
            const unsigned green = channel(30.0f + 34.0f * tealGlow +
                16.0f * blueGlow + 5.0f * (1.0f - ny));
            const unsigned blue = channel(42.0f + 54.0f * blueGlow +
                26.0f * tealGlow + 7.0f * warmGlow);
            result.pixels[static_cast<std::size_t>(y) * result.width + x] =
                0xff000000u | blue | (green << 8) | (red << 16);
        }
    }
    return result;
}

std::uint8_t Channel(std::uint32_t pixel, unsigned shift)
{
    return static_cast<std::uint8_t>((pixel >> shift) & 0xffu);
}

std::uint32_t SampleBilinear(
    const Wallpaper& source, float sourceX, float sourceY)
{
    sourceX = std::clamp(sourceX, 0.0f,
        static_cast<float>(source.width - 1));
    sourceY = std::clamp(sourceY, 0.0f,
        static_cast<float>(source.height - 1));
    const int x0 = static_cast<int>(std::floor(sourceX));
    const int y0 = static_cast<int>(std::floor(sourceY));
    const int x1 = std::min(x0 + 1, source.width - 1);
    const int y1 = std::min(y0 + 1, source.height - 1);
    const float tx = sourceX - static_cast<float>(x0);
    const float ty = sourceY - static_cast<float>(y0);
    const auto pixelAt = [&](int x, int y) {
        return source.pixels[static_cast<std::size_t>(y) *
            source.width + x];
    };
    const std::uint32_t samples[] = {
        pixelAt(x0, y0), pixelAt(x1, y0),
        pixelAt(x0, y1), pixelAt(x1, y1) };
    const float weights[] = {
        (1.0f - tx) * (1.0f - ty), tx * (1.0f - ty),
        (1.0f - tx) * ty, tx * ty };
    std::uint32_t result = 0xff000000u;
    for (const unsigned shift : { 0u, 8u, 16u })
    {
        float value = 0.0f;
        for (std::size_t index = 0; index < 4; ++index)
            value += static_cast<float>(Channel(samples[index], shift)) *
                weights[index];
        result |= static_cast<std::uint32_t>(
            std::lround(std::clamp(value, 0.0f, 255.0f))) << shift;
    }
    return result;
}

D2D1_RECT_F ToD2DRect(const RECT& rect)
{
    return D2D1::RectF(static_cast<float>(rect.left),
        static_cast<float>(rect.top), static_cast<float>(rect.right),
        static_cast<float>(rect.bottom));
}
}

Wallpaper LoadWallpaperImage(const std::filesystem::path& path)
{
    return DecodeWallpaperFile(path);
}

WallpaperPosition WallpaperPositionFromLegacySettings(
    int wallpaperStyle, bool tileWallpaper)
{
    if (tileWallpaper) return WallpaperPosition::Tile;
    switch (wallpaperStyle)
    {
    case 2:
        return WallpaperPosition::Stretch;
    case 6:
        return WallpaperPosition::Fit;
    case 10:
        return WallpaperPosition::Fill;
    case 22:
        return WallpaperPosition::Span;
    case 0:
    default:
        return WallpaperPosition::Center;
    }
}

std::uint64_t WallpaperFingerprint(const Wallpaper& wallpaper)
{
    if (wallpaper.width <= 0 || wallpaper.height <= 0 ||
        wallpaper.pixels.empty())
        return 0;
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(wallpaper.width));
    mix(static_cast<std::uint64_t>(wallpaper.height));
    const std::size_t step = std::max<std::size_t>(
        1, wallpaper.pixels.size() / 4096);
    for (std::size_t index = 0; index < wallpaper.pixels.size();
         index += step)
        mix(wallpaper.pixels[index]);
    mix(wallpaper.pixels.back());
    return hash;
}

bool WallpaperIsLight(const Wallpaper& wallpaper)
{
    if (wallpaper.pixels.empty()) return false;
    std::uint64_t luminance = 0;
    std::size_t samples = 0;
    const std::size_t step = std::max<std::size_t>(
        1, wallpaper.pixels.size() / 4096);
    for (std::size_t index = 0; index < wallpaper.pixels.size();
         index += step)
    {
        const std::uint32_t pixel = wallpaper.pixels[index];
        const unsigned blue = pixel & 0xffu;
        const unsigned green = (pixel >> 8) & 0xffu;
        const unsigned red = (pixel >> 16) & 0xffu;
        luminance += 54u * red + 183u * green + 19u * blue;
        ++samples;
    }
    return samples > 0 && luminance / samples > 150u * 256u;
}

Wallpaper GenerateWallpaper(int width, int height, bool lightTheme)
{
    return GenerateWallpaper(width, height, lightTheme, {});
}

Wallpaper GenerateWallpaper(int width, int height, bool lightTheme,
    const WallpaperViewport& viewport)
{
    (void)lightTheme;
    static const Wallpaper source = DefaultWallpaperSource();
    return GenerateWallpaper(source, width, height, viewport);
}

Wallpaper GenerateWallpaper(const Wallpaper& source, int width, int height,
    const WallpaperViewport& viewport)
{
    Wallpaper wallpaper;
    if (width <= 0 || height <= 0)
        return wallpaper;
    if (source.width <= 0 || source.height <= 0 || source.pixels.empty() ||
        source.pixels.size() < static_cast<std::size_t>(source.width) *
            source.height)
        return wallpaper;
    wallpaper.width = width;
    wallpaper.height = height;
    wallpaper.pixels.resize(static_cast<std::size_t>(width) * height);
    const int canvasWidth = viewport.canvasWidth > 0
        ? viewport.canvasWidth : width;
    const int canvasHeight = viewport.canvasHeight > 0
        ? viewport.canvasHeight : height;
    const float scale = std::max(
        static_cast<float>(canvasWidth) / source.width,
        static_cast<float>(canvasHeight) / source.height);
    const float visibleWidth = static_cast<float>(canvasWidth) / scale;
    const float visibleHeight = static_cast<float>(canvasHeight) / scale;
    const float sourceLeft =
        (static_cast<float>(source.width) - visibleWidth) * 0.5f;
    const float sourceTop =
        (static_cast<float>(source.height) - visibleHeight) * 0.5f;
    for (int y = 0; y < height; ++y)
    {
        const float normalizedY =
            (static_cast<float>(viewport.offsetY + y) + 0.5f) /
            static_cast<float>(canvasHeight);
        for (int x = 0; x < width; ++x)
        {
            const float normalizedX =
                (static_cast<float>(viewport.offsetX + x) + 0.5f) /
                static_cast<float>(canvasWidth);
            wallpaper.pixels[static_cast<std::size_t>(y) * width + x] =
                SampleBilinear(source,
                    sourceLeft + normalizedX * visibleWidth - 0.5f,
                    sourceTop + normalizedY * visibleHeight - 0.5f);
        }
    }
    return wallpaper;
}

Wallpaper RenderWallpaperRegion(const Wallpaper& source,
    const RECT& canvasBounds, const RECT& targetBounds,
    WallpaperPosition position, std::uint32_t backgroundColor)
{
    Wallpaper result;
    const int canvasWidth = canvasBounds.right - canvasBounds.left;
    const int canvasHeight = canvasBounds.bottom - canvasBounds.top;
    const int targetWidth = targetBounds.right - targetBounds.left;
    const int targetHeight = targetBounds.bottom - targetBounds.top;
    if (canvasWidth <= 0 || canvasHeight <= 0 || targetWidth <= 0 ||
        targetHeight <= 0 || source.width <= 0 || source.height <= 0 ||
        source.pixels.size() < static_cast<std::size_t>(source.width) *
            source.height)
    {
        return result;
    }

    result.width = targetWidth;
    result.height = targetHeight;
    result.pixels.assign(static_cast<std::size_t>(targetWidth) *
        targetHeight, backgroundColor | 0xff000000u);

    if (position == WallpaperPosition::Tile)
    {
        const auto positiveRemainder = [](int value, int divisor) {
            const int remainder = value % divisor;
            return remainder < 0 ? remainder + divisor : remainder;
        };
        for (int y = 0; y < targetHeight; ++y)
        {
            const int screenY = targetBounds.top + y;
            if (screenY < canvasBounds.top || screenY >= canvasBounds.bottom)
                continue;
            const int sourceY = positiveRemainder(
                screenY - canvasBounds.top, source.height);
            for (int x = 0; x < targetWidth; ++x)
            {
                const int screenX = targetBounds.left + x;
                if (screenX < canvasBounds.left ||
                    screenX >= canvasBounds.right)
                    continue;
                const int sourceX = positiveRemainder(
                    screenX - canvasBounds.left, source.width);
                result.pixels[static_cast<std::size_t>(y) * targetWidth + x] =
                    source.pixels[static_cast<std::size_t>(sourceY) *
                        source.width + sourceX] | 0xff000000u;
            }
        }
        return result;
    }

    float destinationWidth = static_cast<float>(source.width);
    float destinationHeight = static_cast<float>(source.height);
    if (position == WallpaperPosition::Stretch)
    {
        destinationWidth = static_cast<float>(canvasWidth);
        destinationHeight = static_cast<float>(canvasHeight);
    }
    else if (position == WallpaperPosition::Fit ||
             position == WallpaperPosition::Fill ||
             position == WallpaperPosition::Span)
    {
        const float horizontalScale = static_cast<float>(canvasWidth) /
            static_cast<float>(source.width);
        const float verticalScale = static_cast<float>(canvasHeight) /
            static_cast<float>(source.height);
        const float scale = position == WallpaperPosition::Fit
            ? std::min(horizontalScale, verticalScale)
            : std::max(horizontalScale, verticalScale);
        destinationWidth *= scale;
        destinationHeight *= scale;
    }
    const float destinationLeft = static_cast<float>(canvasBounds.left) +
        (static_cast<float>(canvasWidth) - destinationWidth) * 0.5f;
    const float destinationTop = static_cast<float>(canvasBounds.top) +
        (static_cast<float>(canvasHeight) - destinationHeight) * 0.5f;

    for (int y = 0; y < targetHeight; ++y)
    {
        const float screenY = static_cast<float>(targetBounds.top + y) + 0.5f;
        if (screenY < destinationTop ||
            screenY >= destinationTop + destinationHeight ||
            screenY < static_cast<float>(canvasBounds.top) ||
            screenY >= static_cast<float>(canvasBounds.bottom))
            continue;
        const float sourceY =
            (screenY - destinationTop) * source.height /
                destinationHeight - 0.5f;
        for (int x = 0; x < targetWidth; ++x)
        {
            const float screenX =
                static_cast<float>(targetBounds.left + x) + 0.5f;
            if (screenX < destinationLeft ||
                screenX >= destinationLeft + destinationWidth ||
                screenX < static_cast<float>(canvasBounds.left) ||
                screenX >= static_cast<float>(canvasBounds.right))
                continue;
            const float sourceX =
                (screenX - destinationLeft) * source.width /
                    destinationWidth - 0.5f;
            result.pixels[static_cast<std::size_t>(y) * targetWidth + x] =
                SampleBilinear(source, sourceX, sourceY);
        }
    }
    return result;
}

Wallpaper CropWallpaper(const Wallpaper& source, const RECT& sourceBounds,
    const RECT& targetBounds)
{
    Wallpaper result;
    const int sourceWidth = sourceBounds.right - sourceBounds.left;
    const int sourceHeight = sourceBounds.bottom - sourceBounds.top;
    const int targetWidth = targetBounds.right - targetBounds.left;
    const int targetHeight = targetBounds.bottom - targetBounds.top;
    if (sourceWidth <= 0 || sourceHeight <= 0 || targetWidth <= 0 ||
        targetHeight <= 0 || source.width != sourceWidth ||
        source.height != sourceHeight ||
        source.pixels.size() < static_cast<std::size_t>(sourceWidth) *
            sourceHeight || targetBounds.left < sourceBounds.left ||
        targetBounds.top < sourceBounds.top ||
        targetBounds.right > sourceBounds.right ||
        targetBounds.bottom > sourceBounds.bottom)
        return result;
    result.width = targetWidth;
    result.height = targetHeight;
    result.pixels.resize(static_cast<std::size_t>(targetWidth) *
        targetHeight);
    const int sourceX = targetBounds.left - sourceBounds.left;
    const int sourceY = targetBounds.top - sourceBounds.top;
    for (int y = 0; y < targetHeight; ++y)
    {
        const auto* row = source.pixels.data() +
            static_cast<std::size_t>(sourceY + y) * sourceWidth + sourceX;
        std::copy_n(row, targetWidth, result.pixels.data() +
            static_cast<std::size_t>(y) * targetWidth);
    }
    return result;
}

AcrylicNoisePixels GenerateAcrylicNoise(bool lightTheme)
{
    AcrylicNoisePixels pixels{};
    std::uint32_t state = 0x534E4F57u; // "SNOW", fixed seed.
    for (std::uint32_t& pixel : pixels)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const std::uint8_t alpha = static_cast<std::uint8_t>(
            2u + ((state >> 24) & 0x06u));
        const std::uint8_t channel = lightTheme ? 0u : alpha;
        pixel = (static_cast<std::uint32_t>(alpha) << 24) |
            (static_cast<std::uint32_t>(channel) << 16) |
            (static_cast<std::uint32_t>(channel) << 8) |
            static_cast<std::uint32_t>(channel);
    }
    return pixels;
}

bool DrawStage(ID2D1DeviceContext* context, const RECT& bounds,
    const StageStyle& style, const WallpaperViewport& viewport,
    const Wallpaper* source)
{
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (!context || width <= 0 || height <= 0)
        return false;
    const Wallpaper wallpaper = source && !source->pixels.empty()
        ? GenerateWallpaper(*source, width, height, viewport)
        : GenerateWallpaper(width, height, style.lightTheme, viewport);
    if (wallpaper.pixels.empty()) return false;

    const D2D1_BITMAP_PROPERTIES1 bitmapProperties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> sourceBitmap;
    if (FAILED(context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width),
                static_cast<UINT32>(height)),
            wallpaper.pixels.data(),
            static_cast<UINT32>(width * sizeof(std::uint32_t)),
            &bitmapProperties, &sourceBitmap)) || !sourceBitmap)
        return false;

    context->DrawBitmap(sourceBitmap.Get(), ToD2DRect(bounds), 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR);
    if (!style.glassEnabled || style.blurRadius <= 0.0f)
        return true;

    ComPtr<ID2D1Effect> blur;
    if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) ||
        !blur)
        return true;
    blur->SetInput(0, sourceBitmap.Get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
        std::clamp(style.blurRadius, 0.0f, 48.0f));
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
        D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
        D2D1_BORDER_MODE_HARD);

    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    ComPtr<ID2D1RoundedRectangleGeometry> clip;
    const float radius = std::max(0.0f, style.cornerRadius);
    if (!factory || FAILED(factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(ToD2DRect(bounds), radius, radius), &clip)) ||
        !clip)
        return true;
    context->PushLayer(D2D1::LayerParameters(
        ToD2DRect(bounds), clip.Get()), nullptr);
    context->DrawImage(blur.Get(), D2D1::Point2F(
        static_cast<float>(bounds.left), static_cast<float>(bounds.top)),
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
            static_cast<float>(height)), D2D1_INTERPOLATION_MODE_LINEAR,
        D2D1_COMPOSITE_MODE_SOURCE_OVER);
    context->PopLayer();
    return true;
}

void DrawAcrylicNoise(ID2D1DeviceContext* context, const RECT& bounds,
    float cornerRadius, bool lightTheme, POINT pixelOrigin)
{
    if (!context || IsRectEmpty(&bounds)) return;
    const AcrylicNoisePixels pixels = GenerateAcrylicNoise(lightTheme);
    const D2D1_BITMAP_PROPERTIES1 bitmapProperties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(AcrylicNoiseSize),
                static_cast<UINT32>(AcrylicNoiseSize)), pixels.data(),
            static_cast<UINT32>(AcrylicNoiseSize * sizeof(std::uint32_t)),
            &bitmapProperties, &bitmap)) || !bitmap)
        return;
    const D2D1_BITMAP_BRUSH_PROPERTIES1 brushProperties =
        D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP,
            D2D1_EXTEND_MODE_WRAP,
            D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    ComPtr<ID2D1BitmapBrush1> brush;
    if (FAILED(context->CreateBitmapBrush(bitmap.Get(), &brushProperties,
            nullptr, &brush)) || !brush)
        return;
    brush->SetTransform(D2D1::Matrix3x2F::Translation(
        -static_cast<float>(pixelOrigin.x),
        -static_cast<float>(pixelOrigin.y)));
    const float radius = std::max(0.0f, cornerRadius);
    context->FillRoundedRectangle(
        D2D1::RoundedRect(ToD2DRect(bounds), radius, radius), brush.Get());
}

bool DrawDimensionalBorder(ID2D1DeviceContext* context, const RECT& bounds,
    float cornerRadius, D2D1_COLOR_F color, float strokeWidth,
    float effectStrength)
{
    const float strength = std::clamp(effectStrength, 0.0f, 1.0f);
    if (!context || color.a <= 0.0f || strength <= 0.0005f ||
        IsRectEmpty(&bounds))
        return false;
    strokeWidth = std::max(0.5f, strokeWidth);
    const auto mix = [](float value, float target, float amount) {
        return std::clamp(value + (target - value) * amount, 0.0f, 1.0f);
    };
    const float highlightMix = 0.75f * strength;
    const float shadowMix = 0.50f * strength;
    const D2D1_COLOR_F bright = D2D1::ColorF(
        mix(color.r, 1.0f, highlightMix),
        mix(color.g, 1.0f, highlightMix),
        mix(color.b, 1.0f, highlightMix),
        std::clamp(color.a * (0.72f + 0.28f * strength), 0.0f, 1.0f));
    const D2D1_COLOR_F lowerRight = D2D1::ColorF(
        mix(color.r, 0.0f, shadowMix),
        mix(color.g, 0.0f, shadowMix),
        mix(color.b, 0.0f, shadowMix),
        std::clamp(color.a * (0.52f + 0.30f * strength), 0.0f, 1.0f));
    const D2D1_GRADIENT_STOP upperLeftStops[] = {
        { 0.0f, bright },
        { 0.46f, D2D1::ColorF(bright.r, bright.g, bright.b,
            bright.a * 0.55f) },
        { 0.82f, D2D1::ColorF(bright.r, bright.g, bright.b,
            bright.a * 0.20f) },
        { 1.0f, D2D1::ColorF(bright.r, bright.g, bright.b,
            bright.a * 0.12f) },
    };
    const D2D1_GRADIENT_STOP lowerRightStops[] = {
        { 0.0f, lowerRight },
        { 0.46f, D2D1::ColorF(lowerRight.r, lowerRight.g, lowerRight.b,
            lowerRight.a * 0.55f) },
        { 0.82f, D2D1::ColorF(lowerRight.r, lowerRight.g, lowerRight.b,
            lowerRight.a * 0.20f) },
        { 1.0f, D2D1::ColorF(lowerRight.r, lowerRight.g, lowerRight.b,
            lowerRight.a * 0.12f) },
    };
    ComPtr<ID2D1GradientStopCollection> upperLeftCollection;
    ComPtr<ID2D1GradientStopCollection> lowerRightCollection;
    if (FAILED(context->CreateGradientStopCollection(upperLeftStops,
            static_cast<UINT32>(std::size(upperLeftStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &upperLeftCollection)) ||
        !upperLeftCollection ||
        FAILED(context->CreateGradientStopCollection(lowerRightStops,
            static_cast<UINT32>(std::size(lowerRightStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &lowerRightCollection)) ||
        !lowerRightCollection)
        return false;

    auto createCornerBrush = [context](const D2D1_RECT_F& rect,
        bool lower, ID2D1GradientStopCollection* stops,
        ComPtr<ID2D1RadialGradientBrush>& brush) {
        const float width = rect.right - rect.left;
        const float height = rect.bottom - rect.top;
        const D2D1_POINT_2F center = lower
            ? D2D1::Point2F(rect.right, rect.bottom)
            : D2D1::Point2F(rect.left, rect.top);
        return SUCCEEDED(context->CreateRadialGradientBrush(
            D2D1::RadialGradientBrushProperties(center,
                D2D1::Point2F(0.0f, 0.0f), width, height),
            stops, &brush)) && brush;
    };

    const D2D1_RECT_F outerRect = ToD2DRect(bounds);
    ComPtr<ID2D1RadialGradientBrush> upperLeftBrush;
    ComPtr<ID2D1RadialGradientBrush> lowerRightBrush;
    if (!createCornerBrush(outerRect, false, upperLeftCollection.Get(),
            upperLeftBrush) ||
        !createCornerBrush(outerRect, true, lowerRightCollection.Get(),
            lowerRightBrush))
        return false;
    const float radius = std::max(0.0f, cornerRadius);
    const D2D1_ROUNDED_RECT outer = D2D1::RoundedRect(
        outerRect, radius, radius);
    const float inset = std::max(0.85f, strokeWidth * 0.85f);
    const D2D1_RECT_F innerRect = D2D1::RectF(
        bounds.left + inset, bounds.top + inset,
        bounds.right - inset, bounds.bottom - inset);
    const bool hasInnerEdge = innerRect.right > innerRect.left &&
        innerRect.bottom > innerRect.top;
    ComPtr<ID2D1GradientStopCollection> innerCollection;
    ComPtr<ID2D1RadialGradientBrush> innerLowerRightBrush;
    if (hasInnerEdge)
    {
        const float darkAlpha = std::clamp(
            color.a * (0.18f + 0.42f * strength), 0.01f, 0.22f);
        const D2D1_GRADIENT_STOP innerStops[] = {
            { 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, darkAlpha) },
            { 0.46f, D2D1::ColorF(0.0f, 0.0f, 0.0f,
                darkAlpha * 0.32f) },
            { 0.82f, D2D1::ColorF(0.0f, 0.0f, 0.0f,
                darkAlpha * 0.10f) },
            { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
        };
        if (FAILED(context->CreateGradientStopCollection(innerStops,
            static_cast<UINT32>(std::size(innerStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &innerCollection)) ||
            !innerCollection)
            return false;
        if (!createCornerBrush(innerRect, true, innerCollection.Get(),
                innerLowerRightBrush))
            return false;
    }

    for (ID2D1RadialGradientBrush* brush :
        { upperLeftBrush.Get(), lowerRightBrush.Get() })
    {
        brush->SetOpacity(0.10f + 0.22f * strength);
        context->DrawRoundedRectangle(outer, brush, strokeWidth + 1.35f);
        brush->SetOpacity(1.0f);
        context->DrawRoundedRectangle(outer, brush, strokeWidth);
    }
    if (hasInnerEdge)
    {
        const D2D1_ROUNDED_RECT inner = D2D1::RoundedRect(innerRect,
            std::max(0.0f, radius - inset),
            std::max(0.0f, radius - inset));
        const float innerStroke = std::max(0.65f, strokeWidth * 0.60f);
        context->DrawRoundedRectangle(
            inner, innerLowerRightBrush.Get(), innerStroke);
    }
    return true;
}

} // namespace snowdesktop::widget_preview
