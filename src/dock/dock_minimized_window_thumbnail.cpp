#include "dock_minimized_window_thumbnail.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace snowdesktop::dock_minimized_window
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

bool AllowsPreview(WindowIdentity window) noexcept
{
    DWORD affinity = WDA_NONE;
    return window.IsCurrent() &&
        (!GetWindowDisplayAffinity(window.window, &affinity) || affinity == WDA_NONE);
}

Pixels CapturePixels(WindowIdentity source, bool minimized)
{
    if (!AllowsPreview(source)) return {};
    RECT bounds{};
    if (IsIconic(source.window))
    {
        if (!minimized) return {};
        WINDOWPLACEMENT placement{sizeof(placement)};
        if (!GetWindowPlacement(source.window, &placement)) return {};
        bounds = placement.rcNormalPosition;
    }
    else if (!GetWindowRect(source.window, &bounds))
        return {};

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    constexpr int maxSourcePixels = 16 * 1024 * 1024;
    if (width < 32 || height < 32 || width > maxSourcePixels / height) return {};

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
    std::fill_n(pixels, static_cast<size_t>(width) * height, 0u);
    if (!PrintWindow(source.window, dib.dc, PW_RENDERFULLCONTENT) ||
        !GdiFlush() || !AllowsPreview(source))
        return {};

    constexpr int maxThumbnailEdge = 256;
    const int divisor = std::max({width, height, maxThumbnailEdge});
    const auto targetWidth = static_cast<UINT32>(std::max(1, width * maxThumbnailEdge / divisor));
    const auto targetHeight = static_cast<UINT32>(std::max(1, height * maxThumbnailEdge / divisor));
    Pixels result{{targetWidth, targetHeight}, {}};
    result.data.resize(static_cast<size_t>(targetWidth) * targetHeight);
    bool hasContent = false;
    for (UINT32 y = 0; y < targetHeight; ++y)
    {
        const size_t row = static_cast<size_t>(y) * height / targetHeight * width;
        for (UINT32 x = 0; x < targetWidth; ++x)
        {
            const auto pixel = pixels[row + static_cast<size_t>(x) * width / targetWidth] & 0x00ffffffu;
            result.data[static_cast<size_t>(y) * targetWidth + x] = pixel;
            // Some iconic HWNDs return success after painting only a title bar.
            hasContent |= y >= targetHeight / 4 && pixel != 0;
        }
    }
    return hasContent ? std::move(result) : Pixels{};
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
    return L"minimized-window:" + std::to_wstring(reinterpret_cast<UINT_PTR>(window)) +
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

void ThumbnailCache::Warm(HWND window, HWND notifyWindow, UINT message)
{
    const auto identity = WindowIdentity::Capture(window);
    if (!identity.IsCurrent() || IsIconic(window)) return;
    const auto existing = std::ranges::find(warm_, identity, &WarmEntry::window);
    if (existing != warm_.end())
    {
        std::rotate(warm_.begin(), existing, std::next(existing));
        return;
    }
    // Keep the preceding foreground window until discovery observes its
    // minimization, even when the next app has already taken focus.
    constexpr size_t maxRecentWindows = 4;
    if (warm_.size() >= maxRecentWindows) warm_.pop_back();
    warm_.push_front({identity, std::make_shared<Thumbnail>()});
    Request(identity, warm_.front().thumbnail, false, notifyWindow, message);
}

std::shared_ptr<Thumbnail> ThumbnailCache::Show(WindowIdentity window, HWND notifyWindow, UINT message)
{
    std::shared_ptr<Thumbnail> thumbnail;
    const auto existing = std::ranges::find(warm_, window, &WarmEntry::window);
    if (existing != warm_.end())
    {
        thumbnail = std::move(existing->thumbnail);
        warm_.erase(existing);
    }
    if (!thumbnail || !AllowsPreview(window)) thumbnail = std::make_shared<Thumbnail>();
    thumbnail->shown_ = true;
    Request(window, thumbnail, true, notifyWindow, message);
    return thumbnail;
}

void ThumbnailCache::Request(WindowIdentity window, const std::shared_ptr<Thumbnail>& thumbnail,
    bool minimized, HWND notifyWindow, UINT message)
{
    const auto weak = std::weak_ptr{thumbnail};
    work_.Submit(window.Key() + (minimized ? L":iconic:" : L":visible:") +
        std::to_wstring(reinterpret_cast<UINT_PTR>(thumbnail.get())),
        [window, minimized, weak] {
            return weak.expired() ? Pixels{} : CapturePixels(window, minimized);
        }, [this, window, minimized, weak](Pixels pixels) {
            const auto target = weak.lock();
            if (!target || pixels.data.empty() || !AllowsPreview(window) ||
                (!minimized && target->minimizedFrame_))
                return;
            target->size_ = pixels.size;
            target->pixels_ = std::move(pixels.data);
            target->bitmap_.Reset();
            target->minimizedFrame_ = minimized;
            repaint_ |= target->shown_;
        }, notifyWindow, message);
}

void ThumbnailCache::Clear()
{
    work_.Cancel();
    warm_.clear();
    repaint_ = false;
}

bool ThumbnailCache::Drain()
{
    work_.Drain();
    return std::exchange(repaint_, false);
}
}
