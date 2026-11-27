#include "dock_window_thumbnail.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <utility>

namespace snowdesktop::dock_window_thumbnail
{
namespace
{
struct Pixels
{
    D2D1_SIZE_U size{};
    std::vector<std::uint32_t> data;
    float aspectRatio = 0.0f;
};

struct WindowDib
{
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;

    ~WindowDib()
    {
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
};

bool AllowsPreview(WindowIdentity window) noexcept
{
    DWORD affinity = WDA_NONE;
    return window.IsCurrent() &&
        (!GetWindowDisplayAffinity(window.window, &affinity) || affinity == WDA_NONE);
}

bool AllowsRedirectedSurface(WindowIdentity window) noexcept
{
    DWORD affinity = WDA_NONE;
    return window.IsCurrent() &&
        GetWindowDisplayAffinity(window.window, &affinity) && affinity == WDA_NONE;
}

SIZE ReadWindowSize(HWND window, bool iconic) noexcept
{
    RECT bounds{};
    if (iconic)
    {
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(window, &placement)) return {};
        bounds = placement.rcNormalPosition;
    }
    else if (!GetWindowRect(window, &bounds))
        return {};
    return {bounds.right - bounds.left, bounds.bottom - bounds.top};
}

float WindowAspectRatio(SIZE size) noexcept
{
    return size.cx > 0 && size.cy > 0
        ? static_cast<float>(size.cx) / static_cast<float>(size.cy) : 0.0f;
}

bool IsUsableSize(int width, int height)
{
    constexpr int maxSourcePixels = 16 * 1024 * 1024;
    return width >= 32 && height >= 32 && width <= maxSourcePixels / height;
}

Pixels SamplePixels(const void* buffer, size_t rowPitch, int width, int height)
{
    constexpr int maxThumbnailEdge = 256;
    const int divisor = std::max({width, height, maxThumbnailEdge});
    const auto targetWidth = static_cast<UINT32>(std::max(1, width * maxThumbnailEdge / divisor));
    const auto targetHeight = static_cast<UINT32>(std::max(1, height * maxThumbnailEdge / divisor));
    Pixels result{{targetWidth, targetHeight}, {}, WindowAspectRatio({width, height})};
    result.data.resize(static_cast<size_t>(targetWidth) * targetHeight);
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    bool hasContent = false;
    for (UINT32 y = 0; y < targetHeight; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            bytes + static_cast<size_t>(y) * height / targetHeight * rowPitch);
        for (UINT32 x = 0; x < targetWidth; ++x)
        {
            const auto pixel = row[static_cast<size_t>(x) * width / targetWidth] & 0x00ffffffu;
            result.data[static_cast<size_t>(y) * targetWidth + x] = pixel;
            // Ignore chrome when testing whether the client was drawn.
            hasContent |= y > targetHeight / 4 && y < targetHeight * 3 / 4 &&
                x > targetWidth / 8 && x < targetWidth * 7 / 8 && pixel != 0;
        }
    }
    if (!hasContent) return {};
    return result;
}

Pixels CaptureRedirectedSurface(WindowIdentity source)
{
    // An optional DWM surface may still be available when the application
    // cannot paint a minimized window. Require explicit capture permission.
    using ReadSharedSurface = BOOL(WINAPI*)(HWND, HANDLE*, LUID*, ULONG*, ULONG*, ULONGLONG*);
    static const auto readSurface = std::bit_cast<ReadSharedSurface>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "DwmGetDxSharedSurface"));
    if (!readSurface || !AllowsRedirectedSurface(source)) return {};

    HANDLE sharedHandle = nullptr;
    LUID luid{};
    ULONG format = 0, presentFlags = 0;
    ULONGLONG updateId = 0;
    if (!readSurface(source.window, &sharedHandle, &luid, &format, &presentFlags, &updateId) ||
        !sharedHandle)
        return {};

    using Microsoft::WRL::ComPtr;
    struct CaptureDevice
    {
        LUID luid{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
    };
    thread_local CaptureDevice capture;
    if (!capture.device || capture.luid.LowPart != luid.LowPart || capture.luid.HighPart != luid.HighPart)
    {
        capture = {};
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                &capture.device, nullptr, &capture.context)))
            return {};
        capture.luid = luid;
    }

    // This is a legacy DXGI shared handle, owned by the underlying surface.
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(capture.device->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&texture)))) return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    constexpr UINT restricted = D3D11_RESOURCE_MISC_RESTRICTED_CONTENT |
        D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE | D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER |
        D3D11_RESOURCE_MISC_HW_PROTECTED;
    if ((desc.MiscFlags & restricted) || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM) ||
        !IsUsableSize(static_cast<int>(desc.Width), static_cast<int>(desc.Height)) ||
        !AllowsRedirectedSurface(source))
        return {};

    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(capture.device->CreateTexture2D(&desc, nullptr, &staging))) return {};
    capture.context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture.Get(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(capture.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    Pixels pixels = SamplePixels(mapped.pData, mapped.RowPitch,
        static_cast<int>(desc.Width), static_cast<int>(desc.Height));
    capture.context->Unmap(staging.Get(), 0);
    if (!AllowsRedirectedSurface(source)) return {};
    return pixels;
}

Pixels PrintWindowPixels(WindowIdentity source, bool iconic)
{
    if (!AllowsPreview(source)) return {};
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(source.window, WM_NULL, 0, 0,
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 100, &reply))
        return {};
    const SIZE sourceSize = ReadWindowSize(source.window, iconic);
    const int width = static_cast<int>(sourceSize.cx);
    const int height = static_cast<int>(sourceSize.cy);
    if (!IsUsableSize(width, height)) return {};

    WindowDib dib;
    if (!dib.dc) return {};
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* buffer = nullptr;
    dib.bitmap = CreateDIBSection(dib.dc, &info, DIB_RGB_COLORS, &buffer, nullptr, 0);
    if (!dib.bitmap || !buffer) return {};
    dib.previous = SelectObject(dib.dc, dib.bitmap);
    if (!dib.previous || dib.previous == HGDI_ERROR) return {};
    auto* pixels = static_cast<std::uint32_t*>(buffer);
    // Render-full-content handles composed windows. Some Win32 windows
    // still require the ordinary WM_PRINT path to draw their client area.
    constexpr UINT printModes[]{PW_RENDERFULLCONTENT, 0};
    for (const UINT flags : printModes)
    {
        std::fill_n(pixels, static_cast<size_t>(width) * height, 0u);
        if (!PrintWindow(source.window, dib.dc, flags) || !GdiFlush()) continue;
        if (!AllowsPreview(source)) return {};
        auto result = SamplePixels(buffer, static_cast<size_t>(width) * sizeof(*pixels), width, height);
        if (!result.data.empty()) return result;
    }
    return {};
}

