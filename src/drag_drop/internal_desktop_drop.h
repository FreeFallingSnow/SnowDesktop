#pragma once

#include "core/drop_model.h"
#include <functional>

namespace snowdesktop::internal_desktop_drop
{
struct Services
{
    std::function<void(const std::vector<std::wstring>&)> removeKeys;
    std::function<size_t(const std::wstring&)> findWidget, findItem;
    std::function<int(const GridCell&)> slot;
    std::function<void()> mapPages, committed;
};

// The host's existing internal desktop commit. Group-label releases retain
// their dedicated route before this boundary; file IO is never submitted here.
inline bool Execute(const DragSourceList& sources, const DropPreviewList& preview,
    std::vector<DesktopItem>& items, std::vector<DesktopWidget>& widgets, const Services& services)
{
    services.removeKeys(sources.DesktopKeys());
    bool changed = false;
    for (const auto& landing : preview.landings)
    {
        const auto source = std::find_if(sources.entries.begin(), sources.entries.end(),
            [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (source == sources.entries.end()) continue;
        if (source->desktopWidgetIcon)
        {
            const auto index = services.findWidget(source->widgetId);
            if (index < widgets.size()) { widgets[index].gridCell = landing.cell; changed = true; }
            continue;
        }
        if (source->desktopIndex >= items.size()) continue;
        items[source->desktopIndex].gridCell = landing.cell;
        items[source->desktopIndex].slot = services.slot(landing.cell);
        changed = true;
    }
    if (changed) services.committed();
    return changed;
}
}
