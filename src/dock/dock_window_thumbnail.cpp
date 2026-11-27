#include "dock_window_thumbnail.h"
#include "dock_window_live_thumbnails.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_4.h>
#include <new>
#include <wincrypt.h>
#include <utility>

namespace snowdesktop::dock_window_thumbnail
{
namespace
{
struct Pixels
{
    D2D1_SIZE_U size{};
    std::vector<std::uint32_t> data;
    bool fromSession = false;
    bool liveDwmFrame = false;
};

bool HasUnpaintedTopBand(HWND window, const Pixels& frame)
{
    if (frame.data.empty() || (GetWindowLongPtrW(window, GWL_STYLE) & WS_CAPTION) != WS_CAPTION)
        return false;

    // A redirected frame can contain the client image before the window
    // chrome is available. Ignore the thin frame borders, but require an
    // uninterrupted band of black pixels so a dark title bar with detail passes.
    const UINT32 margin = std::max(1u, frame.size.width / 32);
    const UINT32 firstRow = frame.size.height / 64;
    const UINT32 rows = std::max(2u, (frame.size.height + 9) / 10);
    if (frame.size.width <= 2 * margin || frame.size.height < firstRow + rows) return false;
    for (UINT32 y = firstRow; y < firstRow + rows; ++y)
    {
        const auto* row = frame.data.data() + static_cast<size_t>(y) * frame.size.width;
        if (std::any_of(row + margin, row + frame.size.width - margin,
                [](std::uint32_t pixel) { return pixel != 0; }))
            return false;
    }
    return true;
}

constexpr UINT32 kMaxThumbnailEdge = 256;
constexpr wchar_t kSessionCacheKey[] = L"Software\\SnowDesktop.DockThumbnailSession.v1";
constexpr DWORD kMaxSessionFrames = 64;
SRWLOCK sessionWriteLock = SRWLOCK_INIT;

struct SessionFrameHeader
{
    std::uint64_t processCreation;
    D2D1_SIZE_U size;
};

struct SessionRegistryKey
{
    HKEY key = nullptr;
    ~SessionRegistryKey() { if (key) RegCloseKey(key); }
};

struct ProtectedFrame
{
    DATA_BLOB blob{};
    ~ProtectedFrame() { LocalFree(blob.pbData); }
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

struct ComposedThumbnail
{
    HWND window = nullptr;
    HTHUMBNAIL thumbnail = nullptr;

