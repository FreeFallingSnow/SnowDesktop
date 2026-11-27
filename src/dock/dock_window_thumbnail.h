#pragma once

#include "common/background_work.h"

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::dock_window_thumbnail
{
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

private:
    friend class ThumbnailCache;
    WindowIdentity source_;
    D2D1_SIZE_U size_{};
    std::vector<std::uint32_t> pixels_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    std::uint64_t epoch_ = 0;
    ULONGLONG nextAttempt_ = 0;
    bool minimized_ = false;
    bool foreground_ = false;
    bool pending_ = false;
    bool refreshNeeded_ = true;
};

class ThumbnailCache final
{
public:
    void Refresh(const std::shared_ptr<Thumbnail>& thumbnail, bool minimized,
        bool foreground, HWND notifyWindow, UINT message);
    void Clear();
    bool Drain();

private:
    // PrintWindow is synchronous and an unresponsive app can stall its caller.
    // Keep it off the UI thread and out of the Shell metadata queue.
    BackgroundWork work_{2, 64};
    std::uint64_t epoch_ = 1;
    bool repaint_ = false;
};
}
