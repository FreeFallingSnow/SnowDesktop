#pragma once

#include "app/drag_drop/desktop_preview_cache.h"

namespace preview_cache_test
{
// Only storage is supplied; the production cache performs matching, rebuild
// selection and publication. Builders execute real request/planning paths.
struct Storage
{
    DropPreviewList preview;
    POINT point{-1, -1};
    int mods = -1;
    Container* target = nullptr;
    Slot* slot = nullptr;
    HitRegion region = HitRegion::None;
    bool hasItems = false;
    size_t sourceCount = static_cast<size_t>(-1);
    std::uint64_t sceneRevision = 0;
    size_t externalCount = 0;
    snowdesktop::desktop_preview_cache::State Ref()
    {
        return {preview, point, mods, target, slot, region, hasItems, sourceCount, sceneRevision, externalCount};
    }
};
}