    ~ComposedThumbnail()
    {
        if (thumbnail) DwmUnregisterThumbnail(thumbnail);
        if (window) DestroyWindow(window);
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

std::uint64_t SessionProcessCreation(WindowIdentity source)
{
    if (!source.IsCurrent()) return 0;
    DWORD sourceSession = 0, currentSession = 0;
    if (!ProcessIdToSessionId(source.process, &sourceSession) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &currentSession) || sourceSession != currentSession)
        return 0;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, source.process);
    if (!process) return 0;
    FILETIME creation{}, exit{}, kernel{}, user{};
    const BOOL read = GetProcessTimes(process, &creation, &exit, &kernel, &user);
    CloseHandle(process);
    return read && source.IsCurrent()
        ? (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32u) | creation.dwLowDateTime : 0;
}

std::wstring SessionFrameName(WindowIdentity source)
{
    return std::to_wstring(reinterpret_cast<UINT_PTR>(source.window)) + L"." +
        std::to_wstring(source.process) + L"." + std::to_wstring(source.thread);
}

void ForgetSessionFrame(WindowIdentity source)
{
    SessionRegistryKey registry;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSessionCacheKey, 0, KEY_SET_VALUE, &registry.key) == ERROR_SUCCESS)
        RegDeleteValueW(registry.key, SessionFrameName(source).c_str());
}

Pixels ReadSessionFrame(WindowIdentity source)
{
    if (!AllowsRedirectedSurface(source)) return {};
    const auto processCreation = SessionProcessCreation(source);
    if (!processCreation) return {};
    SessionRegistryKey registry;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSessionCacheKey, 0, KEY_QUERY_VALUE, &registry.key) != ERROR_SUCCESS)
        return {};
    const auto name = SessionFrameName(source);
    DWORD bytes = 0;
    constexpr auto limit = static_cast<DWORD>(sizeof(SessionFrameHeader) +
        kMaxThumbnailEdge * kMaxThumbnailEdge * sizeof(std::uint32_t) + 4096);
    if (RegGetValueW(registry.key, nullptr, name.c_str(), RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        !bytes || bytes > limit)
        return {};
    std::vector<BYTE> encrypted(bytes);
    if (RegGetValueW(registry.key, nullptr, name.c_str(), RRF_RT_REG_BINARY, nullptr, encrypted.data(), &bytes) != ERROR_SUCCESS)
        return {};
    DATA_BLOB input{bytes, encrypted.data()};
    ProtectedFrame plain;
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain.blob) ||
        plain.blob.cbData < sizeof(SessionFrameHeader))
        return {};
    SessionFrameHeader header{};
    std::memcpy(&header, plain.blob.pbData, sizeof(header));
    if (header.processCreation != processCreation || !header.size.width || !header.size.height ||
        header.size.width > kMaxThumbnailEdge || header.size.height > kMaxThumbnailEdge)
        return {};
    const size_t count = static_cast<size_t>(header.size.width) * header.size.height;
    if (plain.blob.cbData != sizeof(header) + count * sizeof(std::uint32_t) || !AllowsRedirectedSurface(source))
        return {};
    Pixels frame{header.size, std::vector<std::uint32_t>(count), true};
    std::memcpy(frame.data.data(), plain.blob.pbData + sizeof(header), count * sizeof(std::uint32_t));
    if (HasUnpaintedTopBand(source.window, frame)) return {};
    return frame;
}

