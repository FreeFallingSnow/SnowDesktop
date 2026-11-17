#pragma once
#include "drag_drop/pending_drop_completion.h"
#include "layout/grid_occupancy.h"
#include <limits>

namespace snowdesktop::new_item_placement
{
inline bool PlaceOnPage(DesktopItem& item, GridCell requested, const GridPage& page,
    grid_occupancy::Slots& used)
{
    const GridSpan span{std::max(1, item.gridSpan.columns), std::max(1, item.gridSpan.rows)};
    if (page.columns < span.columns || page.rows < span.rows) return false;
    requested.column = std::clamp(requested.column, 0, page.columns - span.columns);
    requested.row = std::clamp(requested.row, 0, page.rows - span.rows);
    GridCell cell = requested;
    if (grid_occupancy::Occupied(used, cell, span))
    {
        int distance = std::numeric_limits<int>::max();
        for (int column = 0; column <= page.columns - span.columns; ++column)
            for (int row = 0; row <= page.rows - span.rows; ++row)
            {
                const int dx = column - requested.column;
                const int dy = row - requested.row;
                const int candidateDistance = dx * dx + dy * dy;
                if (candidateDistance >= distance) continue;
                GridCell candidate{page.id, column, row};
                if (grid_occupancy::Occupied(used, candidate, span)) continue;
                distance = candidateDistance;
                cell = std::move(candidate);
            }
        if (distance == std::numeric_limits<int>::max()) return false;
    }
    item.gridCell = cell;
    grid_occupancy::Mark(used, cell, span);
    return true;
}

enum class Result { Pending, Discarded, Applied };

// Resolve stable identities only when the actual created item is enumerated.
// This also corrects provisional auto-collection into a different widget.
template<class Normalize>
Result Apply(std::vector<DesktopWidget>& widgets, std::vector<DesktopItem>& items,
    const std::wstring& widgetId, const std::wstring& path, size_t insertIndex,
    Normalize&& normalize)
{
    const auto target = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return widget.id == widgetId; });
    if (target == widgets.end() || target->type != DesktopWidgetType::FileCategories)
        return Result::Discarded;
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& candidate) {
        return pending_drop::MatchesExactPath(candidate.parsingName, path);
    });
    if (item == items.end()) return Result::Pending;
    const auto key = normalize(item->layoutKey);
    if (key.empty()) return Result::Pending;
    PendingLandingEntry landing;
    landing.kind = DropLandingKind::WidgetIndex;
    landing.widgetId = widgetId;
    landing.insertIndex = insertIndex;
    return pending_drop::CommitKeyedLanding(widgets, *item, landing, key)
        ? Result::Applied : Result::Discarded;
}
}
