#include "widget_preview_stage.h"
#include "appearance_edge_presets.h"
#include "popup_round_geometry.h"
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace
{
void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void CheckRimCoverage()
{
    using Microsoft::WRL::ComPtr;
    namespace wp = snowdesktop::widget_preview;
    namespace rounded = snowdesktop::popup_round_geometry;
    ComPtr<ID3D11Device> gpu; ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &gpu, nullptr, nullptr)) &&
        SUCCEEDED(gpu.As(&dxgi)) && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) &&
        SUCCEEDED(factory->CreateDevice(dxgi.Get(), &device)) &&
        SUCCEEDED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context)), "rim oracle creates an offscreen WARP context");
    std::size_t oldLoss = 0, tightSurfaceLoss = 0;
    for (float scale : {1.f, 1.25f, 1.5f, 2.f, 3.f})
    {
        const UINT w = static_cast<UINT>(200 * scale), h = static_cast<UINT>(160 * scale);
        const RECT frame{static_cast<LONG>(12 * scale), static_cast<LONG>(16 * scale),
            static_cast<LONG>(164 * scale), static_cast<LONG>(120 * scale)};
        const float radius = 12.25f * scale;
        ComPtr<ID2D1Bitmap1> target, readable;
        Check(SUCCEEDED(context->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)), &target)) &&
            SUCCEEDED(context->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0,
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED)), &readable)), "rim oracle creates readable bitmaps");
        context->SetTarget(target.Get()); context->SetDpi(96,96);
        context->BeginDraw(); context->Clear(D2D1::ColorF(0,0.f));
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
        Check(wp::DrawEdgeHighlight(context.Get(),frame,radius,D2D1::ColorF(1.f,1.f,1.f,.02f),4.f,.9f), "rim draws with its full reserved halo");
        Check(context->GetPrimitiveBlend()==D2D1_PRIMITIVE_BLEND_ADD &&
            context->GetAntialiasMode()==D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,"rim restores caller blend and antialias state");
        Check(SUCCEEDED(context->EndDraw()) && SUCCEEDED(readable->CopyFromBitmap(nullptr,target.Get(),nullptr)),"rim pixels read back");
        D2D1_MAPPED_RECT map{}; Check(SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ,&map)),"rim pixels map");
        HRGN fence = rounded::CreateWindowFence(frame,radius,0,3), old = rounded::CreateWindowFence(frame,radius);
        Check(fence && old,"rim oracle creates both content fences");
        std::size_t lost=0;
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)
            if(map.bits[static_cast<std::size_t>(y)*map.pitch+x*4+3])
            { if(!PtInRegion(fence,static_cast<int>(x),static_cast<int>(y)))++lost;
               if(!PtInRegion(old,static_cast<int>(x),static_cast<int>(y)))++oldLoss;
               if(!PtInRect(&frame,POINT{static_cast<LONG>(x),static_cast<LONG>(y)}))++tightSurfaceLoss; }
        readable->Unmap(); DeleteObject(fence); DeleteObject(old);
        Check(lost==0,"popup fence retains every actual rim pixel, including fractional rounded corners");
        // A status strip renders its complete reflection inside the filled
        // allocation. No exterior halo or opposite contour may require a gap.
        for(auto edge : {wp::HighlightEdge::Top, wp::HighlightEdge::Bottom, wp::HighlightEdge::Left, wp::HighlightEdge::Right})
        {
            const auto style=snowdesktop::MaterialEdges(snowdesktop::MaterialEdgePreset::GlassTransparent);
            context->BeginDraw();context->Clear(D2D1::ColorF(0,0.f));
            Check(wp::DrawEdgeHighlight(context.Get(),frame,0,D2D1::ColorF(1.f,1.f,1.f,.02f),style.width*scale,style.opacity,style.light,edge),"status edge uses the shared material");
            Check(SUCCEEDED(context->EndDraw()) && SUCCEEDED(readable->CopyFromBitmap(nullptr,target.Get(),nullptr)) &&
                SUCCEEDED(readable->Map(D2D1_MAP_OPTIONS_READ,&map)),"single-edge pixels read back");
            const auto alpha=[&](LONG x,LONG y){return map.bits[static_cast<std::size_t>(y)*map.pitch+static_cast<std::size_t>(x)*4+3];};
            const LONG midX=(frame.left+frame.right)/2,midY=(frame.top+frame.bottom)/2;
            const LONG x=edge==wp::HighlightEdge::Left?frame.left:edge==wp::HighlightEdge::Right?frame.right-1:midX;
            const LONG y=edge==wp::HighlightEdge::Top?frame.top:edge==wp::HighlightEdge::Bottom?frame.bottom-1:midY;
            Check(alpha(x,y)>0,"status reflection reaches the original material boundary");
            Check(alpha(midX,frame.top-1)==0 && alpha(midX,frame.bottom)==0 &&
                alpha(frame.left-1,midY)==0 && alpha(frame.right,midY)==0,"status reflection needs no outside drawing space");
            Check(alpha(midX,midY)==0,"single edge adds no contour through the strip body");
            readable->Unmap();
        }
        context->SetTarget(nullptr);
    }
    Check(oldLoss>0,"rim oracle reproduces clipping caused by the previous one-pixel content fence");
    Check(tightSurfaceLoss>0,"a render target bounded to the card crops straight-edge pixels even with an expanded GDI fence");
}
}