bool WriteSessionFrame(WindowIdentity source, const Pixels& frame)
{
    if (!AllowsRedirectedSurface(source)) return false;
    const auto processCreation = SessionProcessCreation(source);
    if (!processCreation) return false;
    const SessionFrameHeader header{processCreation, frame.size};
    const size_t pixelBytes = frame.data.size() * sizeof(std::uint32_t);
    std::vector<BYTE> plain(sizeof(header) + pixelBytes);
    std::memcpy(plain.data(), &header, sizeof(header));
    std::memcpy(plain.data() + sizeof(header), frame.data.data(), pixelBytes);
    DATA_BLOB input{static_cast<DWORD>(plain.size()), plain.data()};
    ProtectedFrame encrypted;
    if (!CryptProtectData(&input, L"SnowDesktop window thumbnail", nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &encrypted.blob))
        return false;
    struct WriteLock
    {
        WriteLock() { AcquireSRWLockExclusive(&sessionWriteLock); }
        ~WriteLock() { ReleaseSRWLockExclusive(&sessionWriteLock); }
    } writeLock;
    SessionRegistryKey registry;
    // Volatile keys survive a Dock restart but are not saved with the user
    // hive. The last image is tied to the same window and process lifetime.
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSessionCacheKey, 0, nullptr, REG_OPTION_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &registry.key, nullptr) != ERROR_SUCCESS)
        return false;
    const auto name = SessionFrameName(source);
    DWORD count = 0;
    if (RegQueryInfoKeyW(registry.key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
            &count, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS ||
        (count >= kMaxSessionFrames && RegQueryValueExW(registry.key, name.c_str(), nullptr,
            nullptr, nullptr, nullptr) != ERROR_SUCCESS) ||
        !AllowsRedirectedSurface(source))
        return false;
    return RegSetValueExW(registry.key, name.c_str(), 0, REG_BINARY,
        encrypted.blob.pbData, encrypted.blob.cbData) == ERROR_SUCCESS;
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

Pixels SamplePixels(const void* buffer, size_t rowPitch, int width, int height, bool composedThumbnail = false)
{
    constexpr int maxThumbnailEdge = kMaxThumbnailEdge;
    const int divisor = std::max({width, height, maxThumbnailEdge});
    const auto targetWidth = static_cast<UINT32>(std::max(1, width * maxThumbnailEdge / divisor));
    const auto targetHeight = static_cast<UINT32>(std::max(1, height * maxThumbnailEdge / divisor));
    Pixels result{{targetWidth, targetHeight}, {}};
    result.data.resize(static_cast<size_t>(targetWidth) * targetHeight);
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    const UINT32 innerLeft = targetWidth / 8;
    const UINT32 innerRight = targetWidth - innerLeft;
    UINT32 detailRows = 0;
    UINT32 clientDetailRows = 0;
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
        {
            ++detailRows;
            if (y >= targetHeight / 4) ++clientDetailRows;
        }
    }
    // Initial minimized frames can contain solid horizontal bands and no
    // client content. A nonblack pixel on its own does not validate a frame.
    if (composedThumbnail)
    {
        // DWM can report the original window size while supplying only its
        // toolbar. Check the scaled client area as well as the full frame.
        if (detailRows < std::max(2u, targetHeight / 24) ||
            clientDetailRows < std::max(2u, targetHeight / 48))
            return {};
    }
    else if (detailRows < std::max(1u, targetHeight / 12))
        return {};
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

Pixels ReadComposedThumbnailPixels(HWND destination, HWND source, int width, int height)
{
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
    // Include the DWM thumbnail composed into our off-screen destination.
    constexpr UINT printModes[]{PW_RENDERFULLCONTENT, PW_RENDERFULLCONTENT | PW_CLIENTONLY};
    for (const UINT flags : printModes)
    {
        std::fill_n(pixels, static_cast<size_t>(width) * height, 0u);
        if (!PrintWindow(destination, dib.dc, flags) || !GdiFlush()) continue;
        auto result = SamplePixels(buffer, static_cast<size_t>(width) * sizeof(*pixels), width, height, true);
        if (!result.data.empty() && !HasUnpaintedTopBand(source, result)) return result;
    }
    return {};
}

Pixels CaptureComposedThumbnail(WindowIdentity source, bool iconic)
{
    if (!AllowsRedirectedSurface(source) || !IsWindowVisible(source.window) ||
        (IsIconic(source.window) != FALSE) != iconic)
        return {};
    ComposedThumbnail image;
    // The thumbnail interface renders to a top-level HWND. Use a small
    // off-screen destination so its result can join the Dock's own scene.
    image.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", nullptr,
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!image.window || FAILED(DwmRegisterThumbnail(image.window, source.window, &image.thumbnail)))
        return {};
    SIZE size{};
    if (FAILED(DwmQueryThumbnailSourceSize(image.thumbnail, &size)) || size.cx < 32 || size.cy < 32)
        return {};
    if (iconic)
    {
        const SIZE expected = ReadWindowSize(source.window, true);
        if (size.cx < expected.cx / 2 || size.cy < expected.cy / 2) return {};
    }
    constexpr LONG edge = 512;
    const LONG longest = std::max(size.cx, size.cy);
    const int width = std::max(32, MulDiv(size.cx, edge, longest));
    const int height = std::max(32, MulDiv(size.cy, edge, longest));
    if (!SetWindowPos(image.window, nullptr,
            GetSystemMetrics(SM_XVIRTUALSCREEN) - width - 16,
            GetSystemMetrics(SM_YVIRTUALSCREEN) - height - 16, width, height,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW) ||
        FAILED(snowdesktop::dock_thumbnail::UpdateWholeWindowThumbnail(
            image.thumbnail, {0, 0, width, height}, size)))
        return {};
    UpdateWindow(image.window);
    if (FAILED(DwmFlush()) || !AllowsRedirectedSurface(source) ||
        (IsIconic(source.window) != FALSE) != iconic)
        return {};
    auto pixels = ReadComposedThumbnailPixels(image.window, source.window, width, height);
    if (!AllowsRedirectedSurface(source)) return {};
    return pixels;
}

Pixels CapturePixels(WindowIdentity source, bool allowExtraCapture)
{
    if (!AllowsPreview(source)) return {};
    const bool iconic = IsIconic(source.window) != FALSE;
    // Prefer the same whole-window DWM thumbnail used by hover previews when
    // filling an empty cache. A redirected surface is a cheaper refresh path.
    if (allowExtraCapture)
    {
        auto result = CaptureComposedThumbnail(source, iconic);
        if (!result.data.empty())
        {
            result.liveDwmFrame = iconic;
            return result;
        }
    }
    auto result = CaptureRedirectedSurface(source, iconic);
    if (HasUnpaintedTopBand(source.window, result)) return {};
    return result;
}

