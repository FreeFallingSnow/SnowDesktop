#include "large_icon_renderer.h"
#include "preview_png_writer.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <d2d1helper.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
int failures = 0;
void Check(bool condition, const char* message)
{ if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }
void Require(HRESULT result, const char* operation)
{ if (FAILED(result)) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result)); }

struct Canvas
{
    static constexpr int width = 480, height = 360;
    ComPtr<IWICBitmap> surface;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<IDWriteFactory> fonts;
    Canvas()
    {
        ComPtr<IWICImagingFactory> images;
        Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&images)), "WIC factory");
        Require(images->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &surface), "WIC surface");
        ComPtr<ID2D1Factory> drawing;
        Require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, drawing.GetAddressOf()), "D2D factory");
        Require(drawing->CreateWicBitmapRenderTarget(surface.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &target), "D2D WIC target");
        Require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(fonts.GetAddressOf())), "DWrite factory");
    }
    ComPtr<ID2D1Bitmap> Image(int w, int h, bool transparent = false)
    {
        std::vector<unsigned> pixels(static_cast<size_t>(w) * h, 0xffff4020);
        if (transparent)
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
                if (x < w / 4 || x >= w * 3 / 4 || y < h / 4 || y >= h * 3 / 4) pixels[static_cast<size_t>(y) * w + x] = 0;
        ComPtr<ID2D1Bitmap> image;
        Require(target->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w * 4,
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96), &image), "image fixture");
        return image;
    }
    std::vector<unsigned> Draw(const snowdesktop::LargeIconConfig& config, const snowdesktop::large_icon_renderer::View& view)
    {
        target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.f));
        snowdesktop::large_icon_renderer::DrawFrame(target.Get(), fonts.Get(), config, view);
        Require(target->EndDraw(), "production draw");
        std::vector<unsigned> pixels(width * height);
        Require(surface->CopyPixels(nullptr, width * 4, width * height * 4, reinterpret_cast<BYTE*>(pixels.data())), "read rendered pixels");
        return pixels;
    }
};
unsigned Pixel(const std::vector<unsigned>& image, int x, int y) { return image[static_cast<size_t>(y) * Canvas::width + x]; }
bool Red(unsigned value) { return (value >> 24) > 200 && ((value >> 16) & 255) > 220 && ((value >> 8) & 255) < 110; }
RECT RedBounds(const std::vector<unsigned>& image)
{
    RECT rect{Canvas::width, Canvas::height, 0, 0};
    for (int y = 0; y < Canvas::height; ++y) for (int x = 0; x < Canvas::width; ++x)
        if (Red(Pixel(image, x, y))) { rect.left = std::min<LONG>(rect.left, x); rect.top = std::min<LONG>(rect.top, y); rect.right = std::max<LONG>(rect.right, x + 1); rect.bottom = std::max<LONG>(rect.bottom, y + 1); }
    return rect;
}
size_t Visible(const std::vector<unsigned>& image, RECT area)
{
    size_t count = 0;
    for (LONG y = area.top; y < area.bottom; ++y) for (LONG x = area.left; x < area.right; ++x)
        count += (Pixel(image, x, y) >> 24) != 0;
    return count;
}
int TextBands(const std::vector<unsigned>& image, RECT area)
{
    int bands = 0, blank = 3;
    for (LONG y = area.top; y < area.bottom; ++y)
    {
        bool ink = false;
        for (LONG x = area.left; x < area.right; ++x)
        {
            const auto value = Pixel(image, x, y);
            ink = ink || ((value >> 24) > 200 && ((value >> 16) & 255) > 180 &&
                ((value >> 8) & 255) > 180 && (value & 255) > 180);
        }
        if (ink) { if (blank >= 2) ++bands; blank = 0; }
        else ++blank;
    }
    return bands;
}
void Save(const char* directory, const char* name, const std::vector<unsigned>& pixels)
{
    if (!directory) return;
    std::string error;
    const bool saved = snowdesktop::preview_png::Save(std::filesystem::path(directory) / name, Canvas::width, Canvas::height, pixels, error);
    Check(saved, error.c_str());
}
}

