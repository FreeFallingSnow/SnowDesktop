#include "dock_refraction.h"

#include "diagnostics/diagnostic_log.h"
#include "ui/render/smooth_corner_geometry.h"
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <utility>
#include <vector>

namespace snowdesktop
{
using Microsoft::WRL::ComPtr;
namespace graphics = winrt::Windows::Graphics;
namespace wgc = graphics::Capture;

namespace
{
constexpr auto kFormat = graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
bool MonitorCaptureBounds(HMONITOR monitor, RECT& bounds)
{
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;
    bounds = info.rcMonitor;
    return true;
}

bool ValidSize(graphics::SizeInt32 size)
{
    return size.Width > 0 && size.Height > 0 &&
        size.Width <= static_cast<int>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) &&
        size.Height <= static_cast<int>(D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
}

bool CanExcludeFromCapture()
{
    static const bool supported = [] {
        using QueryVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
        const auto query = reinterpret_cast<QueryVersion>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        return query && query(&version) == 0 && version.dwBuildNumber >= 19041;
    }();
    return supported;
}

struct CaptureExclusion
{
    HWND window{};
    DWORD previous = WDA_NONE;
    bool changed = false;

    ~CaptureExclusion()
    {
        if (changed && IsWindow(window) && !SetWindowDisplayAffinity(window, previous))
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock,
                L"Could not restore the Dock window's display affinity", DiagnosticLogLevel::Warning);
    }

    void Apply(HWND target)
    {
        window = target;
        if (!window) return;
        // These are the Dock's own HWNDs. A newly created non-layered window
        // can have no queryable affinity, in which case its default is NONE.
        if (!GetWindowDisplayAffinity(window, &previous))
        {
            if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_LAYERED)
                winrt::throw_last_error();
            previous = WDA_NONE;
        }
        if (previous == WDA_EXCLUDEFROMCAPTURE) return;
        winrt::check_bool(SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE));
        changed = true;
    }
};

bool HasWindowAbove(HWND dock, const RECT& sample)
{
    unsigned remaining = 4096;
    for (HWND window = GetWindow(dock, GW_HWNDPREV); window; window = GetWindow(window, GW_HWNDPREV))
    {
        if (!remaining--) return true;
        RECT bounds{}, intersection{};
        if (!IsWindowVisible(window) || IsIconic(window) ||
            !GetWindowRect(window, &bounds) || !IntersectRect(&intersection, &bounds, &sample))
            continue;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        return true;
    }
    return false;
}
}

struct DockRefractionSource::Capture
{
    HMONITOR monitor{};
    CaptureExclusion content, backdrop;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID2D1Bitmap1> bitmap;
    graphics::DirectX::Direct3D11::IDirect3DDevice runtimeDevice{nullptr};
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool pool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    graphics::SizeInt32 size{};
    RECT bounds{};
    ULONGLONG started = GetTickCount64();

    ~Capture()
    {
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
    }

