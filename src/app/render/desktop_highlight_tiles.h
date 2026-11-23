#pragma once

#include "app/render/desktop_backdrop_compositor.h"
#include "diagnostics/performance_trace.h"

namespace snowdesktop::desktop_highlight_tiles
{
inline bool Intersects(const RECT& first, const RECT& second)
{
    RECT overlap{};
    return IntersectRect(&overlap, &first, &second) != FALSE;
}

inline constexpr LONG kSceneTileSize = 512;

template<typename Tile>
bool Stage(DesktopBackdropCompositor& compositor, HWND window, ID2D1DeviceContext* context,
    ID2D1CommandList* scene, const RECT& client, const RECT& dirty, POINT updateOffset,
    const std::vector<Tile>& retained, std::uintptr_t& nextKey, std::vector<Tile>& candidates)
{
    D2D1_RECT_F bounds{};
    const bool hasBounds = SUCCEEDED(context->GetImageLocalBounds(scene, &bounds));
    if (hasBounds)
    {
        // Image bounds are geometric. Include the adjacent antialias pixels.
        bounds.left -= 2.f;
        bounds.top -= 2.f;
        bounds.right += 2.f;
        bounds.bottom += 2.f;
    }
    const auto tileStart = [](LONG value, LONG start) {
        return start + (value - start) / kSceneTileSize * kSceneTileSize;
    };
    std::vector<std::uintptr_t> newKeys;
    std::vector<std::uintptr_t> staged;
    double paintedPixels = 0;
    for (LONG top = tileStart(dirty.top, client.top); top < dirty.bottom; top += kSceneTileSize)
    {
        for (LONG left = tileStart(dirty.left, client.left); left < dirty.right; left += kSceneTileSize)
        {
            const RECT frame{left, top, std::min(left + kSceneTileSize, client.right),
                std::min(top + kSceneTileSize, client.bottom)};
            RECT patch{};
            if (!IntersectRect(&patch, &frame, &dirty)) continue;
            const POINT origin{updateOffset.x + patch.left - dirty.left,
                updateOffset.y + patch.top - dirty.top};
            const bool hasInk = !hasBounds || !(bounds.right <= static_cast<float>(origin.x) ||
                bounds.bottom <= static_cast<float>(origin.y) ||
                bounds.left >= static_cast<float>(origin.x + patch.right - patch.left) ||
                bounds.top >= static_cast<float>(origin.y + patch.bottom - patch.top));
            const auto old = std::find_if(retained.begin(), retained.end(),
                [&](const auto& tile) { return EqualRect(&tile.frame, &frame) != FALSE; });
            const bool wholeTile = EqualRect(&patch, &frame) != FALSE;
            const bool existing = old != retained.end();
            if (!hasInk && (!existing || wholeTile)) continue;
            const auto key = existing ? old->key : nextKey;
            bool painted = false;
            if (existing && !wholeTile)
            {
                // Clear the patch even when the new scene has no ink there.
                // Pixels outside the patch still belong to the retained tile.
                RECT localDamage = patch;
                OffsetRect(&localDamage, -frame.left, -frame.top);
                painted = SUCCEEDED(compositor.UpdateSrgbScene(key, localDamage, scene, origin));
            }
            else
            {
                if (!existing)
                {
                    nextKey += 2;
                    newKeys.push_back(key);
                }
                // An absent tile was transparent. Initialize the allocation
                // and replay only the part recorded in this frame.
                painted = compositor.StageSrgbScene(window, key, frame, context, origin, scene, &patch);
                staged.push_back(key);
            }
            if (!painted)
            {
                for (const auto created : newKeys) compositor.RemoveSrgbContent(created);
                return false;
            }
            const RECT& raster = existing ? patch : frame;
            paintedPixels += static_cast<double>(raster.right - raster.left) * (raster.bottom - raster.top);
            candidates.push_back({key, frame});
        }
    }
    for (const auto key : staged)
    {
        if (compositor.PublishSrgbContent(key)) continue;
        for (const auto created : newKeys) compositor.RemoveSrgbContent(created);
        return false;
    }
    performance::Value("desktop", "tile_raster_pixels", {}, paintedPixels);
    return true;
}

template<typename Tile>
void Publish(DesktopBackdropCompositor& compositor, const RECT& dirty, bool fullScene,
    const std::vector<Tile>& candidates, std::vector<Tile>& retained)
{
    std::erase_if(retained, [&](const auto& old) {
        if (!fullScene && !Intersects(old.frame, dirty)) return false;
        if (std::none_of(candidates.begin(), candidates.end(),
            [&](const auto& next) { return next.key == old.key; }))
            compositor.RemoveSrgbContent(old.key);
        return true;
    });
    retained.insert(retained.end(), candidates.begin(), candidates.end());
}
}
