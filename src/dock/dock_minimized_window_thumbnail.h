#pragma once

#include "common/background_work.h"

#include <d2d1_1.h>
#include <wrl/client.h>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace snowdesktop::dock_minimized_window
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
    ID2D1Bitmap1* Bitmap(ID2D1DeviceContext& context);

private:
    friend class ThumbnailCache;
    D2D1_SIZE_U size_{};
    std::vector<std::uint32_t> pixels_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap_;
    bool shown_ = false;
    bool minimizedFrame_ = false;
};

class ThumbnailCache final
{
public:
    void Warm(HWND window, HWND notifyWindow, UINT message);
    std::shared_ptr<Thumbnail> Show(WindowIdentity window, HWND notifyWindow, UINT message);
    void Clear();
    bool Drain();

private:
    void Request(WindowIdentity window, const std::shared_ptr<Thumbnail>& thumbnail,
        bool minimized, HWND notifyWindow, UINT message);

    // PrintWindow is synchronous and an unresponsive app can stall its caller.
    // Keep it off the UI thread and out of the Shell metadata queue.
    BackgroundWork work_{2, 64};
    struct WarmEntry
    {
        WindowIdentity window;
        std::shared_ptr<Thumbnail> thumbnail;
    };
    std::deque<WarmEntry> warm_;
    bool repaint_ = false;
};
}
