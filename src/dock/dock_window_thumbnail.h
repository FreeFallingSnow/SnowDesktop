#pragma once

#include "common/background_work.h"

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace snowdesktop::dock_window_thumbnail
{
inline constexpr float kHorizontalCardAspectRatio = 16.0f / 10.0f;

struct WindowIdentity
{
    HWND window = nullptr;
    DWORD process = 0;
    DWORD thread = 0;

    static WindowIdentity Capture(HWND window) noexcept;
    bool IsCurrent() const noexcept;
    std::wstring Key() const;
    bool operator==(const WindowIdentity&) const = default;
};

class Thumbnail final
{
public:
    explicit Thumbnail(WindowIdentity source) : source_(source) {}
    ID2D1Bitmap1* Bitmap(ID2D1DeviceContext& context);
    float LayoutAspectRatio() const noexcept { return kHorizontalCardAspectRatio; }

private:
    friend class ThumbnailCache;
    WindowIdentity source_;
    SIZE liveSize_{};
    D2D1_SIZE_U size_{};
    std::vector<std::uint32_t> pixels_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    std::uint64_t epoch_ = 0;
    ULONGLONG nextAttempt_ = 0;
    bool minimizedInDock_ = false;
    bool minimized_ = false;
    bool foreground_ = false;
    bool pending_ = false;
    bool refreshNeeded_ = true;
};

class ThumbnailCache final
{
public:
    struct TrackedWindow
    {
        WindowIdentity source;
        bool minimized = false;
    };

    void SetTrackedWindows(std::span<const TrackedWindow> windows);
    std::shared_ptr<Thumbnail> Find(HWND window) const;
    bool Refresh(HWND foreground, HWND notifyWindow, UINT message);
    void Clear();
    bool Drain();

private:
    void Refresh(const std::shared_ptr<Thumbnail>& thumbnail, bool minimized,
        bool foreground, HWND notifyWindow, UINT message);

    // PrintWindow is synchronous and an unresponsive app can stall its caller.
    // Keep it off the UI thread and out of the Shell metadata queue.
    BackgroundWork work_{2, 64};
    std::unordered_map<HWND, std::shared_ptr<Thumbnail>> windows_;
    std::uint64_t epoch_ = 1;
    bool changed_ = false;
};
}
