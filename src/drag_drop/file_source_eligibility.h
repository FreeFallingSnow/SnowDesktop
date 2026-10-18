#pragma once

#include "core/drop_model.h"

namespace snowdesktop::file_source_eligibility
{
inline bool ContainsProtectedDesktopIcons(const DragSourceList& sources)
{
    return std::any_of(sources.entries.begin(), sources.entries.end(),
        [](const auto& entry) { return entry.protectedDesktopIcon; });
}

// Preserve the captured sourceIndex: accepted landings and completion records
// must still refer to the original mixed selection, including skipped objects.
inline DragSourceList ForPhysicalFolder(const DragSourceList& sources)
{
    auto eligible = sources;
    std::erase_if(eligible.entries, [](const auto& entry) { return entry.protectedDesktopIcon; });
    eligible.hasDesktopIcons = std::any_of(eligible.entries.begin(), eligible.entries.end(),
        [](const auto& entry) { return entry.kind == DropSourceKind::DesktopIcon; });
    return eligible;
}
}
