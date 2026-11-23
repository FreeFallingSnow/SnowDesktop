#pragma once

#include "core/desktop.h"
#include "core/drag_session.h"
#include "core/widget.h"

#include <algorithm>

namespace snowdesktop::desktop_drop_feedback
{
inline DesktopGrid* Target(const DragSession& session)
{
    auto* desktop = dynamic_cast<DesktopGrid*>(session.TargetContainer());
    if (!desktop || dynamic_cast<const DesktopGrid*>(session.Source()))
        return desktop;

    // Native widget file members use the same desktop landing renderer.
    // Widget objects, group tabs and search projections keep their own
    // presentation paths.
    if (!dynamic_cast<const ScrollingItemWidget*>(session.Source()))
        return nullptr;
    const auto& entries = session.SourceList().entries;
    const bool fileMembers = !entries.empty() &&
        std::all_of(entries.begin(), entries.end(), [](const DragSourceEntry& entry) {
            return (entry.kind == DropSourceKind::DesktopIcon && !entry.desktopWidgetIcon) ||
                entry.kind == DropSourceKind::FolderEntry;
        });
    return fileMembers ? desktop : nullptr;
}
}
