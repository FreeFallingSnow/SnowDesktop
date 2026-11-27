#include "dock_window_thumbnail.h"

#include <algorithm>
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

bool AllowsPreview(WindowIdentity window) noexcept
{
    DWORD affinity = WDA_NONE;
    return window.IsCurrent() &&
        (!GetWindowDisplayAffinity(window.window, &affinity) || affinity == WDA_NONE);
}

Pixels CapturePixels(WindowIdentity source, bool minimized)
{
    if (!AllowsPreview(source)) return {};
    DWORD_PTR reply = 0;
    if (!SendMessageTimeoutW(source.window, WM_NULL, 0, 0,
            SMTO_BLOCK | SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 100, &reply))
        return {};
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
    constexpr int maxThumbnailEdge = 256;
    const int divisor = std::max({width, height, maxThumbnailEdge});
    const auto targetWidth = static_cast<UINT32>(std::max(1, width * maxThumbnailEdge / divisor));
    const auto targetHeight = static_cast<UINT32>(std::max(1, height * maxThumbnailEdge / divisor));
    Pixels result{{targetWidth, targetHeight}, {}};
    result.data.resize(static_cast<size_t>(targetWidth) * targetHeight);
    // Render-full-content handles composed windows. Some Win32 windows
    // still require the ordinary WM_PRINT path to draw their client area.
    constexpr UINT printModes[]{PW_RENDERFULLCONTENT, 0};
    for (const UINT flags : printModes)
    {
        std::fill_n(pixels, static_cast<size_t>(width) * height, 0u);
        if (!PrintWindow(source.window, dib.dc, flags) || !GdiFlush()) continue;
        if (!AllowsPreview(source)) return {};
        bool hasContent = false;
        for (UINT32 y = 0; y < targetHeight; ++y)
        {
            const size_t row = static_cast<size_t>(y) * height / targetHeight * width;
            for (UINT32 x = 0; x < targetWidth; ++x)
            {
                const auto pixel = pixels[row + static_cast<size_t>(x) * width / targetWidth] & 0x00ffffffu;
                result.data[static_cast<size_t>(y) * targetWidth + x] = pixel;
                // Ignore chrome when testing whether the client was drawn.
                hasContent |= y > targetHeight / 4 && y < targetHeight * 3 / 4 &&
                    x > targetWidth / 8 && x < targetWidth * 7 / 8 && pixel != 0;
            }
        }
        if (hasContent) return result;
    }
    return {};
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
    // Keep the last usable frame while iconic. The normal window can
    // provide its next frame on restore without replacing the task entry.
    if (!state.pixels_.empty() && (minimized || (!state.refreshNeeded_ && !foreground)))
    {
        state.nextAttempt_ = now + 3000;
        if (!AllowsPreview(state.source_))
        {
            state.pixels_.clear();
            state.bitmap_.Reset();
            repaint_ = true;
            PostMessageW(notifyWindow, message, 0, 0);
        }
        return;
    }
    state.nextAttempt_ = now + (state.pixels_.empty() ? 3000 : 1000);
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
                repaint_ |= !target->pixels_.empty();
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
            repaint_ = true;
        }, notifyWindow, message);
    if (state.pending_) state.refreshNeeded_ = false;
}

void ThumbnailCache::Clear()
{
    work_.Cancel();
    ++epoch_;
    repaint_ = false;
}

bool ThumbnailCache::Drain()
{
    work_.Drain();
    return std::exchange(repaint_, false);
}
}
