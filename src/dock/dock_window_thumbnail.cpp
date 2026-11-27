#include "dock_window_thumbnail.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <utility>

namespace snowdesktop::dock_window_thumbnail
{
namespace
{
struct Pixels
{
    D2D1_SIZE_U size{};
    std::vector<std::uint32_t> data;
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

namespace capture = winrt::Windows::Graphics::Capture;

struct WindowCapture
{
    capture::Direct3D11CaptureFramePool pool{nullptr};
    capture::GraphicsCaptureSession session{nullptr};

    ~WindowCapture()
    {
        try { if (session) session.Close(); } catch (...) {}
        try { if (pool) pool.Close(); } catch (...) {}
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

bool IsUsableSize(int width, int height)
{
    constexpr int maxSourcePixels = 16 * 1024 * 1024;
    return width >= 32 && height >= 32 && width <= maxSourcePixels / height;
}

bool ColorsDifferVisibly(std::uint32_t first, std::uint32_t second)
{
    constexpr int contrast = 24;
    for (const unsigned shift : {0u, 8u, 16u})
        if (std::abs(static_cast<int>((first >> shift) & 0xffu) -
                     static_cast<int>((second >> shift) & 0xffu)) >= contrast)
            return true;
    return false;
}

Pixels SamplePixels(const void* buffer, size_t rowPitch, int width, int height)
{
    constexpr int maxThumbnailEdge = 256;
    const int divisor = std::max({width, height, maxThumbnailEdge});
    const auto targetWidth = static_cast<UINT32>(std::max(1, width * maxThumbnailEdge / divisor));
    const auto targetHeight = static_cast<UINT32>(std::max(1, height * maxThumbnailEdge / divisor));
    Pixels result{{targetWidth, targetHeight}, {}};
    result.data.resize(static_cast<size_t>(targetWidth) * targetHeight);
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    const UINT32 innerLeft = targetWidth / 8;
    const UINT32 innerRight = targetWidth - innerLeft;
    UINT32 detailRows = 0;
    for (UINT32 y = 0; y < targetHeight; ++y)
    {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            bytes + static_cast<size_t>(y) * height / targetHeight * rowPitch);
        std::uint32_t previous = 0;
        unsigned edges = 0;
        for (UINT32 x = 0; x < targetWidth; ++x)
        {
            const auto pixel = row[static_cast<size_t>(x) * width / targetWidth] & 0x00ffffffu;
            result.data[static_cast<size_t>(y) * targetWidth + x] = pixel;
            if (x > innerLeft && x < innerRight)
                edges += ColorsDifferVisibly(previous, pixel);
            previous = pixel;
        }
        if (edges >= 3 && y >= targetHeight / 8 && y < targetHeight - targetHeight / 8)
            ++detailRows;
    }
    // Initial minimized frames can contain solid horizontal bands and no
    // client content. A nonblack pixel on its own does not validate a frame.
    if (detailRows < std::max(1u, targetHeight / 12)) return {};
    return result;
}

Pixels ReadTexture(ID3D11Texture2D& texture, ID3D11Device& device, ID3D11DeviceContext& context,
    int width, int height)
{
    D3D11_TEXTURE2D_DESC desc{};
    texture.GetDesc(&desc);
    constexpr UINT restricted = D3D11_RESOURCE_MISC_RESTRICTED_CONTENT |
        D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE | D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER |
        D3D11_RESOURCE_MISC_HW_PROTECTED;
    if ((desc.MiscFlags & restricted) || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM) ||
        !IsUsableSize(width, height) ||
        desc.Width < static_cast<UINT>(width) || desc.Height < static_cast<UINT>(height))
        return {};

    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device.CreateTexture2D(&desc, nullptr, &staging))) return {};
    const D3D11_BOX bounds{0, 0, 0, desc.Width, desc.Height, 1};
    context.CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, &texture, 0, &bounds);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context.Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    Pixels pixels = SamplePixels(mapped.pData, mapped.RowPitch, width, height);
    context.Unmap(staging.Get(), 0);
    return pixels;
}