    void Start(ID3D11Device* renderDevice, HMONITOR output, HWND dock, HWND glass)
    {
        if (!renderDevice || !dock || !CanExcludeFromCapture() || !MonitorCaptureBounds(output, bounds) ||
            !wgc::GraphicsCaptureSession::IsSupported())
            winrt::throw_hresult(E_NOTIMPL);

        monitor = output;
        device = renderDevice;
        const auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForMonitor(monitor, winrt::guid_of<wgc::GraphicsCaptureItem>(),
            winrt::put_abi(item)));
        size = item.Size();
        if (!ValidSize(size)) winrt::throw_hresult(E_INVALIDARG);

        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device.As(&dxgi));
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        runtimeDevice = inspectable.as<graphics::DirectX::Direct3D11::IDirect3DDevice>();
        pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(runtimeDevice, kFormat, 2, size);
        session = pool.CreateCaptureSession(item);
        try { session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { session.IsBorderRequired(false); } catch (...) {}
        try { session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{333333}); } catch (...) {}
        content.Apply(dock);
        backdrop.Apply(glass);
        winrt::check_hresult(DwmFlush());
        session.StartCapture();
    }

    bool LoadLatest()
    {
        wgc::Direct3D11CaptureFrame frame{nullptr};
        for (int i = 0; i < 2; ++i)
        {
            auto next = pool.TryGetNextFrame();
            if (!next) break;
            if (frame) frame.Close();
            frame = std::move(next);
        }
        if (!frame)
        {
            if (!texture && GetTickCount64() - started > 5000)
                winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
            return false;
        }
        const auto content = frame.ContentSize();
        if (!ValidSize(content)) winrt::throw_hresult(E_INVALIDARG);
        RECT newBounds{};
        if (!MonitorCaptureBounds(monitor, newBounds) ||
            newBounds.right - newBounds.left != content.Width ||
            newBounds.bottom - newBounds.top != content.Height)
            winrt::throw_hresult(E_UNEXPECTED);
        if (content.Width != size.Width || content.Height != size.Height)
        {
            frame.Close();
            pool.Recreate(runtimeDevice, kFormat, 2, content);
            size = content;
            bitmap.Reset();
            texture.Reset();
            started = GetTickCount64();
            return true;
        }

        const auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> frameTexture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&frameTexture)));
        D3D11_TEXTURE2D_DESC desc{};
        frameTexture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.Width < static_cast<UINT>(size.Width) || desc.Height < static_cast<UINT>(size.Height) ||
            desc.SampleDesc.Count != 1)
            winrt::throw_hresult(E_INVALIDARG);

        D3D11_TEXTURE2D_DESC destination{};
        destination.Width = static_cast<UINT>(size.Width);
        destination.Height = static_cast<UINT>(size.Height);
        destination.MipLevels = destination.ArraySize = destination.SampleDesc.Count = 1;
        destination.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        destination.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (!texture) winrt::check_hresult(device->CreateTexture2D(&destination, nullptr, &texture));
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        const D3D11_BOX data{0, 0, 0, destination.Width, destination.Height, 1};
        context->CopySubresourceRegion(texture.Get(), 0, 0, 0, 0, frameTexture.Get(), 0, &data);
        bounds = newBounds;
        return true;
    }
};

DockRefractionSource::DockRefractionSource() = default;
DockRefractionSource::~DockRefractionSource() = default;

void DockRefractionSource::Reset() noexcept
{
    capture_.reset();
    retryAt_ = 0;
}

bool DockRefractionSource::Update(ID3D11Device* device, HMONITOR monitor, HWND content, HWND backdrop) noexcept
{
    const bool hadFrame = capture_ && capture_->texture;
    if (capture_ && (capture_->monitor != monitor || capture_->device.Get() != device ||
            capture_->content.window != content || capture_->backdrop.window != backdrop))
        Reset();
    if (GetTickCount64() < retryAt_) return false;
    try
    {
        if (!capture_)
        {
            capture_ = std::make_unique<Capture>();
            capture_->Start(device, monitor, content, backdrop);
        }
        return capture_->LoadLatest() || (hadFrame && !capture_->texture);
    }
    catch (...)
    {
        const HRESULT error = winrt::to_hresult();
        capture_.reset();
        retryAt_ = GetTickCount64() + 2000;
        wchar_t message[100]{};
        swprintf_s(message, L"Dock refraction desktop capture unavailable (0x%08X)",
            static_cast<unsigned>(error));
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message);
        return hadFrame;
    }
}

ID2D1Bitmap1* DockRefractionSource::Bitmap(ID2D1DeviceContext* context) noexcept
{
    if (!capture_ || !capture_->texture) return nullptr;
    if (!capture_->bitmap)
    {
        ComPtr<IDXGISurface> surface;
        if (FAILED(capture_->texture.As(&surface))) return nullptr;
        auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        if (FAILED(context->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &capture_->bitmap)))
            return nullptr;
    }
    return capture_->bitmap.Get();
}

RECT DockRefractionSource::Bounds() const noexcept
{
    return capture_ ? capture_->bounds : RECT{};
}

bool DockRefractionSource::CanSample(RECT panel, float padding) const noexcept
{
    if (!capture_ || !capture_->texture) return false;
    const RECT bounds = Bounds();
    if (IsRectEmpty(&panel) || panel.left < bounds.left || panel.top < bounds.top ||
        panel.right > bounds.right || panel.bottom > bounds.bottom)
        return false;
    const auto margin = static_cast<int>(std::ceil(padding));
    InflateRect(&panel, margin, margin);
    IntersectRect(&panel, &panel, &bounds);
    return !HasWindowAbove(capture_->content.window, panel);
}

