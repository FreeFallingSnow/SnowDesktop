#include "taskbar_classic_surface.h"
#include <d3d11.h>
#include <dxgi.h>

namespace snowdesktop::taskbar_hook::native
{
using Microsoft::WRL::ComPtr;

void ClassicSurface::Reset()
{
    if (target_) target_->SetRoot(nullptr);
    if (device_) device_->Commit();
    visual_.Reset(); target_.Reset(); device_.Reset(); factory_.Reset();
    window_ = nullptr;
}

HRESULT ClassicSurface::Draw(HWND window, const TargetAppearance& style)
{
    const auto gradient = DecodeGradient(style.gradient);
    if (!gradient.enabled && style.borderAlpha <= 0)
    {
        Reset();
        return S_OK;
    }
    RECT bounds{};
    if (!GetClientRect(window, &bounds)) return HRESULT_FROM_WIN32(GetLastError());
    const LONG width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return S_FALSE;
    HRESULT hr = S_OK;
    if (!device_ || window_ != window)
    {
        Reset();
        ComPtr<ID3D11Device> d3d;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &d3d, nullptr, nullptr);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &d3d, nullptr, nullptr);
        ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(hr)) hr = d3d.As(&dxgi);
        if (SUCCEEDED(hr)) hr = DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&device_));
        if (SUCCEEDED(hr)) hr = device_->CreateTargetForHwnd(window, FALSE, &target_);
        if (SUCCEEDED(hr)) hr = device_->CreateVisual(&visual_);
        if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
        if (FAILED(hr)) { Reset(); return hr; }
        window_ = window;
    }

    ComPtr<IDCompositionSurface> surface;
    hr = device_->CreateSurface(static_cast<UINT>(width), static_cast<UINT>(height),
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset{};
    if (SUCCEEDED(hr)) hr = surface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1RenderTarget> render;
    auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    hr = factory_->CreateDxgiSurfaceRenderTarget(dxgiSurface.Get(), &properties, &render);
    if (SUCCEEDED(hr))
    {
        render->BeginDraw();
        render->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(offset.x), static_cast<float>(offset.y)));
        const auto rectangle = D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));
        render->PushAxisAlignedClip(rectangle, D2D1_ANTIALIAS_MODE_ALIASED);
        render->Clear(D2D1::ColorF(0, 0));
        ComPtr<ID2D1SolidColorBrush> solid;
        hr = render->CreateSolidColorBrush(D2D1::ColorF(style.borderRed,
            style.borderGreen, style.borderBlue, style.borderAlpha), &solid);
        if (SUCCEEDED(hr) && gradient.enabled)
        {
            std::vector<D2D1_GRADIENT_STOP> stops;
            for (const auto& stop : gradient.stops)
                stops.push_back({static_cast<float>(stop.position),
                    D2D1::ColorF(stop.color, static_cast<float>(stop.opacity))});
            ComPtr<ID2D1GradientStopCollection> collection;
            hr = render->CreateGradientStopCollection(stops.data(), static_cast<UINT>(stops.size()), &collection);
            const auto line = ResolvePanelGradientLine(gradient, width, height);
            ComPtr<ID2D1LinearGradientBrush> brush;
            if (SUCCEEDED(hr)) hr = render->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(static_cast<float>(line.x1), static_cast<float>(line.y1)),
                    D2D1::Point2F(static_cast<float>(line.x2), static_cast<float>(line.y2))),
                collection.Get(), &brush);
            if (SUCCEEDED(hr)) render->FillRectangle(rectangle, brush.Get());
        }
        // Solid tint is already in ACCENT_POLICY. Drawing it here as well
        // would apply its opacity twice wherever this visual is visible.
        if (SUCCEEDED(hr) && style.borderAlpha > 0)
        {
            const float stroke = static_cast<float>(GetDpiForWindow(window)) / 96.0f;
            const float half = stroke / 2;
            render->DrawRectangle(D2D1::RectF(half, half, static_cast<float>(width) - half,
                static_cast<float>(height) - half), solid.Get(), stroke);
        }
        render->PopAxisAlignedClip();
        const HRESULT drawResult = render->EndDraw();
        if (SUCCEEDED(hr)) hr = drawResult;
    }
    const HRESULT endResult = surface->EndDraw();
    if (SUCCEEDED(hr)) hr = endResult;
    if (SUCCEEDED(hr)) hr = visual_->SetContent(surface.Get());
    if (SUCCEEDED(hr)) hr = target_->SetRoot(visual_.Get());
    if (SUCCEEDED(hr)) hr = device_->Commit();
    if (FAILED(hr)) Reset();
    return hr;
}
}
