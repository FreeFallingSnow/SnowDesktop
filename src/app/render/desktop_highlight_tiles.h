#pragma once

#include "app/render/desktop_backdrop_compositor.h"
#include "ui/render/highlight_coverage.h"

namespace snowdesktop::desktop_highlight_tiles
{
inline bool Intersects(const RECT& first, const RECT& second)
{
    RECT overlap{};
    return IntersectRect(&overlap, &first, &second) != FALSE;
}

template<typename Tile>
void IncludeRetainedTiles(RECT& dirty, const RECT& client, const std::vector<Tile>& tiles)
{
    bool expanded;
    do
    {
        const RECT before = dirty;
        for (const auto& tile : tiles)
            if (Intersects(dirty, tile.frame)) UnionRect(&dirty, &dirty, &tile.frame);
        expanded = !EqualRect(&before, &dirty);
    } while (expanded);
    IntersectRect(&dirty, &dirty, &client);
}

template<typename Tile>
std::vector<Tile> Stage(DesktopBackdropCompositor& compositor, HWND window,
    ID2D1DeviceContext* context, const HighlightCoverage& coverage, const RECT& client,
    const RECT& dirty, POINT updateOffset, const std::vector<Tile>& retained, std::uintptr_t& nextKey)
{
    auto frames = coverage.Bounds({dirty.left - updateOffset.x, dirty.top - updateOffset.y});
    for (auto& frame : frames) IntersectRect(&frame, &frame, &client);
    std::erase_if(frames, [&](const RECT& frame) { return !Intersects(frame, dirty); });
    for (std::size_t i = 0; i < frames.size();)
    {
        const auto overlap = std::find_if(frames.begin() + i + 1, frames.end(),
            [&](const RECT& frame) { return Intersects(frames[i], frame); });
        if (overlap == frames.end())
        {
            ++i;
            continue;
        }
        UnionRect(&frames[i], &frames[i], &*overlap);
        frames.erase(overlap);
        i = 0;
    }

    std::vector<Tile> candidates;
    D2D1_MATRIX_3X2_F paintTransform{};
    context->GetTransform(&paintTransform);
    for (const auto& frame : frames)
    {
        RECT visible{};
        IntersectRect(&visible, &frame, &dirty);
        const bool complete = EqualRect(&frame, &visible) != FALSE;
        if (!complete)
        {
            InvalidateRect(window, &frame, FALSE);
        }
        const auto old = std::find_if(retained.begin(), retained.end(),
            [&](const auto& tile) { return EqualRect(&tile.frame, &frame) != FALSE; });
        const auto key = old == retained.end() ? nextKey : old->key;
        const POINT atlas{updateOffset.x + frame.left - dirty.left,
            updateOffset.y + frame.top - dirty.top};
        if (complete && compositor.StageSrgbContent(window, key, frame, context, atlas) &&
            compositor.PublishSrgbContent(key))
        {
            if (old == retained.end()) nextKey += 2;
            candidates.push_back({key, frame});
            continue;
        }
        context->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(updateOffset.x - dirty.left),
            static_cast<float>(updateOffset.y - dirty.top)));
        context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(visible.left),
            static_cast<float>(visible.top), static_cast<float>(visible.right),
            static_cast<float>(visible.bottom)), D2D1_ANTIALIAS_MODE_ALIASED);
        (void)coverage.Paint(context, {}, {}, HighlightCoverage::Pass::MaterialFallback);
        context->PopAxisAlignedClip();
        context->SetTransform(paintTransform);
    }
    return candidates;
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