Pixels CapturePixels(WindowIdentity source, bool minimized)
{
    if (!AllowsPreview(source)) return {};
    const bool iconic = IsIconic(source.window) != FALSE;
    if (iconic && !minimized) return {};
    auto result = iconic ? CaptureRedirectedSurface(source) : PrintWindowPixels(source, false);
    if (!result.data.empty()) return result;
    return iconic ? PrintWindowPixels(source, true) : CaptureRedirectedSurface(source);
}
}

WindowIdentity WindowIdentity::Capture(HWND window) noexcept
{
    WindowIdentity identity{window};
    if (window) identity.thread = GetWindowThreadProcessId(window, &identity.process);
    return identity;
}

bool WindowIdentity::IsCurrent() const noexcept
{
    return process && thread && *this == Capture(window);
}

std::wstring WindowIdentity::Key() const
{
    return L"task-window:" + std::to_wstring(reinterpret_cast<UINT_PTR>(window)) +
        L":" + std::to_wstring(process) + L":" + std::to_wstring(thread);
}

Thumbnail::Thumbnail(WindowIdentity source) : source_(source)
{
    if (!source.IsCurrent()) return;
    const bool iconic = IsIconic(source.window) != FALSE;
    const SIZE size = ReadWindowSize(source.window, iconic);
    if (!iconic) liveSize_ = size;
    const float measured = WindowAspectRatio(size);
    if (measured > 0.0f) aspectRatio_ = measured;
    layoutFrom_ = layoutTo_ = TargetLayoutAspectRatio();
}

float Thumbnail::LayoutAspectRatio() const noexcept
{
    return std::lerp(layoutFrom_, layoutTo_, layoutProgress_.Amount());
}

float Thumbnail::TargetLayoutAspectRatio() const noexcept
{
    // Keep very narrow windows easy to hit and bound the space a single
    // panoramic window can occupy. The image still fits without distortion.
    return std::clamp(aspectRatio_, 0.55f, 4.0f);
}

void Thumbnail::RetargetLayout(double now, bool animate, double durationScale)
{
    const float target = TargetLayoutAspectRatio();
    if (target == layoutTo_ && animate) return;
    layoutFrom_ = LayoutAspectRatio();
    layoutTo_ = target;
    layoutProgress_ = {};
    layoutProgress_.SetVisible(true, now, animate && layoutFrom_ != target, durationScale);
}