struct RetiredThumbnailVisuals
{
    Microsoft::WRL::ComPtr<IDCompositionDesktopDevice> device;
    std::vector<dock_thumbnail::SharedVisual> images;
};

void CALLBACK ReleaseRetiredThumbnailVisuals(PTP_CALLBACK_INSTANCE, void* context)
{
    const std::unique_ptr<RetiredThumbnailVisuals> retired(
        static_cast<RetiredThumbnailVisuals*>(context));
    if (SUCCEEDED(retired->device->WaitForCommitCompletion())) (void)DwmFlush();
}
}

void LiveThumbnailLayer::BeginFrame() noexcept
{
    for (auto& entry : entries_)
    {
        entry.second.pending.reset();
        entry.second.requested = false;
    }
}

std::optional<D2D1_RECT_F> LiveThumbnailLayer::Place(HWND destination,
    IDCompositionDesktopDevice& device, WindowIdentity source, D2D1_RECT_F card, POINT origin)
{
    if (!source.IsCurrent() || !IsIconic(source.window) ||
        card.right <= card.left || card.bottom <= card.top)
        return {};
    const auto [found, added] = entries_.try_emplace(source.window);
    auto& entry = found->second;
    if (added) entry.source = source;
    if (entry.source != source) return {};
    entry.requested = true;
    if (!entry.image.visual)
    {
        const auto now = GetTickCount64();
        if (now < entry.retryAfter) return {};
        entry.retryAfter = now + 5000;
        SIZE size{};
        if (!AllowsRedirectedSurface(source) ||
            FAILED(dock_thumbnail::SourceSize(source.window, size)))
            return {};
        const SIZE expected = ReadWindowSize(source.window, true);
        if (size.cx < expected.cx / 2 || size.cy < expected.cy / 2)
            return {};
        if (FAILED(entry.image.Create(destination, source.window, &device, size)))
            return {};
        entry.sourceSize = size;
    }

    const float width = static_cast<float>(entry.sourceSize.cx);
    const float height = static_cast<float>(entry.sourceSize.cy);
    const float scale = std::min((card.right - card.left) / width, (card.bottom - card.top) / height);
    const float x = (card.left + card.right - width * scale) * 0.5f;
    const float y = (card.top + card.bottom - height * scale) * 0.5f;
    entry.pending = Placement{scale, x - static_cast<float>(origin.x), y - static_cast<float>(origin.y)};
    return D2D1::RectF(x, y, x + width * scale, y + height * scale);
}

HRESULT LiveThumbnailLayer::Publish(IDCompositionVisual& root, IDCompositionVisual& foreground) noexcept
{
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        auto& entry = it->second;
        if (!entry.pending)
        {
            if (entry.requested)
            {
                ++it;
                continue;
            }
            if (entry.attached)
            {
                try { retired_.emplace_back(); }
                catch (...) { return E_OUTOFMEMORY; }
                const HRESULT hr = root.RemoveVisual(entry.image.visual.Get());
                if (FAILED(hr))
                {
                    retired_.pop_back();
                    return hr;
                }
                retired_.back() = std::move(entry.image);
            }
            it = entries_.erase(it);
            continue;
        }

        if (entry.pending != entry.published)
        {
            const auto& pose = *entry.pending;
            const D2D1_MATRIX_3X2_F transform{pose.scale, 0, 0, pose.scale, pose.x, pose.y};
            const HRESULT hr = static_cast<IDCompositionVisual*>(entry.image.visual.Get())->SetTransform(transform);
            if (FAILED(hr)) return hr;
            entry.published = entry.pending;
        }
        if (!entry.attached)
        {
            const HRESULT hr = root.AddVisual(entry.image.visual.Get(), FALSE, &foreground);
            if (FAILED(hr)) return hr;
            entry.attached = true;
        }
        ++it;
    }
    return S_OK;
}

