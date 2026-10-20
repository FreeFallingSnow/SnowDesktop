#pragma once

#include "app/layout/grid_geometry.h"
#include "core/drag_session.h"

namespace snowdesktop::desktop_request
{
inline POINT TargetPoint(const DragSession& session, POINT groupOrigin, POINT legacyMouseDown, POINT current)
{
    // A synchronous drop ends rendering before it consumes the retained context.
    if (session.HasContext()) return session.ResolveTargetPoint(groupOrigin, current);
    return {groupOrigin.x + (current.x - legacyMouseDown.x), groupOrigin.y + (current.y - legacyMouseDown.y)};
}

inline GridCell Resolve(const std::vector<GridPage>& pages, const DragSourceList& sources,
    const DragSession& session, POINT groupOrigin, POINT legacyMouseDown, POINT current,
    const GridPage* fallback = nullptr)
{
    if (sources.UsesPointerDesktopPlacement() || session.IsPointerAnchored())
        return GridCellFromPoint(pages, current, fallback);
    return ResolveGridDragCell(pages, current, TargetPoint(session, groupOrigin, legacyMouseDown, current), fallback);
}
}
