#pragma once

#include "types.h"

#include <cstdint>
#include <optional>

namespace snowdesktop::desktop_drop_cache
{

struct SearchDirection
{
    int column = 1;
    int row = 0;

    bool operator==(const SearchDirection&) const = default;
};

inline SearchDirection ResolveSearchDirection(
    long long dx, long long dy) noexcept
{
    const long long absDx = dx < 0 ? -dx : dx;
    const long long absDy = dy < 0 ? -dy : dy;
    if (absDx >= absDy)
        return { dx >= 0 ? 1 : -1, 0 };
    return { 0, dy >= 0 ? 1 : -1 };
}

inline bool SameCell(
    const GridCell& left,
    const GridCell& right) noexcept
{
    return left.pageId == right.pageId &&
        left.column == right.column &&
        left.row == right.row;
}

struct BestCellKey
{
    GridCell requested;
    SearchDirection direction;
    std::uint64_t staticSceneRevision = 0;
};

inline bool SameKey(
    const BestCellKey& left,
    const BestCellKey& right) noexcept
{
    return SameCell(left.requested, right.requested) &&
        left.direction == right.direction &&
        left.staticSceneRevision == right.staticSceneRevision;
}

class BestCellEntry
{
public:
    bool TryGet(
        bool dragActive,
        const BestCellKey& key,
        GridCell& value) const
    {
        if (!dragActive || !key_ ||
            !SameKey(*key_, key))
            return false;
        value = value_;
        return true;
    }

    void Store(
        bool dragActive,
        const BestCellKey& key,
        const GridCell& value)
    {
        if (!dragActive)
            return;
        key_ = key;
        value_ = value;
    }

    void Clear()
    {
        key_.reset();
        value_ = {};
    }

private:
    std::optional<BestCellKey> key_;
    GridCell value_;
};

} // namespace snowdesktop::desktop_drop_cache