Pixels CaptureRedirectedSurface(WindowIdentity source, bool iconic)
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
    // Release graphics resources in ordinary worker code. A thread-local D3D
    // device would be released in a TLS destructor under the loader lock.
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, nullptr, &context)))
        return {};

    // This is a legacy DXGI shared handle, owned by the underlying surface.
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->OpenSharedResource(sharedHandle, IID_PPV_ARGS(&texture)))) return {};
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const int width = static_cast<int>(desc.Width);
    const int height = static_cast<int>(desc.Height);
    if (iconic || IsIconic(source.window))
    {
        // DWM can retain just the minimized title bar as a redirected
        // surface. Text and caption buttons still contain color detail.
        const SIZE expected = ReadWindowSize(source.window, true);
        if (width < expected.cx / 2 || height < expected.cy / 2) return {};
    }
    if (!AllowsRedirectedSurface(source)) return {};
    auto pixels = ReadTexture(*texture.Get(), *device.Get(), *context.Get(), width, height);
    if (!AllowsRedirectedSurface(source)) return {};
    return pixels;
}

Pixels CaptureVisibleWindow(WindowIdentity source)
{
    namespace directx = winrt::Windows::Graphics::DirectX;
    if (!IsWindowVisible(source.window) || IsIconic(source.window) || !AllowsRedirectedSurface(source))
        return {};
    try
    {
        if (!capture::GraphicsCaptureSession::IsSupported()) return {};
        capture::GraphicsCaptureItem item{nullptr};
        const auto interop = winrt::get_activation_factory<capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        if (FAILED(interop->CreateForWindow(source.window,
                winrt::guid_of<capture::GraphicsCaptureItem>(), winrt::put_abi(item))) || !item)
            return {};
        const auto size = item.Size();
        if (!IsUsableSize(size.Width, size.Height)) return {};

        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                device.put(), nullptr, context.put()))) return {};
        winrt::com_ptr<IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
            device.as<IDXGIDevice>().get(), inspectable.put()));
        const auto graphicsDevice = inspectable.as<directx::Direct3D11::IDirect3DDevice>();
        WindowCapture captureWork;
        captureWork.pool = capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            graphicsDevice, directx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        captureWork.session = captureWork.pool.CreateCaptureSession(item);
        if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled"))
            captureWork.session.IsCursorCaptureEnabled(false);
        if (!AllowsRedirectedSurface(source) || IsIconic(source.window)) return {};
        captureWork.session.StartCapture();
        const ULONGLONG deadline = GetTickCount64() + 180;
        do
        {
            if (!AllowsRedirectedSurface(source) || IsIconic(source.window)) return {};
            if (auto frame = captureWork.pool.TryGetNextFrame())
            {
                const auto content = frame.ContentSize();
                const auto surface = frame.Surface().as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> texture;
                winrt::check_hresult(surface->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
                auto pixels = ReadTexture(
                    *texture.get(), *device.get(), *context.get(), content.Width, content.Height);
                frame.Close();
                if (!pixels.data.empty() && AllowsRedirectedSurface(source)) return pixels;
            }
            Sleep(8);
        } while (GetTickCount64() < deadline);
    }
    catch (const winrt::hresult_error&)
    {
        // Capture availability and permission can change with the window.
    }
    return {};
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
    constexpr UINT printModes[]{PW_RENDERFULLCONTENT, PW_RENDERFULLCONTENT | PW_CLIENTONLY, 0};
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

Pixels CapturePixels(WindowIdentity source, bool allowGraphicsCapture)
{
    if (!AllowsPreview(source)) return {};
    const bool iconic = IsIconic(source.window) != FALSE;
    auto result = iconic ? CaptureRedirectedSurface(source, true) : PrintWindowPixels(source, false);
    if (!result.data.empty()) return result;
    result = iconic ? PrintWindowPixels(source, true) : CaptureRedirectedSurface(source, false);
    if (!result.data.empty()) return result;
    return !iconic && allowGraphicsCapture ? CaptureVisibleWindow(source) : Pixels{};
}
}

D2D1_RECT_F CardFrame(RECT cell, int edge, bool vertical)
{
    const float size = static_cast<float>(edge);
    const float width = vertical ? size : std::min(size * kHorizontalCardAspectRatio,
        static_cast<float>(std::max(1L, cell.right - cell.left)));
    const float height = vertical ? size / kHorizontalCardAspectRatio : size;
    const float left = (cell.left + cell.right - width) * 0.5f;
    const float top = (cell.top + cell.bottom - height) * 0.5f;
    return D2D1::RectF(left, top, left + width, top + height);
}

