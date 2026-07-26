#pragma once

#include "core/drop_model.h"

#include <windows.h>

namespace snowdesktop::dock_drop_rules
{

// External resources pinned to Dock are represented by a shortcut on the
// managed desktop. The source file must never be moved into that directory.
inline DropAction ExternalMappingAction() noexcept
{
    return DropAction::Link;
}

// Prefer the native link cursor. Some drag sources expose only copy/move; copy
// is still safe because SnowDesktop creates its own link and the source must
// retain the original. Move-only sources are rejected.
inline DWORD ChooseExternalMappingEffect(DWORD allowed) noexcept
{
    const DWORD available =
        allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (available & DROPEFFECT_LINK)
        return DROPEFFECT_LINK;
    if (available & DROPEFFECT_COPY)
        return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}

// Items with a dedicated Dock position, such as Recycle Bin, ignore sortable
// insertion indices and therefore must not show the fixed-area insertion bar.
inline bool ShouldDrawSortableInsertionIndicator(
    bool fixedPlacementSource) noexcept
{
    return !fixedPlacementSource;
}

} // namespace snowdesktop::dock_drop_rules