bool DockRefractionMaterial::Draw(ID2D1DeviceContext* context, DockRefractionSource& source,
    RECT panel, POINT screenOffset, float radius, float blur, float widthSetting, float strength)
{
    if (!std::isfinite(widthSetting) || !std::isfinite(strength) || !std::isfinite(blur) ||
        !std::isfinite(radius) || strength <= 0.f)
        return false;
    const float deviation = std::round(std::clamp(blur, 0.f, 48.f));
    const float peak = std::clamp(strength, 0.f, 24.f);
    RECT screen = panel;
    OffsetRect(&screen, screenOffset.x, screenOffset.y);
    if (!source.CanSample(screen, deviation * 3.f + peak)) return false;
    auto* background = source.Bitmap(context);
    if (!background) return false;
    const RECT bounds = source.Bounds();

    const auto width = static_cast<UINT>(screen.right - screen.left);
    const auto height = static_cast<UINT>(screen.bottom - screen.top);
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    const float band = std::min(std::clamp(widthSetting, 1.f, 32.f), std::min(w, h) * 0.5f);
    if (!displacement_ || size_.width != width || size_.height != height ||
        radius_ != radius || band_ != band)
    {
        std::vector<UINT32> pixels(static_cast<std::size_t>(width) * height);
        const smooth_corners::Distance distance(w, h, radius);
        const auto encode = [](float value) {
            return static_cast<UINT32>(std::lround(std::clamp(0.5f + value, 0.f, 1.f) * 255.f));
        };
        for (UINT y = 0; y < height; ++y)
        {
            const auto py = static_cast<float>(y) + 0.5f;
            for (UINT x = 0; x < width; ++x)
            {
                const auto px = static_cast<float>(x) + 0.5f;
                const float depth = -distance.At(px, py);
                float dx = 0, dy = 0;
                if (depth >= -1.f && depth < band)
                {
                    const float t = 1.f - std::clamp(depth / band, 0.f, 1.f);
                    const float weight = t * t * (3.f - 2.f * t);
                    dx = (distance.At(px + 0.5f, py) - distance.At(px - 0.5f, py)) * weight * 0.5f;
                    dy = (distance.At(px, py + 0.5f) - distance.At(px, py - 0.5f)) * weight * 0.5f;
                }
                pixels[static_cast<std::size_t>(y) * width + x] = 0xff000000u | (encode(dx) << 16) | (encode(dy) << 8);
            }
        }
        ComPtr<ID2D1Bitmap1> field;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(context->CreateBitmap({width, height}, pixels.data(),
                width * static_cast<UINT>(sizeof(UINT32)), &properties, &field)))
            return false;
        displacement_ = std::move(field);
        size_ = {width, height};
        radius_ = radius;
        band_ = band;
    }

    ComPtr<ID2D1Effect> blurEffect, align, displacement;
    if (FAILED(context->CreateEffect(CLSID_D2D1GaussianBlur, &blurEffect)) ||
        FAILED(context->CreateEffect(CLSID_D2D12DAffineTransform, &align)) ||
        FAILED(context->CreateEffect(CLSID_D2D1DisplacementMap, &displacement)))
        return false;
    const auto translation = D2D1::Matrix3x2F::Translation(
        static_cast<float>(bounds.left - screen.left), static_cast<float>(bounds.top - screen.top));
    if (FAILED(blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, deviation)) ||
        FAILED(blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED)) ||
        FAILED(blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD)) ||
        FAILED(align->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, translation)) ||
        FAILED(displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, 2.f * peak)) ||
        FAILED(displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R)) ||
        FAILED(displacement->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G)))
        return false;
    blurEffect->SetInput(0, background);
    align->SetInputEffect(0, blurEffect.Get());
    displacement->SetInputEffect(0, align.Get());
    displacement->SetInput(1, displacement_.Get());

    const auto rect = D2D1::RectF(static_cast<float>(panel.left), static_cast<float>(panel.top),
        static_cast<float>(panel.right), static_cast<float>(panel.bottom));
    ComPtr<ID2D1Factory> factory;
    context->GetFactory(&factory);
    const auto clip = smooth_corners::Geometry(factory.Get(), rect, radius);
    if (!clip) return false;
    context->PushLayer(D2D1::LayerParameters1(rect, clip.Get()), nullptr);
    context->DrawImage(displacement.Get(), D2D1::Point2F(rect.left, rect.top), D2D1::RectF(0, 0, w, h));
    context->PopLayer();
    return true;
}
}