RECT CardAnchor(RECT cell, int edge, bool vertical)
{
    const auto frame = CardFrame(cell, edge, vertical);
    return {std::lround(frame.left), std::lround(frame.top),
        std::lround(frame.right), std::lround(frame.bottom)};
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

void ThumbnailCache::SetTrackedWindows(std::span<const TrackedWindow> windows)
{
    decltype(windows_) tracked;
    tracked.reserve(windows.size());
    for (const auto& [source, minimized] : windows)
    {
        auto cached = Find(source.window);
        if (!cached || cached->source_ != source)
            cached = std::make_shared<Thumbnail>(source);
        cached->minimizedInDock_ = minimized;
        tracked.emplace(source.window, std::move(cached));
    }
    windows_.swap(tracked);
}

std::shared_ptr<Thumbnail> ThumbnailCache::Find(HWND window) const
{
    const auto found = windows_.find(window);
    return found != windows_.end() ? found->second : nullptr;
}

bool ThumbnailCache::Refresh(HWND foreground, HWND notifyWindow, UINT message)
{
    bool stateChanged = false;
    const auto refresh = [&](const auto& thumbnail, bool active) {
        if (!thumbnail->source_.IsCurrent()) return;
        const bool minimized = IsIconic(thumbnail->source_.window) != FALSE;
        stateChanged |= minimized != thumbnail->minimizedInDock_;
        Refresh(thumbnail, minimized, active, notifyWindow, message);
    };
    if (auto foregroundThumbnail = Find(foreground))
        refresh(foregroundThumbnail, true);
    for (const auto& [window, thumbnail] : windows_)
        if (window != foreground)
            refresh(thumbnail, false);
    return stateChanged;
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
        state.nextGraphicsCapture_ = 0;
    }
    if (state.minimized_ != minimized || (foreground && !state.foreground_))
    {
        state.refreshNeeded_ = true;
        state.nextAttempt_ = 0;
        if (state.minimized_ && !minimized) state.nextGraphicsCapture_ = 0;
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
            changed_ |= state.minimized_;
            PostMessageW(notifyWindow, message, 0, 0);
        }
        return;
    }
    state.nextAttempt_ = now + (state.pixels_.empty() && minimized ? 1500 : 1000);
    const auto source = state.source_;
    const auto epoch = epoch_;
    const auto weak = std::weak_ptr{thumbnail};
    const bool allowGraphicsCapture = !minimized && state.pixels_.empty() &&
        now >= state.nextGraphicsCapture_ && (foreground || state.nextGraphicsCapture_ == 0);
    state.pending_ = work_.Submit(source.Key() + L":" +
        std::to_wstring(reinterpret_cast<UINT_PTR>(thumbnail.get())),
        [source, weak, allowGraphicsCapture] {
            return weak.expired() ? Pixels{} : CapturePixels(source, allowGraphicsCapture);
        }, [this, source, minimized, weak, epoch](Pixels pixels) {
            const auto target = weak.lock();
            if (!target || epoch != epoch_) return;
            target->pending_ = false;
            if (!AllowsPreview(source))
            {
                changed_ |= target->minimized_ && !target->pixels_.empty();
                target->pixels_.clear();
                target->bitmap_.Reset();
                return;
            }
            if (pixels.data.empty() || (minimized && !target->minimized_)) return;
            if (target->size_.width == pixels.size.width && target->size_.height == pixels.size.height &&
                target->pixels_ == pixels.data) return;
            target->size_ = pixels.size;
            target->pixels_ = std::move(pixels.data);
            target->bitmap_.Reset();
            changed_ |= target->minimized_;
        }, notifyWindow, message);
    if (state.pending_)
    {
        state.refreshNeeded_ = false;
        if (allowGraphicsCapture) state.nextGraphicsCapture_ = now + 5000;
    }
}

void ThumbnailCache::Clear()
{
    work_.Cancel();
    windows_.clear();
    ++epoch_;
    changed_ = false;
}

bool ThumbnailCache::Drain()
{
    work_.Drain();
    return std::exchange(changed_, false);
}
}