void Thumbnail::AdvanceLayout(double now, bool animate)
{
    if (animate)
        layoutProgress_.Advance(now);
    else
        layoutProgress_.SetVisible(true, now, false);
}

bool Thumbnail::IsLayoutAnimating() const
{
    return layoutProgress_.IsAnimating();
}

ID2D1Bitmap1* Thumbnail::Bitmap(ID2D1DeviceContext& context)
{
    if (pixels_.empty()) return nullptr;
    Microsoft::WRL::ComPtr<ID2D1Device> device;
    context.GetDevice(&device);
    if (device.Get() != device_.Get())
    {
        bitmap_.Reset();
        device_ = std::move(device);
    }
    if (!bitmap_)
    {
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        context.CreateBitmap(size_, pixels_.data(), size_.width * 4u,
            &properties, bitmap_.ReleaseAndGetAddressOf());
    }
    return bitmap_.Get();
}

void ThumbnailCache::Refresh(const std::shared_ptr<Thumbnail>& thumbnail, bool minimized,
    bool foreground, HWND notifyWindow, UINT message)
{
    if (!thumbnail || !notifyWindow) return;
    auto& state = *thumbnail;
    if (state.epoch_ != epoch_)
    {
        state.epoch_ = epoch_;
        state.pending_ = false;
        state.refreshNeeded_ = true;
        state.nextAttempt_ = 0;
    }
    if (state.minimized_ != minimized || (foreground && !state.foreground_))
    {
        state.refreshNeeded_ = true;
        state.nextAttempt_ = 0;
    }
    state.minimized_ = minimized;
    state.foreground_ = foreground;
    if (state.pending_) return;
    const auto now = GetTickCount64();
    if (now < state.nextAttempt_) return;
    if (!minimized)
    {
        const SIZE size = ReadWindowSize(state.source_.window, false);
        if (size.cx > 0 && size.cy > 0 &&
            (size.cx != state.liveSize_.cx || size.cy != state.liveSize_.cy))
        {
            state.liveSize_ = size;
            state.refreshNeeded_ = true;
        }
    }
    // Keep the last usable frame while iconic. The normal window can
    // provide its next frame on restore without replacing the task entry.
    if (!state.pixels_.empty() && (minimized || (!state.refreshNeeded_ && !foreground)))
    {
        state.nextAttempt_ = now + 3000;
        if (!AllowsPreview(state.source_))
        {
            state.pixels_.clear();
            state.bitmap_.Reset();
            changes_.repaint = true;
            PostMessageW(notifyWindow, message, 0, 0);
        }
        return;
    }
    state.nextAttempt_ = now + (state.pixels_.empty() && minimized ? 3000 : 1000);
    const auto source = state.source_;
    const auto epoch = epoch_;
    const auto weak = std::weak_ptr{thumbnail};
    state.pending_ = work_.Submit(source.Key() + L":" +
        std::to_wstring(reinterpret_cast<UINT_PTR>(thumbnail.get())),
        [source, minimized, weak] {
            return weak.expired() ? Pixels{} : CapturePixels(source, minimized);
        }, [this, source, minimized, weak, epoch](Pixels pixels) {
            const auto target = weak.lock();
            if (!target || epoch != epoch_) return;
            target->pending_ = false;
            if (!AllowsPreview(source))
            {
                changes_.repaint |= !target->pixels_.empty();
                target->pixels_.clear();
                target->bitmap_.Reset();
                return;
            }
            if (pixels.data.empty() || (minimized && !target->minimized_)) return;
            const float previousLayoutAspect = target->TargetLayoutAspectRatio();
            target->aspectRatio_ = pixels.aspectRatio;
            changes_.layoutChanged |= target->TargetLayoutAspectRatio() != previousLayoutAspect;
            if (target->size_.width == pixels.size.width && target->size_.height == pixels.size.height &&
                target->pixels_ == pixels.data) return;
            target->size_ = pixels.size;
            target->pixels_ = std::move(pixels.data);
            target->bitmap_.Reset();
            changes_.repaint = true;
        }, notifyWindow, message);
    if (state.pending_) state.refreshNeeded_ = false;
}

void ThumbnailCache::Clear()
{
    work_.Cancel();
    ++epoch_;
    changes_ = {};
}

ThumbnailCache::Changes ThumbnailCache::Drain()
{
    work_.Drain();
    return std::exchange(changes_, {});
}
}