int RunLargeIconRenderingTests(const char* outputDirectory)
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        Canvas canvas;
        snowdesktop::LargeIconConfig config;
        snowdesktop::large_icon_renderer::View view;
        view.frame = {100, 60, 300, 260}; view.name = L"SnowDesktop";
        auto original = canvas.Image(64, 64); view.bitmap = original.Get();
        auto pixels = canvas.Draw(config, view);
        auto bounds = RedBounds(pixels);
        Check(bounds.left == 168 && bounds.top == 128 && bounds.right == 232 && bounds.bottom == 192,
            "production renderer centers low-resolution originals without upscaling");
        Check(Pixel(pixels, 115, 160) == 0xffe8ecf4, "first frame has an opaque default-beautify background before assets load");
        view.hover = 1;
        Check(canvas.Draw(config, view) == pixels, "no-effect hover leaves original and background unchanged");
        Check(Visible(pixels, {0, 265, Canvas::width, Canvas::height}) == 0, "no external title layer is drawn");
        Save(outputDirectory, "01-original.png", pixels);
        view.hasEdgeColor = true; view.edgeColor = 0x0112ff; view.accent = 0x006622;
        pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 115, 160) == 0xff0112ff, "contour background preserves extracted RGB and ignores transparency settings");
        config.smartFill = false; config.themeOpacity = .4;
        pixels = canvas.Draw(config, view);
        Check((Pixel(pixels, 115, 160) >> 24) >= 100 && (Pixel(pixels, 115, 160) >> 24) <= 104,
            "theme fallback uses its independent opacity");
        config.themeGradient = true; config.themeAngle = 0;
        pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 115, 160) != Pixel(pixels, 285, 160), "automatic gradient changes color across the requested direction");

        config = {}; config.effect = 2;
        view = {}; view.frame = {40, 75, 440, 255}; view.bitmap = original.Get(); view.hover = 1;
        view.name = L"A"; view.backgroundResolved = true; view.background.opacity = 0;
        config.autoTitleColor = false; config.titleColor = 0xffffff;
        pixels = canvas.Draw(config, view); bounds = RedBounds(pixels);
        Check(bounds.left == 88 && bounds.top == 133 && bounds.right == 152 && bounds.bottom == 197,
            "left reveal preserves original size and centers it in its left column");
        Check(Visible(pixels, {212, 133, 290, 198}) == 0 && Visible(pixels, {350, 133, 428, 198}) == 0 &&
            Visible(pixels, {300, 135, 340, 196}) > 50,
            "large inner title is centered in the right column without a backdrop");
        Save(outputDirectory, "02-left-title.png", pixels);
        view.animations = false;
        Check(canvas.Draw(config, view) == pixels, "reduced motion uses the same final inner-title pose without an external label");
        view.name = L"A very long title with enough words to occupy several lines without escaping the fixed card frame. More title words follow.";
        pixels = canvas.Draw(config, view);
        Check(TextBands(pixels, {212, 133, 428, 198}) == 2, "long title renders at most two centered lines");
        Check(Visible(pixels, {0, 260, Canvas::width, Canvas::height}) == 0, "long truncated title never creates a floating label");
        Save(outputDirectory, "03-long-title.png", pixels);
        view.name = L"Snow";
        const auto titleArea = RECT{212, 133, 428, 198};
        config.titleWeight = 100;
        const auto light = canvas.Draw(config, view);
        config.titleWeight = 900;
        const auto heavy = canvas.Draw(config, view);
        Check(Visible(heavy, titleArea) > Visible(light, titleArea), "title weight changes actual glyph coverage");
        config.backgroundStyle = 7; config.autoTitleColor = true; view.componentForeground = 0x161616;
        pixels = canvas.Draw(config, view);
        auto darkest = std::count_if(pixels.begin(), pixels.end(), [](unsigned p) { return p == 0xff161616; });
        Check(darkest > 10, "component background title uses the actual dark theme foreground");
        config.autoTitleColor = false; config.titleColor = 0x00ff00;
        pixels = canvas.Draw(config, view);
        Check(std::count(pixels.begin(), pixels.end(), 0xff00ff00) > 10, "manual color reaches rendered title glyphs");
        config.titleDirection = 1; config.titleWeight = 600;
        view.frame = {140, 10, 320, 350}; view.name = L"Title";
        pixels = canvas.Draw(config, view); bounds = RedBounds(pixels);
        Check(bounds.top == 46 && bounds.bottom == 110, "up reveal moves unchanged original into the upper column");
        Check(Visible(pixels, {152, 158, 308, 338}) > 50, "up reveal draws its title in the lower region");
        Save(outputDirectory, "04-up-title.png", pixels);

        config = {}; config.backgroundStyle = -2; config.radius = 32;
        view = {}; view.frame = {100, 60, 300, 260};
        auto cover = canvas.Image(800, 200); view.bitmap = cover.Get();
        int backgroundCalls = 0;
        view.drawBackground = [&](auto*, RECT, float, float) { ++backgroundCalls; };
        pixels = canvas.Draw(config, view);
        Check(backgroundCalls == 0, "fill mode never invokes component background or blur drawing");
        Check(Pixel(pixels, 100, 60) == 0 && Red(Pixel(pixels, 200, 61)), "fill and frame share rounded clipping");
        Check(Visible(pixels, {0, 0, 100, Canvas::height}) == 0 && Visible(pixels, {300, 0, Canvas::width, Canvas::height}) == 0,
            "fill never escapes the frame");
        config.radiusPercent = 100; pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 110, 80) == 0 && Red(Pixel(pixels, 200, 160)), "100 percent rounding uses half short edge");
        config.radiusPercent = 0; config.effect = 2;
        view.frame = {40, 75, 440, 255}; view.hover = 1;
        pixels = canvas.Draw(config, view);
        Check(Visible(pixels, {275, 80, 435, 250}) == 0 && Red(Pixel(pixels, 120, 160)),
            "an oversized fill source is cropped before left movement, leaving transparent title space");
        Save(outputDirectory, "05-fill-reveal.png", pixels);
        config.effect = 0; config.fit = 0; view.frame = {100, 60, 300, 260};
        pixels = canvas.Draw(config, view);
        Check(Visible(pixels, {105, 65, 295, 120}) == 0 && Red(Pixel(pixels, 200, 160)),
            "contain fill leaves gaps transparent without an automatic base");
        view.bitmap = nullptr; view.selected = true;
        int placeholders = 0; view.placeholder = [&](RECT, float) { ++placeholders; };
        pixels = canvas.Draw(config, view);
        Check(Visible(pixels, {105, 65, 114, 74}) == 0 && Pixel(pixels, 200, 160) == 0 &&
            Visible(pixels, {190, 59, 210, 62}) > 0,
            "transparent selected frame keeps an independent outline without a top-left badge");
        Check(placeholders == 0, "unavailable fill remains transparent instead of drawing a generic placeholder");
        config.backgroundStyle = -1;
        canvas.Draw(config, view);
        Check(backgroundCalls == 1, "component-series background dispatches to the shared engine");

        config = {}; view = {}; view.frame = {120, 80, 360, 320}; view.scale = 2;
        auto transparent = canvas.Image(128, 128, true); view.bitmap = transparent.Get();
        pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 130, 210) == 0xffe8ecf4, "transparent source pixels retain the default background at 200 percent DPI");
        Save(outputDirectory, "06-transparent-200dpi.png", pixels);
    }
    catch (const std::exception& error) { Check(false, error.what()); }
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (failures) return 1;
    std::cout << "Large-icon production rendering checks passed (WIC software target, no desktop window)\n";
    return 0;
}
