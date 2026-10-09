#pragma once

#include "common/types.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::dock_running_drag
{
enum class Area { None, Fixed, Running, Files };

struct Target
{
    Area area = Area::None;
    size_t index = 0;
    long axis = 0;
};

// The separators belong to the adjoining pin areas. In particular, an empty
// fixed/file area still has a landing boundary before/after the running group.
constexpr Area AreaAt(long axis, long runningBegin, long filesBegin) noexcept
{
    if (axis < runningBegin) return Area::Fixed;
    if (axis >= filesBegin) return Area::Files;
    return Area::Running;
}

// Rotate the live objects: copying/erasing an app would duplicate bitmap
// ownership or discard sibling windows. The index is a pre-removal boundary.
template<class Apps>
bool Reorder(Apps& apps, const std::wstring& identity, size_t boundary)
{
    const auto found = std::find_if(apps.begin(), apps.end(), [&](const auto& app) {
        return app.identityKey == identity && app.presence.Interactive();
    });
    if (found == apps.end() || boundary > apps.size()) return false;
    const size_t from = static_cast<size_t>(found - apps.begin());
    const size_t to = boundary > from ? boundary - 1 : boundary;
    if (from == to) return false;
    if (from < to)
        std::rotate(found, found + 1, apps.begin() + static_cast<std::ptrdiff_t>(to + 1));
    else
        std::rotate(apps.begin() + static_cast<std::ptrdiff_t>(to), found, found + 1);
    return true;
}

// A Shell lookup can finish after another pin. Retain the actual neighboring
// entry rather than a Container/Slot pointer or a shifted numerical index.
struct PinPosition
{
    Area area = Area::None;
    size_t index = 0;
    std::optional<DockEntry> before;
    std::optional<DockEntry> after;

    size_t Resolve(const std::vector<DockEntry>& entries, size_t mainEnd,
        size_t filesEnd) const
    {
        const size_t begin = area == Area::Files ? mainEnd : 0;
        const size_t end = area == Area::Files ? filesEnd : mainEnd;
        const auto find = [&](const DockEntry& anchor) {
            for (size_t i = begin; i < end; ++i)
                if (entries[i].type == anchor.type && entries[i].reference == anchor.reference)
                    return i;
            return end;
        };
        if (before)
            if (const size_t i = find(*before); i < end) return i;
        if (after)
            if (const size_t i = find(*after); i < end) return i + 1;
        return std::clamp(index, begin, end);
    }
};
}
