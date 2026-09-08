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
    std::vector<unsigned> Draw(const snowdesktop::LargeIconConfig& config, const snowdesktop::large_icon_renderer::View& view, bool title = false)
    {
        target->BeginDraw(); target->Clear(D2D1::ColorF(0, 0.f));
        snowdesktop::large_icon_renderer::DrawFrame(target.Get(), fonts.Get(), config, view);
        if (title) snowdesktop::large_icon_renderer::DrawFloatingTitle(target.Get(), fonts.Get(), config, view, {0, 0, width, height});
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
        view.frame = {100, 60, 300, 260}; view.name = L"SnowDesktop 原始图标";
        auto original = canvas.Image(64, 64); view.bitmap = original.Get();
        auto pixels = canvas.Draw(config, view);
        auto bounds = RedBounds(pixels);
        Check(bounds.left == 168 && bounds.top == 128 && bounds.right == 232 && bounds.bottom == 192,
            "production renderer centers a low-resolution original without upscaling");
        Check((Pixel(pixels, 115, 160) >> 24) >= 160 && (Pixel(pixels, 115, 160) >> 24) <= 170, "default frame has a background before color extraction");
        Check(Visible(pixels, {0, 265, Canvas::width, Canvas::height}) == 0, "idle frame reserves no permanent title row");
        Save(outputDirectory, "01-original.png", pixels);
        view.hover = 1; pixels = canvas.Draw(config, view, true); bounds = RedBounds(pixels);
        Check(bounds.left == 168 && bounds.top == 128 && bounds.right == 232 && bounds.bottom == 192,
            "default hover reveals text without moving the original pixels");
        Check(Visible(pixels, {0, 265, Canvas::width, Canvas::height}) > 100, "floating title is rendered below the frame");
        Save(outputDirectory, "02-floating-title.png", pixels);
        view.frame = {100, 145, 300, 345}; pixels = canvas.Draw(config, view, true);
        Check(Visible(pixels, {0, 0, Canvas::width, 139}) > 100 && Visible(pixels, {0, 350, Canvas::width, Canvas::height}) == 0,
            "floating title moves above a frame at the bottom screen edge");
        Save(outputDirectory, "03-edge-title.png", pixels);

        view.frame = {40, 75, 440, 255}; config.titleMode = config.hoverContent = 1;
        pixels = canvas.Draw(config, view, true); bounds = RedBounds(pixels);
        Check(bounds.left == 88 && bounds.top == 133 && bounds.right == 152 && bounds.bottom == 197,
            "left reveal preserves original pixel dimensions and centers the original in its left column");
        Check(Visible(pixels, {0, 270, Canvas::width, Canvas::height}) == 0, "a short inner title does not create a duplicate floating label");
        Save(outputDirectory, "04-left-title.png", pixels);
        view.name = L"一个很长的游戏名称，用于检查两行省略后仍然能够查看完整名称以及保持原始图标尺寸和外框几何不变。继续加入额外的标题内容，确保这段名称确实超过两行宽度，覆盖完整名称提示的回归场景。";
        pixels = canvas.Draw(config, view, true);
        Check(TextBands(pixels, {212, 133, 428, 198}) == 2, "large inner text keeps two lines before ellipsis");
        Check(Visible(pixels, {0, 270, Canvas::width, Canvas::height}) > 100, "truncated inner titles retain a full floating name");
        Save(outputDirectory, "05-long-title.png", pixels);
        const auto visibleConfig = config;
        config.opacity = 0; config.border = false; config.hoverFrame = 0;
        view.name = L"A"; pixels = canvas.Draw(config, view);
        Check(Visible(pixels, {212, 133, 290, 198}) == 0 && Visible(pixels, {350, 133, 428, 198}) == 0 &&
            Visible(pixels, {300, 145, 340, 188}) > 80,
            "large reveal text is centered on the right without a title backdrop");
        Save(outputDirectory, "10-left-title-no-backdrop.png", pixels);
        config = visibleConfig;
        view.animations = false; pixels = canvas.Draw(config, view, true); bounds = RedBounds(pixels);
        Check(bounds.left == 208 && bounds.right == 272, "reduced-motion rendering keeps originals centered");

        config = {}; config.content = 1; config.fit = 1; config.radius = 32; config.opacity = 0; config.border = false;
        view = {}; view.frame = {100, 60, 300, 260}; view.original = false;
        auto cover = canvas.Image(200, 200); view.bitmap = cover.Get();
        pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 100, 60) == 0 && Red(Pixel(pixels, 200, 60)), "cover and frame share rounded clipping");
        Check(Visible(pixels, {0, 0, 100, Canvas::height}) == 0 && Visible(pixels, {300, 0, Canvas::width, Canvas::height}) == 0,
            "filled content does not escape the fixed frame");
        Save(outputDirectory, "06-cover.png", pixels);
        config.radiusPercent = 100; pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 110, 80) == 0 && Red(Pixel(pixels, 200, 160)),
            "100 percent rounding clips cover content to half the short edge");
        config.radiusPercent = -1;
        view.pressed = true; pixels = canvas.Draw(config, view); bounds = RedBounds(pixels);
        Check(bounds.left == 104 && bounds.top == 64 && bounds.right == 296 && bounds.bottom == 256,
            "production press feedback shrinks an exact-aspect cover rather than recropping it unchanged");
        Save(outputDirectory, "07-cover-pressed.png", pixels);
        view.pressed = false; view.selected = true; pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 109, 66) == 0, "selected rounded covers do not retain a top-left circle outside the frame");
        Save(outputDirectory, "08-selected-transparent.png", pixels);
        view.bitmap = nullptr; config.radius = 0; pixels = canvas.Draw(config, view);
        Check(Visible(pixels, {105, 65, 114, 74}) == 0,
            "transparent selected frames do not draw an extra top-left badge");
        Check(Visible(pixels, {190, 59, 210, 62}) > 0 && Pixel(pixels, 200, 160) == 0,
            "an independent selection outline remains visible without an image or background");

        config = {}; config.shadow = true; config.shadowStrength = 1; config.radius = 14;
        view = {}; view.frame = {120, 80, 360, 320}; view.scale = 2;
        auto transparent = canvas.Image(128, 128, true); view.bitmap = transparent.Get();
        pixels = canvas.Draw(config, view);
        Check((Pixel(pixels, 117, 210) >> 24) > 0, "production shadow spreads outside the frame at the active DPI");
        Check((Pixel(pixels, 130, 210) >> 24) > 0, "transparent source pixels retain the default frame background");
        Save(outputDirectory, "09-transparent-200dpi.png", pixels);
    }
    catch (const std::exception& error) { Check(false, error.what()); }
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (failures) return 1;
    std::cout << "Large-icon production rendering checks passed (WIC software target, no desktop window)\n";
    return 0;
}