void LiveThumbnailLayer::RetireAfterCommit(IDCompositionDesktopDevice& device) noexcept
{
    if (retired_.empty()) return;
    auto retired = std::unique_ptr<RetiredThumbnailVisuals>(new (std::nothrow) RetiredThumbnailVisuals);
    if (!retired) return;
    retired->device = &device;
    retired->images.swap(retired_);
    // DWM may still be showing the old frame when the detach is queued.
    // Release registrations only after the replacement committed and presented.
    auto* pending = retired.release();
    if (!TrySubmitThreadpoolCallback(ReleaseRetiredThumbnailVisuals, pending, nullptr))
    {
        pending->images.swap(retired_);
        delete pending;
    }
}

void LiveThumbnailLayer::Reset(IDCompositionVisual* root) noexcept
{
    if (root)
        for (const auto& entry : entries_)
            if (entry.second.attached) (void)root->RemoveVisual(entry.second.image.visual.Get());
    entries_.clear();
    retired_.clear();
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
    for (const auto& [window, thumbnail] : tracked)
        if (!windows_.contains(window) && thumbnail->sessionRevision_ && !thumbnail->source_.IsCurrent())
            ForgetSessionFrame(thumbnail->source_);
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
    // On startup give already-present cards a chance to load before
    // preparing snapshots for windows that are still on screen.
    for (const bool minimized : {true, false})
        for (const auto& [window, thumbnail] : windows_)
            if (window != foreground && thumbnail->minimizedInDock_ == minimized)
                refresh(thumbnail, false);
    return stateChanged;
}

void ThumbnailCache::PrepareMinimize(HWND window, HWND notifyWindow, UINT message)
{
    const auto thumbnail = Find(window);
    if (!thumbnail || !thumbnail->source_.IsCurrent() || IsIconic(window)) return;
    thumbnail->prepareMinimizeUntil_ = GetTickCount64() + 1500;
    if (!thumbnail->pixels_.empty() || thumbnail->pending_) return;
    thumbnail->nextAttempt_ = 0;
    thumbnail->nextExtraCapture_ = 0;
    Refresh(thumbnail, false, true, notifyWindow, message);
}

