#include "widget_preview_stage.h"

#include "resource.h"

#include <d2d1effects.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

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

Wallpaper LoadLandscapeResource()
{
    Wallpaper result;
    const HMODULE module = GetModuleHandleW(nullptr);
    const HRSRC resource = FindResourceW(module,
        MAKEINTRESOURCEW(IDR_WIDGET_PREVIEW_LANDSCAPE), RT_RCDATA);
    if (!resource) return result;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD resourceSize = SizeofResource(module, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(loaded));
    if (!loaded || !bytes || resourceSize == 0) return result;

    ScopedCom com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE)
        return result;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || !factory ||
        FAILED(factory->CreateStream(&stream)) || !stream ||
        FAILED(stream->InitializeFromMemory(bytes, resourceSize)) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder)) || !decoder ||
        FAILED(decoder->GetFrame(0, &frame)) || !frame ||
        FAILED(factory->CreateFormatConverter(&converter)) || !converter ||
        FAILED(converter->Initialize(frame.Get(),
            GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
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

Wallpaper GenerateWallpaper(int width, int height, bool lightTheme)
{
    return GenerateWallpaper(width, height, lightTheme, {});
}

Wallpaper GenerateWallpaper(int width, int height, bool lightTheme,
    const WallpaperViewport& viewport)
{
    Wallpaper wallpaper;
    if (width <= 0 || height <= 0)
        return wallpaper;
    (void)lightTheme;
    wallpaper.width = width;
    wallpaper.height = height;
    wallpaper.pixels.resize(static_cast<std::size_t>(width) * height);
    static const Wallpaper source = LoadLandscapeResource();
    if (source.pixels.empty()) return {};
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
    const StageStyle& style, const WallpaperViewport& viewport)
{
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (!context || width <= 0 || height <= 0)
        return false;
    const Wallpaper wallpaper = GenerateWallpaper(
        width, height, style.lightTheme, viewport);
    if (wallpaper.pixels.empty()) return false;

    const D2D1_BITMAP_PROPERTIES1 bitmapProperties =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> source;
    if (FAILED(context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(width),
                static_cast<UINT32>(height)),
            wallpaper.pixels.data(),
            static_cast<UINT32>(width * sizeof(std::uint32_t)),
            &bitmapProperties, &source)) || !source)
        return false;

    context->DrawBitmap(source.Get(), ToD2DRect(bounds), 1.0f,
        D2D1_INTERPOLATION_MODE_LINEAR);
    if (!style.glassEnabled || style.blurRadius <= 0.0f)
        return true;

    ComPtr<ID2D1Effect> blur;
    if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blur)) ||
        !blur)
        return true;
    blur->SetInput(0, source.Get());
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

bool DrawGlassBorder(ID2D1DeviceContext* context, const RECT& bounds,
    float cornerRadius, D2D1_COLOR_F color, float strokeWidth)
{
    if (!context || color.a <= 0.0f || IsRectEmpty(&bounds))
        return false;
    const auto mixWhite = [](float value, float amount) {
        return std::clamp(value + (1.0f - value) * amount, 0.0f, 1.0f);
    };
    const D2D1_COLOR_F bright = D2D1::ColorF(
        mixWhite(color.r, 0.58f), mixWhite(color.g, 0.58f),
        mixWhite(color.b, 0.58f),
        std::clamp(color.a * 0.91f, 0.0f, 1.0f));
    const D2D1_COLOR_F lowerRight = D2D1::ColorF(
        mixWhite(color.r, 0.18f), mixWhite(color.g, 0.18f),
        mixWhite(color.b, 0.18f),
        std::clamp(color.a * 0.82f, 0.0f, 1.0f));
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
    for (ID2D1RadialGradientBrush* brush :
        { upperLeftBrush.Get(), lowerRightBrush.Get() })
    {
        brush->SetOpacity(0.24f);
        context->DrawRoundedRectangle(outer, brush, strokeWidth + 1.35f);
        brush->SetOpacity(1.0f);
        context->DrawRoundedRectangle(outer, brush, strokeWidth);
    }

    const float inset = std::max(0.85f, strokeWidth * 0.85f);
    const D2D1_RECT_F innerRect = D2D1::RectF(
        bounds.left + inset, bounds.top + inset,
        bounds.right - inset, bounds.bottom - inset);
    if (innerRect.right <= innerRect.left || innerRect.bottom <= innerRect.top)
        return true;
    const float darkAlpha = std::clamp(color.a * 0.30f, 0.015f, 0.14f);
    const D2D1_GRADIENT_STOP innerStops[] = {
        { 0.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, darkAlpha) },
        { 0.46f, D2D1::ColorF(0.0f, 0.0f, 0.0f,
            darkAlpha * 0.32f) },
        { 0.82f, D2D1::ColorF(0.0f, 0.0f, 0.0f,
            darkAlpha * 0.10f) },
        { 1.0f, D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f) },
    };
    ComPtr<ID2D1GradientStopCollection> innerCollection;
    ComPtr<ID2D1RadialGradientBrush> innerUpperLeftBrush;
    ComPtr<ID2D1RadialGradientBrush> innerLowerRightBrush;
    if (SUCCEEDED(context->CreateGradientStopCollection(innerStops,
            static_cast<UINT32>(std::size(innerStops)), D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP, &innerCollection)) &&
        innerCollection &&
        createCornerBrush(innerRect, false, innerCollection.Get(),
            innerUpperLeftBrush) &&
        createCornerBrush(innerRect, true, innerCollection.Get(),
            innerLowerRightBrush))
    {
        const D2D1_ROUNDED_RECT inner = D2D1::RoundedRect(innerRect,
            std::max(0.0f, radius - inset),
            std::max(0.0f, radius - inset));
        const float innerStroke = std::max(0.65f, strokeWidth * 0.65f);
        context->DrawRoundedRectangle(
            inner, innerUpperLeftBrush.Get(), innerStroke);
        context->DrawRoundedRectangle(
            inner, innerLowerRightBrush.Get(), innerStroke);
    }
    return true;
}

} // namespace snowdesktop::widget_preview
