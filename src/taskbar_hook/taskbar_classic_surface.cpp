#include "taskbar_classic_surface.h"
#include "taskbar_classic_appearance.h"
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>

namespace snowdesktop::taskbar_hook::native
{
using Microsoft::WRL::ComPtr;

namespace
{
LRESULT CALLBACK BackdropProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateBackdrop()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&BackdropProc), &module)) return nullptr;
    static const ATOM atom = [module] {
        WNDCLASSW registration{};
        registration.hInstance = module;
        registration.lpfnWndProc = BackdropProc;
        registration.lpszClassName = L"SnowDesktopClassicTaskbarBackdrop";
        return RegisterClassW(&registration);
    }();
    if (!atom) return nullptr;
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_LAYERED | WS_EX_TRANSPARENT, MAKEINTATOM(atom), nullptr,
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, module, nullptr);
}

HRESULT ApplyBackdropMaterial(HWND window, const TargetAppearance& style)
{
    struct CompositionData { int attribute; void* data; SIZE_T size; };
    using SetComposition = BOOL(WINAPI*)(HWND, CompositionData*);
    const auto set = reinterpret_cast<SetComposition>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!set) return E_NOTIMPL;
    auto accent = MakeClassicAccentPolicy(style);
    CompositionData data{19, &accent, sizeof(accent)};
    if (set(window, &data)) return S_OK;
    if (accent.state == 4)
    {
        accent = MakeClassicAccentPolicy(style, false);
        if (set(window, &data)) return S_OK;
    }
    return E_FAIL;
}
}

void ClassicSurface::Reset()
{
    if (window_ && GetPropW(window_, kClassicBackdropProperty) == backdrop_)
        RemovePropW(window_, kClassicBackdropProperty);
    if (backdrop_) ShowWindow(backdrop_, SW_HIDE);
    if (target_) target_->SetRoot(nullptr);
    if (device_) device_->Commit();
    visual_.Reset(); target_.Reset(); device_.Reset(); factory_.Reset();
    if (backdrop_) DestroyWindow(backdrop_);
    backdrop_ = nullptr;
    window_ = nullptr;
}

HRESULT ClassicSurface::Synchronize(HWND window)
{
    if (!backdrop_) return S_OK;
    DWORD cloaked = 0;
    RECT bounds{};
    if (!IsWindowVisible(window) ||
        FAILED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) || cloaked ||
        !GetClientRect(window, &bounds))
    {
        ShowWindow(backdrop_, SW_HIDE);
        return S_OK;
    }
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
    MONITORINFO monitor{sizeof(monitor)};
    RECT visible{};
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) ||
        !IntersectRect(&visible, &bounds, &monitor.rcMonitor))
    {
        ShowWindow(backdrop_, SW_HIDE);
        return S_OK;
    }
    // Auto-hide can move most of the taskbar onto an adjacent monitor. Clip
    // to this monitor so its backdrop cannot bleed onto that other display.
    HRGN region = CreateRectRgn(visible.left - bounds.left, visible.top - bounds.top,
        visible.right - bounds.left, visible.bottom - bounds.top);
    if (!region) return E_OUTOFMEMORY;
    if (!SetWindowRgn(backdrop_, region, FALSE))
    { DeleteObject(region); return E_FAIL; }
    RECT previous{};
    GetWindowRect(backdrop_, &previous);
    UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW;
    if (EqualRect(&previous, &bounds)) flags |= SWP_NOMOVE | SWP_NOSIZE;
    if (GetWindow(backdrop_, GW_HWNDPREV) == window) flags |= SWP_NOZORDER;
    return SetWindowPos(backdrop_, window, bounds.left, bounds.top,
        bounds.right - bounds.left, bounds.bottom - bounds.top, flags) ? S_OK : E_FAIL;
}

HRESULT ClassicSurface::Draw(HWND window, const TargetAppearance& style)
{
    const auto gradient = DecodeGradient(style.gradient);
    if (!NeedsClassicSurface(style))
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
        backdrop_ = CreateBackdrop();
        if (!backdrop_) return E_FAIL;
        window_ = window;
        if (!SetLayeredWindowAttributes(backdrop_, 0, 255, LWA_ALPHA) ||
            !SetPropW(window, kClassicBackdropProperty, backdrop_))
        { Reset(); return E_FAIL; }
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
        if (SUCCEEDED(hr)) hr = device_->CreateTargetForHwnd(backdrop_, FALSE, &target_);
        if (SUCCEEDED(hr)) hr = device_->CreateVisual(&visual_);
        if (SUCCEEDED(hr)) hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf());
        if (FAILED(hr)) { Reset(); return hr; }
    }

    hr = ApplyBackdropMaterial(backdrop_, style);
    if (FAILED(hr)) { Reset(); return hr; }

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
    if (SUCCEEDED(hr)) hr = Synchronize(window);
    if (FAILED(hr)) Reset();
    return hr;
}
}
