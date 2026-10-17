#pragma once

#include "core/drop_model.h"

namespace snowdesktop::desktop_preview_source
{
class Lookup
{
public:
    Lookup(const std::vector<DesktopItem>& items, bool active) : items_(items), active_(active) {}

    const DesktopItem* Find(const DragSourceEntry& source) const
    {
        if (!active_ || source.fromDock || source.desktopIndex >= items_.size() || !items_[source.desktopIndex].largeIcon)
            return nullptr;
        return &items_[source.desktopIndex];
    }

private:
    const std::vector<DesktopItem>& items_;
    bool active_;
};
}
