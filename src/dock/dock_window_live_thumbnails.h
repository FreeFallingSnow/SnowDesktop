#pragma once

#include "dock_window_shared_thumbnail.h"
#include "dock_window_thumbnail.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace snowdesktop::dock_window_thumbnail
{
// Keeps one DWM visual per visible card in the Dock's composition tree.
// The normal Dock surface draws its decorations above these visuals.
class LiveThumbnailLayer final
{
public:
    void BeginFrame() noexcept;
    std::optional<D2D1_RECT_F> Place(HWND destination, IDCompositionDesktopDevice& device,
        WindowIdentity source, D2D1_RECT_F card, POINT origin);
    HRESULT Publish(IDCompositionVisual& root, IDCompositionVisual& foreground) noexcept;
    void RetireAfterCommit(IDCompositionDesktopDevice& device) noexcept;
    void Reset(IDCompositionVisual* root) noexcept;

private:
    struct Placement
    {
        float scale;
        float x;
        float y;
        bool operator==(const Placement&) const = default;
    };
    struct Entry
    {
        WindowIdentity source;
        SIZE sourceSize{};
        dock_thumbnail::SharedVisual image;
        std::optional<Placement> pending;
        std::optional<Placement> published;
        ULONGLONG retryAfter = 0;
        bool requested = false;
        bool attached = false;
    };

    std::unordered_map<HWND, Entry> entries_;
    std::vector<dock_thumbnail::SharedVisual> retired_;
};
}
