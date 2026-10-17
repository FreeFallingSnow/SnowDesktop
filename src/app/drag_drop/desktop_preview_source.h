#pragma once

#include "core/drop_model.h"
#include "desktop/desktop_membership_recovery.h"
#include <map>

namespace snowdesktop::desktop_preview_source
{
class Lookup
{
public:
    Lookup(const std::vector<DesktopItem>& items, bool active) : items_(items), active_(active) {}

    const DesktopItem* Find(const DragSourceEntry& source) const
    {
        if (!active_ || source.fromDock || source.kind != DropSourceKind::DesktopIcon || source.desktopWidgetIcon || source.desktopKey.empty())
            return nullptr;
        const DesktopItem* item = nullptr;
        if (source.desktopIndex < items_.size() &&
            CompareStringOrdinal(items_[source.desktopIndex].layoutKey.c_str(), -1, source.desktopKey.c_str(), -1, TRUE) == CSTR_EQUAL)
            item = &items_[source.desktopIndex];
        else
        {
            // One lookup lives for one draw. Keep the usual index fast path,
            // and build the current identity map once only after a mismatch.
            if (!indexed_)
            {
                for (const auto& current : items_) byKey_.try_emplace(current.layoutKey, &current);
                indexed_ = true;
            }
            const auto found = byKey_.find(source.desktopKey);
            if (found != byKey_.end()) item = found->second;
        }
        return item && item->largeIcon ? item : nullptr;
    }

private:
    const std::vector<DesktopItem>& items_;
    bool active_;
    mutable bool indexed_ = false;
    mutable std::map<std::wstring, const DesktopItem*, desktop_membership_recovery::KeyLess> byKey_;
};
}
