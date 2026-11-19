#pragma once

#include "app/render/desktop_backdrop_compositor.h"

namespace snowdesktop::desktop_highlight_tiles
{
inline bool Intersects(const RECT& first, const RECT& second)
{
    RECT overlap{};
    return IntersectRect(&overlap, &first, &second) != FALSE;
}

inline constexpr LONG kSceneTileSize = 512;

inline void IncludeSceneTiles(RECT& dirty, const RECT& client)
{
    const auto tileStart = [](LONG value) { return value / kSceneTileSize * kSceneTileSize; };
    dirty.left = tileStart(dirty.left);
    dirty.top = tileStart(dirty.top);
    dirty.right = tileStart(dirty.right - 1) + kSceneTileSize;
    dirty.bottom = tileStart(dirty.bottom - 1) + kSceneTileSize;
    IntersectRect(&dirty, &dirty, &client);
}

template<typename Tile>
bool Stage(DesktopBackdropCompositor& compositor, HWND window, ID2D1DeviceContext* context,
    ID2D1CommandList* scene, const RECT& dirty, POINT updateOffset, const std::vector<Tile>& retained,
    std::uintptr_t& nextKey, std::vector<Tile>& candidates)
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
    std::vector<std::uintptr_t> newKeys;
    for (LONG top = dirty.top; top < dirty.bottom; top += kSceneTileSize)
    {
        for (LONG left = dirty.left; left < dirty.right; left += kSceneTileSize)
        {
            const RECT frame{left, top, std::min(left + kSceneTileSize, dirty.right),
                std::min(top + kSceneTileSize, dirty.bottom)};
            const POINT origin{updateOffset.x + frame.left - dirty.left,
                updateOffset.y + frame.top - dirty.top};
            if (hasBounds && (bounds.right <= static_cast<float>(origin.x) ||
                bounds.bottom <= static_cast<float>(origin.y) ||
                bounds.left >= static_cast<float>(origin.x + frame.right - frame.left) ||
                bounds.top >= static_cast<float>(origin.y + frame.bottom - frame.top))) continue;

            const auto old = std::find_if(retained.begin(), retained.end(),
                [&](const auto& tile) { return EqualRect(&tile.frame, &frame) != FALSE; });
            const auto key = old == retained.end() ? nextKey : old->key;
            if (old == retained.end())
            {
                nextKey += 2;
                newKeys.push_back(key);
            }
            if (!compositor.StageSrgbScene(window, key, frame, context, origin, scene))
            {
                for (const auto created : newKeys) compositor.RemoveSrgbContent(created);
                return false;
            }
            candidates.push_back({key, frame});
        }
    }
    for (const auto& candidate : candidates)
        if (!compositor.PublishSrgbContent(candidate.key)) return false;
    return true;
}

template<typename Tile>
void Publish(DesktopBackdropCompositor& compositor, const RECT& dirty,
    const std::vector<Tile>& candidates, std::vector<Tile>& retained)
{
    std::erase_if(retained, [&](const auto& old) {
        if (!Intersects(old.frame, dirty)) return false;
        if (std::none_of(candidates.begin(), candidates.end(),
            [&](const auto& next) { return next.key == old.key; }))
            compositor.RetireSrgbAfterDirectCommit(old.key);
        return true;
    });
    retained.insert(retained.end(), candidates.begin(), candidates.end());
}
}
