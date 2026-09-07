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
void Save(const char* directory, const char* name, const std::vector<unsigned>& pixels)
{
    if (!directory) return;
    std::string error;
    Check(snowdesktop::preview_png::Save(std::filesystem::path(directory) / name, Canvas::width, Canvas::height, pixels, error), error.c_str());
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
        Check(bounds.left == 52 && bounds.top == 133 && bounds.right == 116 && bounds.bottom == 197,
            "left reveal preserves original pixel dimensions and moves to the frame inset");
        Check(Visible(pixels, {0, 270, Canvas::width, Canvas::height}) == 0, "a short inner title does not create a duplicate floating label");
        Save(outputDirectory, "04-left-title.png", pixels);
        view.name = L"一个很长的游戏名称，用于检查两行省略后仍然能够查看完整名称以及保持原始图标尺寸和外框几何不变。";
        pixels = canvas.Draw(config, view, true);
        Check(Visible(pixels, {0, 270, Canvas::width, Canvas::height}) > 100, "truncated inner titles retain a full floating name");
        Save(outputDirectory, "05-long-title.png", pixels);
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
        view.pressed = true; pixels = canvas.Draw(config, view); bounds = RedBounds(pixels);
        Check(bounds.left == 104 && bounds.top == 64 && bounds.right == 296 && bounds.bottom == 256,
            "production press feedback shrinks an exact-aspect cover rather than recropping it unchanged");
        Save(outputDirectory, "07-cover-pressed.png", pixels);
        view.pressed = false; view.selected = true; pixels = canvas.Draw(config, view);
        Check(Pixel(pixels, 109, 69) != 0, "selection remains independently visible on a transparent frame corner");
        Save(outputDirectory, "08-selected-transparent.png", pixels);

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