void ThumbnailCache::SaveForSession(const std::shared_ptr<Thumbnail>& thumbnail, HWND notifyWindow, UINT message)
{
    auto& state = *thumbnail;
    if (state.pixels_.empty() || state.sessionWritePending_ ||
        state.sessionRevision_ == state.frameRevision_ || (state.sessionRevision_ && !state.minimized_))
        return;
    const auto now = GetTickCount64();
    if (now < state.nextSessionWrite_) return;
    state.nextSessionWrite_ = now + 5000;
    const auto source = state.source_;
    const auto revision = state.frameRevision_;
    const auto epoch = epoch_;
    const auto weak = std::weak_ptr{thumbnail};
    const std::wstring key = L"session-write:" + source.Key() + L":" +
        std::to_wstring(reinterpret_cast<UINT_PTR>(thumbnail.get()));
    state.sessionWritePending_ = work_.Submit(key,
        [source, weak, frame = Pixels{state.size_, state.pixels_}] {
            return !weak.expired() && WriteSessionFrame(source, frame);
        }, [this, weak, epoch, revision](bool success) {
            const auto target = weak.lock();
            if (!target || epoch != epoch_) return;
            target->sessionWritePending_ = false;
            if (success)
            {
                target->sessionRevision_ = revision;
                target->nextSessionWrite_ = 0;
            }
        }, notifyWindow, message);
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
        state.nextExtraCapture_ = 0;
        state.liveDwmProbeUntil_ = 0;
        state.liveDwmFrame_ = false;
        state.nextSessionWrite_ = 0;
        state.sessionRead_ = false;
        state.sessionWritePending_ = false;
    }
    if (state.minimized_ != minimized || (foreground && !state.foreground_))
    {
        state.refreshNeeded_ = true;
        state.nextAttempt_ = 0;
        if (state.minimized_ != minimized)
        {
            state.nextExtraCapture_ = 0;
            state.liveDwmFrame_ = false;
            state.liveDwmProbeUntil_ = minimized ? GetTickCount64() + 5000 : 0;
        }
    }
    state.minimized_ = minimized;
    state.foreground_ = foreground;
    SaveForSession(thumbnail, notifyWindow, message);
    if (state.pending_) return;
    const auto now = GetTickCount64();
    if (now < state.nextAttempt_) return;
    const bool checkLiveDwmFrame = minimized && !state.liveDwmFrame_ &&
        now < state.liveDwmProbeUntil_ && now >= state.nextExtraCapture_;
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
    if (!state.pixels_.empty() && !checkLiveDwmFrame &&
        (minimized || (!state.refreshNeeded_ && !foreground)))
    {
        state.nextAttempt_ = now + 3000;
        if (!AllowsPreview(state.source_) || (state.liveDwmFrame_ && !AllowsRedirectedSurface(state.source_)))
        {
            if (state.sessionRevision_) ForgetSessionFrame(state.source_);
            state.sessionRevision_ = 0;
            state.pixels_.clear();
            state.bitmap_.Reset();
            state.liveDwmFrame_ = false;
            changed_ |= state.minimized_ || now < state.prepareMinimizeUntil_;
            PostMessageW(notifyWindow, message, 0, 0);
        }
        return;
    }
    state.nextAttempt_ = now + (state.pixels_.empty() && minimized ? 1500 : 1000);
    const auto source = state.source_;
    const auto epoch = epoch_;
    const auto weak = std::weak_ptr{thumbnail};
    const bool allowExtraCapture = checkLiveDwmFrame ||
        (state.pixels_.empty() && now >= state.nextExtraCapture_ &&
            (minimized || foreground || state.nextExtraCapture_ == 0));
    const bool readSession = minimized && state.pixels_.empty() && !state.sessionRead_;
    state.pending_ = work_.Submit(source.Key() + L":" +
        std::to_wstring(reinterpret_cast<UINT_PTR>(thumbnail.get())),
        [source, weak, allowExtraCapture, readSession] {
            if (weak.expired()) return Pixels{};
            if (readSession)
            {
                auto cached = ReadSessionFrame(source);
                if (!cached.data.empty()) return cached;
            }
            return CapturePixels(source, allowExtraCapture);
        }, [this, source, minimized, weak, epoch, notifyWindow, message](Pixels pixels) {
            const auto target = weak.lock();
            if (!target || epoch != epoch_) return;
            target->pending_ = false;
            if (!AllowsPreview(source) ||
                ((pixels.fromSession || target->liveDwmFrame_) && !AllowsRedirectedSurface(source)))
            {
                if (target->sessionRevision_) ForgetSessionFrame(source);
                target->sessionRevision_ = 0;
                changed_ |= (target->minimized_ || GetTickCount64() < target->prepareMinimizeUntil_) &&
                    !target->pixels_.empty();
                target->pixels_.clear();
                target->bitmap_.Reset();
                target->liveDwmFrame_ = false;
                return;
            }
            if (pixels.data.empty() || (minimized && !target->minimized_)) return;
            if (pixels.liveDwmFrame && target->minimized_ && !target->liveDwmFrame_)
            {
                target->liveDwmFrame_ = true;
                changed_ = true;
            }
            if (target->size_.width == pixels.size.width && target->size_.height == pixels.size.height &&
                target->pixels_ == pixels.data) return;
            target->size_ = pixels.size;
            target->pixels_ = std::move(pixels.data);
            target->bitmap_.Reset();
            ++target->frameRevision_;
            if (pixels.fromSession) target->sessionRevision_ = target->frameRevision_;
            changed_ |= target->minimized_ || GetTickCount64() < target->prepareMinimizeUntil_;
            SaveForSession(target, notifyWindow, message);
        }, notifyWindow, message);
    if (state.pending_)
    {
        state.refreshNeeded_ = false;
        if (readSession) state.sessionRead_ = true;
        if (allowExtraCapture) state.nextExtraCapture_ = now + (checkLiveDwmFrame ? 1500 : 5000);
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