int main()
{
    CheckRimCoverage();
    using namespace snowdesktop::widget_preview;
    const Wallpaper dark = GenerateWallpaper(96, 72, false);
    const Wallpaper repeated = GenerateWallpaper(96, 72, false);
    const Wallpaper light = GenerateWallpaper(96, 72, true);
    Check(dark.width == 96 && dark.height == 72 &&
            dark.pixels.size() == 96u * 72u,
        "preview wallpaper preserves its requested dimensions");
    Check(dark.pixels == repeated.pixels,
        "preview wallpaper generation is deterministic");
    Check(dark.pixels == light.pixels,
        "the compatibility background is appearance-independent");
    Check(std::all_of(dark.pixels.begin(), dark.pixels.end(),
            [](std::uint32_t pixel) { return (pixel >> 24) == 0xffu; }) &&
          std::all_of(light.pixels.begin(), light.pixels.end(),
            [](std::uint32_t pixel) { return (pixel >> 24) == 0xffu; }),
        "preview wallpaper is fully opaque");
    const std::unordered_set<std::uint32_t> darkColors(
        dark.pixels.begin(), dark.pixels.end());
    const std::unordered_set<std::uint32_t> lightColors(
        light.pixels.begin(), light.pixels.end());
    Check(darkColors.size() > 512 && lightColors.size() > 512,
        "neutral preview fallback retains enough visual detail for blur");
    Check(GenerateWallpaper(1, 1, false).pixels.size() == 1 &&
            GenerateWallpaper(0, 72, false).pixels.empty() &&
            GenerateWallpaper(96, -1, true).pixels.empty(),
        "preview wallpaper handles minimum and invalid dimensions");

    const Wallpaper full = GenerateWallpaper(240, 180, false);
    const Wallpaper crop = GenerateWallpaper(80, 60, false,
        { 240, 180, 47, 33 });
    bool cropMatches = crop.width == 80 && crop.height == 60;
    for (int y = 0; y < crop.height && cropMatches; ++y)
    {
        for (int x = 0; x < crop.width; ++x)
        {
            if (crop.pixels[static_cast<std::size_t>(y) * crop.width + x] !=
                full.pixels[static_cast<std::size_t>(y + 33) * full.width +
                    x + 47])
            {
                cropMatches = false;
                break;
            }
        }
    }
    Check(cropMatches,
        "preview wallpaper crops preserve one continuous composition");

    Wallpaper selectedSource;
    selectedSource.width = 240;
    selectedSource.height = 180;
    selectedSource.pixels.resize(240u * 180u);
    for (int y = 0; y < selectedSource.height; ++y)
    {
        for (int x = 0; x < selectedSource.width; ++x)
        {
            selectedSource.pixels[static_cast<std::size_t>(y) *
                selectedSource.width + x] = 0xff000000u |
                static_cast<std::uint32_t>(x) |
                (static_cast<std::uint32_t>(y) << 8) |
                (static_cast<std::uint32_t>((x + y) & 0xff) << 16);
        }
    }
    const Wallpaper selectedFull = GenerateWallpaper(
        selectedSource, 240, 180);
    const Wallpaper selectedCrop = GenerateWallpaper(
        selectedSource, 80, 60, { 240, 180, 47, 33 });
    Check(selectedCrop.pixels.front() ==
            (0xff000000u | 47u | (33u << 8) | (80u << 16)) &&
            selectedCrop.pixels.back() ==
            (0xff000000u | 126u | (92u << 8) | (218u << 16)),
        "an explicit wallpaper preserves the shared card crop");
    const Wallpaper fingerprintSource{ 2, 2,
        { 0xff112233u, 0xff445566u, 0xff778899u, 0xffaabbccu } };
    auto changedPixel = fingerprintSource;
    changedPixel.pixels[1] ^= 1;
    const auto samePixels = fingerprintSource;
    Check(WallpaperFingerprint(fingerprintSource) == WallpaperFingerprint(samePixels) &&
            WallpaperFingerprint(fingerprintSource) != WallpaperFingerprint(changedPixel),
        "equal images share a fingerprint; a changed sampled pixel with equal dimensions invalidates it");
    Wallpaper white{ 1, 1, { 0xffffffffu } };
    Wallpaper black{ 1, 1, { 0xff000000u } };
    Check(WallpaperIsLight(white) && !WallpaperIsLight(black),
        "wallpaper luminance selects contrasting card chrome");
    Check(WallpaperPositionFromLegacySettings(0, false) ==
            WallpaperPosition::Center &&
          WallpaperPositionFromLegacySettings(2, false) ==
            WallpaperPosition::Stretch &&
          WallpaperPositionFromLegacySettings(6, false) ==
            WallpaperPosition::Fit &&
          WallpaperPositionFromLegacySettings(10, false) ==
            WallpaperPosition::Fill &&
          WallpaperPositionFromLegacySettings(22, false) ==
            WallpaperPosition::Span &&
          WallpaperPositionFromLegacySettings(10, true) ==
            WallpaperPosition::Tile,
        "legacy Windows wallpaper values map to every placement mode");

    Wallpaper placementSource;
    placementSource.width = 2;
    placementSource.height = 2;
    placementSource.pixels = {
        0xff100001u, 0xff200002u,
        0xff300003u, 0xff400004u,
    };
    constexpr std::uint32_t placementBackground = 0xffabcdefu;
    const RECT placementCanvas{ 0, 0, 4, 4 };
    const Wallpaper centered = RenderWallpaperRegion(placementSource,
        placementCanvas, placementCanvas, WallpaperPosition::Center,
        placementBackground);
    Check(centered.pixels[0] == placementBackground &&
            centered.pixels[5] == placementSource.pixels[0] &&
            centered.pixels[10] == placementSource.pixels[3] &&
            centered.pixels[15] == placementBackground,
        "center placement preserves native pixels and desktop borders");

    const Wallpaper tiled = RenderWallpaperRegion(placementSource,
        { -2, -1, 4, 3 }, { 0, 0, 4, 3 }, WallpaperPosition::Tile,
        placementBackground);
    Check(tiled.pixels == std::vector<std::uint32_t>{
            placementSource.pixels[2], placementSource.pixels[3],
            placementSource.pixels[2], placementSource.pixels[3],
            placementSource.pixels[0], placementSource.pixels[1],
            placementSource.pixels[0], placementSource.pixels[1],
            placementSource.pixels[2], placementSource.pixels[3],
            placementSource.pixels[2], placementSource.pixels[3] },
        "tile placement remains anchored to virtual desktop coordinates");

    const Wallpaper stretched = RenderWallpaperRegion(placementSource,
        placementCanvas, placementCanvas, WallpaperPosition::Stretch,
        placementBackground);
    Check(stretched.pixels.front() == placementSource.pixels.front() &&
            stretched.pixels[3] == placementSource.pixels[1] &&
            stretched.pixels[12] == placementSource.pixels[2] &&
            stretched.pixels.back() == placementSource.pixels.back(),
        "stretch placement maps every source corner to the monitor");

    Wallpaper wideSource{ 2, 1,
        { 0xff0000ffu, 0xffff0000u } };
    const Wallpaper fitted = RenderWallpaperRegion(wideSource,
        placementCanvas, placementCanvas, WallpaperPosition::Fit,
        placementBackground);
    Check(std::all_of(fitted.pixels.begin(), fitted.pixels.begin() + 4,
            [](std::uint32_t pixel) {
                return pixel == placementBackground;
            }) &&
          std::all_of(fitted.pixels.end() - 4, fitted.pixels.end(),
            [](std::uint32_t pixel) {
                return pixel == placementBackground;
            }) &&
          fitted.pixels[4] != placementBackground,
        "fit placement uses the Windows desktop color for letterboxing");
    const Wallpaper filled = RenderWallpaperRegion(wideSource,
        placementCanvas, placementCanvas, WallpaperPosition::Fill,
        placementBackground);
    Check(std::none_of(filled.pixels.begin(), filled.pixels.end(),
            [](std::uint32_t pixel) {
                return pixel == placementBackground;
            }),
        "fill placement covers the monitor");
    const Wallpaper columns{ 4, 2, {
        0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu,
        0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu } };
    const auto centerColumns = RenderWallpaperRegion(columns,
        { 0, 0, 2, 2 }, { 0, 0, 2, 2 }, WallpaperPosition::Fill,
        placementBackground);
    Check(centerColumns.pixels == std::vector<std::uint32_t>{
            0xff00ff00u, 0xff0000ffu, 0xff00ff00u, 0xff0000ffu },
        "fill keeps native scale and crops the central columns of a twice-as-wide source");

    const RECT spanCanvas{ -4, 0, 8, 6 };
    const Wallpaper spanFull = RenderWallpaperRegion(selectedSource,
        spanCanvas, spanCanvas, WallpaperPosition::Span,
        placementBackground);
    const RECT spanRightBounds{ 0, 0, 8, 6 };
    const Wallpaper spanRight = RenderWallpaperRegion(selectedSource,
        spanCanvas, spanRightBounds, WallpaperPosition::Span,
        placementBackground);
    Check(spanRight.pixels == CropWallpaper(
            spanFull, spanCanvas, spanRightBounds).pixels,
        "span placement preserves one continuous multi-monitor composition");
    Check(RenderWallpaperRegion(placementSource, { 0, 0, 0, 4 },
            placementCanvas, WallpaperPosition::Fill,
            placementBackground).pixels.empty(),
        "desktop wallpaper placement rejects invalid canvas dimensions");

    Wallpaper monitor;
    monitor.width = 4;
    monitor.height = 3;
    monitor.pixels = {
        0xff000001u, 0xff000002u, 0xff000003u, 0xff000004u,
        0xff000005u, 0xff000006u, 0xff000007u, 0xff000008u,
        0xff000009u, 0xff00000au, 0xff00000bu, 0xff00000cu,
    };
    const Wallpaper positionedCrop = CropWallpaper(monitor,
        { -1920, 0, -1916, 3 }, { -1918, 1, -1916, 3 });
    Check(positionedCrop.width == 2 && positionedCrop.height == 2 &&
            positionedCrop.pixels == std::vector<std::uint32_t>{
                0xff000007u, 0xff000008u,
                0xff00000bu, 0xff00000cu },
        "desktop wallpaper crop preserves physical position without scaling");
    Check(CropWallpaper(monitor, { 0, 0, 4, 3 },
            { 3, 2, 5, 3 }).pixels.empty(),
        "desktop wallpaper crop rejects rectangles outside the monitor frame");

    const D2D1_COLOR_F opaqueReflection =
        ResolveEdgeHighlightReflection(
            D2D1::ColorF(0.10f, 0.30f, 0.60f, 1.0f), 0.75f);
    const D2D1_COLOR_F translucentReflection =
        ResolveEdgeHighlightReflection(
            D2D1::ColorF(0.10f, 0.30f, 0.60f, 0.25f), 0.75f);
    const D2D1_COLOR_F transparentReflection =
        ResolveEdgeHighlightReflection(
            D2D1::ColorF(0.10f, 0.30f, 0.60f, 0.0f), 0.75f);
    Check(translucentReflection.r > opaqueReflection.r &&
            translucentReflection.g > opaqueReflection.g &&
            translucentReflection.b > opaqueReflection.b &&
            std::abs(translucentReflection.a - opaqueReflection.a) <
                0.0001f,
        "transparent panel material mixes its reflected edge color toward white without weakening the configured light strength");
    Check(std::abs(transparentReflection.r - 1.0f) < 0.0001f &&
            std::abs(transparentReflection.g - 1.0f) < 0.0001f &&
            std::abs(transparentReflection.b - 1.0f) < 0.0001f,
        "fully transparent panel material reflects the neutral white incident-light estimate");

    const AcrylicNoisePixels darkNoise = GenerateAcrylicNoise(false);
    const AcrylicNoisePixels repeatedNoise = GenerateAcrylicNoise(false);
    const AcrylicNoisePixels lightNoise = GenerateAcrylicNoise(true);
    Check(darkNoise == repeatedNoise,
        "acrylic noise generation is deterministic");
    Check(darkNoise != lightNoise,
        "acrylic noise polarity follows the content theme");
    for (std::size_t index = 0; index < darkNoise.size(); ++index)
    {
        Check((darkNoise[index] >> 24) == (lightNoise[index] >> 24),
            "acrylic theme variants preserve the same alpha texture");
    }
    std::cout << "widget preview stage tests passed\n";
    return 0;
}
