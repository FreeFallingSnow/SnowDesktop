#pragma once

#include "navigation/quick_navigation_attraction_renderer.h"

#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <iostream>
#include <vector>

namespace snowdesktop::test
{
inline int CheckQuickNavigationAttractionPixels()
{
    using Microsoft::WRL::ComPtr;
    namespace attraction = dock_attraction;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
        return ok;
    };
    ComPtr<ID3D11Device> graphics;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> drawing;
    ComPtr<ID2D1DeviceContext> context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &graphics, nullptr, nullptr);
    if (SUCCEEDED(hr)) hr = graphics.As(&dxgi);
    if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
    if (SUCCEEDED(hr)) hr = factory->CreateDevice(dxgi.Get(), &drawing);
    if (SUCCEEDED(hr)) hr = drawing->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &context);
    if (!check(SUCCEEDED(hr), "Attraction pixel regression creates an isolated WARP device")) return failures;

    constexpr UINT width = 800, height = 800;
    const auto pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    const auto targetProperties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, pixelFormat, 96, 96);
    const auto readProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, pixelFormat, 96, 96);
    ComPtr<ID2D1Bitmap1> actual, expected, readActual, readExpected, source;
    hr = context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, targetProperties, &actual);
    if (SUCCEEDED(hr)) hr = context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, targetProperties, &expected);
    if (SUCCEEDED(hr)) hr = context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, readProperties, &readActual);
    if (SUCCEEDED(hr)) hr = context->CreateBitmap(D2D1::SizeU(width, height), nullptr, 0, readProperties, &readExpected);
    constexpr UINT sourceWidth = 520, sourceHeight = 352;
    constexpr std::uint32_t uniform = 0x80402010;
    std::vector<std::uint32_t> sourcePixels(sourceWidth * sourceHeight, uniform);
    if (SUCCEEDED(hr)) hr = context->CreateBitmap(D2D1::SizeU(sourceWidth, sourceHeight), sourcePixels.data(),
        sourceWidth * sizeof(std::uint32_t), D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, pixelFormat, 96, 96), &source);
    ComPtr<ID2D1SolidColorBrush> solid;
    if (SUCCEEDED(hr)) hr = context->CreateSolidColorBrush(D2D1::ColorF(64.f / 128, 32.f / 128, 16.f / 128, 128.f / 255), &solid);
    if (!check(SUCCEEDED(hr), "Attraction pixel regression allocates its own images")) return failures;

    // Uniform translucent content must remain uniform under deformation. An
    // independent single-mesh silhouette supplies the expected union coverage;
    // no DComp clips, desktop windows, backdrop or user data are substituted.
    const attraction::Rect panel{-1800.25, -940.75, -1318.875, -627.125};
    const attraction::Rect docks[] = {{-1620, -580, -1572, -532}, {-1620, -1020, -1572, -972},
        {-1880, -800, -1832, -752}, {-1270, -800, -1222, -752}};
    constexpr double hostX = -2000, hostY = -1200;
    std::size_t sampled = 0, mismatches = 0;
    for (const auto dock : docks)
    for (const double collapsed : {0.0, 0.12, 0.33, 0.55, 0.81, 0.94, 1.0})
    {
        const auto frame = attraction::MeshFrame(panel, dock, collapsed);
        context->SetTarget(actual.Get());
        context->SetTransform(D2D1::Matrix3x2F::Translation(9, 13)); // DComp atlas offset.
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0.f));
        hr = quick_navigation_animation_rules::DrawAttractionMesh(context.Get(), source.Get(), frame,
            hostX, hostY, panel.right - panel.left, panel.bottom - panel.top);
        const HRESULT end = context->EndDraw();
        context->SetTarget(nullptr);
        if (!check(SUCCEEDED(hr) && SUCCEEDED(end), "Attraction production mesh rasterizes every pose")) return failures;

        std::vector<D2D1_TRIANGLE> triangles;
        for (std::size_t i = 0; i < attraction::TriangleCount; ++i)
        {
            const auto indices = attraction::TriangleVertices(i);
            const auto point = [&](std::size_t index) {
                return D2D1::Point2F(static_cast<float>(frame.vertices[index].x - hostX),
                    static_cast<float>(frame.vertices[index].y - hostY));
            };
            triangles.push_back({point(indices[0]), point(indices[1]), point(indices[2])});
        }
        ComPtr<ID2D1Mesh> silhouette;
        ComPtr<ID2D1TessellationSink> sink;
        hr = context->CreateMesh(&silhouette);
        if (SUCCEEDED(hr)) hr = silhouette->Open(&sink);
        if (SUCCEEDED(hr)) { sink->AddTriangles(triangles.data(), static_cast<UINT32>(triangles.size())); hr = sink->Close(); }
        if (!check(SUCCEEDED(hr), "Attraction reference builds one unpartitioned coverage mesh")) return failures;
        context->SetTarget(expected.Get());
        context->BeginDraw();
        context->Clear(D2D1::ColorF(0, 0.f));
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
        context->FillMesh(silhouette.Get(), solid.Get());
        hr = context->EndDraw();
        context->SetTarget(nullptr);
        if (SUCCEEDED(hr)) hr = readActual->CopyFromBitmap(nullptr, actual.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = readExpected->CopyFromBitmap(nullptr, expected.Get(), nullptr);
        D2D1_MAPPED_RECT got{}, want{};
        if (SUCCEEDED(hr)) hr = readActual->Map(D2D1_MAP_OPTIONS_READ, &got);
        if (!check(SUCCEEDED(hr), "Attraction output pixels are readable")) return failures;
        hr = readExpected->Map(D2D1_MAP_OPTIONS_READ, &want);
        if (FAILED(hr)) { readActual->Unmap(); check(false, "Attraction reference pixels are readable"); return failures; }
        for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width * 4; ++x)
        {
            const int value = got.bits[static_cast<std::size_t>(y) * got.pitch + x];
            const int reference = want.bits[static_cast<std::size_t>(y) * want.pitch + x];
            if (reference) ++sampled;
            if (std::abs(value - reference) > 1) ++mismatches;
        }
        readActual->Unmap();
        readExpected->Unmap();
    }
    std::cout << "Attraction shared-surface samples=" << sampled << " mismatched channels=" << mismatches << '\n';
    check(sampled > 100000 && mismatches == 0,
        "Attraction shared edges have no holes, bright texels or repeated translucent blending, including folds");

    // A nonuniform source checks actual texture coordinates, including the
    // mirrored half of each cell, rather than only testing a solid brush.
    for (UINT y = 0; y < sourceHeight; ++y)
    for (UINT x = 0; x < sourceWidth; ++x)
        sourcePixels[static_cast<std::size_t>(y) * sourceWidth + x] =
            0xff000000u | ((x % 128) << 16) | ((y % 128) << 8) | ((x + y) % 128);
    hr = source->CopyFromMemory(nullptr, sourcePixels.data(), sourceWidth * sizeof(std::uint32_t));
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->SetTarget(actual.Get());
    context->BeginDraw();
    context->Clear(D2D1::ColorF(0, 0.f));
    if (SUCCEEDED(hr)) hr = quick_navigation_animation_rules::DrawAttractionMesh(context.Get(), source.Get(),
        attraction::MeshFrame({32, 16, 288, 208}, docks[0], 0), 0, 0, 256, 192);
    const HRESULT end = context->EndDraw();
    context->SetTarget(nullptr);
    if (SUCCEEDED(hr)) hr = end;
    if (SUCCEEDED(hr)) hr = readActual->CopyFromBitmap(nullptr, actual.Get(), nullptr);
    D2D1_MAPPED_RECT mapped{};
    if (SUCCEEDED(hr)) hr = readActual->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (check(SUCCEEDED(hr), "Attraction textured source rasterizes at the expanded endpoint"))
    {
        std::size_t wrong = 0;
        for (UINT y = 1; y < 191; ++y)
        for (UINT x = 1; x < 255; ++x)
        {
            const auto* pixel = reinterpret_cast<const std::uint32_t*>(mapped.bits + (y + 16) * mapped.pitch);
            if (pixel[x + 32] != sourcePixels[static_cast<std::size_t>(y) * sourceWidth + x]) ++wrong;
        }
        readActual->Unmap();
        check(wrong == 0, "Attraction keeps source pixels aligned across every mirrored cell and shared edge");
    }
    return failures;
}
}
