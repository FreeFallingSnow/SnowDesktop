#pragma once

#include "core/drop_model.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace snowdesktop::desktop_drop_dispatch
{
inline bool OrdinaryFiles(const DragSourceList& sources)
{
    return !sources.Empty() && std::all_of(sources.entries.begin(), sources.entries.end(), [](const auto& entry) {
        return entry.kind == DropSourceKind::DesktopIcon && entry.fromDock &&
            entry.dockEntryType == DockEntryType::DesktopItem && !entry.dockReference.empty() &&
            !entry.filePath.empty() && !entry.protectedDesktopIcon;
    });
}

// DesktopGrid has a dedicated Dock return before its generic file pipeline.
// Only ordinary path-backed Dock entries have the independent Copy/Link file
// contract here. Component/mixed and generated-item policies stay at the host.
template<class ReturnDock, class ExecuteFiles>
std::optional<bool> Dock(bool dockOrigin, const DragSourceList& sources, int modifiers,
    ReturnDock returnDock, ExecuteFiles executeFiles)
{
    if (!dockOrigin) return std::nullopt;
    const auto action = DropActionFromMods(modifiers, DropAction::Move);
    if (!OrdinaryFiles(sources) || action == DropAction::Move) return returnDock();
    return executeFiles(sources, modifiers);
}
}
