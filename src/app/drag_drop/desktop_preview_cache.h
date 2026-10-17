#pragma once

#include "core/drop_model.h"
#include "core/slot.h"
#include <cstdint>

namespace snowdesktop::desktop_preview_cache
{
struct Key
{
    bool hasItems;
    POINT point;
    int mods;
    Container* target;
    Slot* slot;
    HitRegion region;
    size_t sourceCount;
    std::uint64_t sceneRevision;
    size_t externalCount;
};
struct State
{
    DropPreviewList& preview;
    POINT& point;
    int& mods;
    Container*& target;
    Slot*& slot;
    HitRegion& region;
    bool& hasItems;
    size_t& sourceCount;
    std::uint64_t& sceneRevision;
    size_t& externalCount;
};

template<class Internal, class External>
const DropPreviewList& Read(State state, const Key& key, Internal&& internal, External&& external)
{
    const bool valid = !state.preview.landings.empty() && state.hasItems == key.hasItems &&
        state.point.x == key.point.x && state.point.y == key.point.y && state.mods == key.mods &&
        state.target == key.target && state.slot == key.slot && state.region == key.region && state.sourceCount == key.sourceCount && state.sceneRevision == key.sceneRevision && state.externalCount == key.externalCount;
    if (!valid)
    {
        state.preview = key.hasItems ? internal() : external();
        state.point = key.point; state.mods = key.mods; state.target = key.target; state.slot = key.slot;
        state.region = key.region; state.hasItems = key.hasItems; state.sourceCount = key.sourceCount;
        state.sceneRevision = key.sceneRevision; state.externalCount = key.externalCount;
    }
    return state.preview;
}
}
