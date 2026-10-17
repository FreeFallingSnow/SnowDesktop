#pragma once

#include "core/drop_model.h"
#include "selected_grid_move.h"
#include "widgets/single_cell_widget_rules.h"
#include "widgets/collection_group_rules.h"
#include <functional>

namespace snowdesktop::selected_desktop_move
{
struct State
{
    const std::vector<DesktopItem>& items;
    const std::vector<DesktopWidget>& widgets;
    const std::vector<GridPage>& pages;
};
struct Services
{
    std::function<bool(const DesktopItem&)> owned;
    std::function<bool(const DesktopWidget&)> grouped;
};
struct Move { size_t index; GridCell cell; bool widget; };

inline bool Occupied(State state, const GridCell& cell, GridSpan span, const Services& services)
{
    const auto& items_ = state.items; const auto& widgets_ = state.widgets;
    for (const auto& item : items_)
    {
        if (item.selected || item.name.empty()) continue;
        if (item.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = item.gridCell.column + std::max(1, item.gridSpan.columns);
        const int bottom2 = item.gridCell.row + std::max(1, item.gridSpan.rows);
        if (cell.column < right2 && right1 > item.gridCell.column &&
            cell.row < bottom2 && bottom1 > item.gridCell.row)
        {
            // Normalizing long layout keys is only needed for intersecting items.
            if (!services.owned(item)) return true;
        }
    }
    for (const auto& w : widgets_)
    {
        if (w.selected && snowdesktop::single_cell_widget::IsIconMode(w) &&
            !services.grouped(w)) continue;
        if (!snowdesktop::collection_group_rules::
                ShouldOccupyDesktopGrid(
                    services.grouped(w)))
            continue;
        if (w.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = w.gridCell.column + std::max(1, w.gridSpan.columns);
        const int bottom2 = w.gridCell.row + std::max(1, w.gridSpan.rows);
        if (cell.column < right2 && right1 > w.gridCell.column &&
            cell.row < bottom2 && bottom1 > w.gridCell.row)
            return true;
    }
    return false;
}

inline std::vector<Move> Build(State state, GridCell targetCell, const Services& services, const DragSourceList* sources = nullptr)
{
    (void)sources; // Initial extraction retains the host's live-selection behavior.
    const auto& items_ = state.items; const auto& widgets_ = state.widgets; const auto& gridPages_ = state.pages;
    // The existing grid planner operates on geometry, so project both model
    // kinds into it and retain a stable mapping for the atomic commit.
    std::vector<DesktopItem> geometry;
    std::vector<std::pair<size_t, bool>> owners;
    geometry.reserve(items_.size() + widgets_.size());
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto& item = items_[index];
        if (!item.selected || services.owned(item)) continue;
        DesktopItem value;
        value.selected = true;
        value.gridCell = item.gridCell;
        value.gridSpan = item.gridSpan;
        if (item.largeIcon) value.largeIcon.emplace();
        geometry.push_back(std::move(value));
        owners.push_back({index, false});
    }
    for (size_t index = 0; index < widgets_.size(); ++index)
    {
        const auto& widget = widgets_[index];
        if (!widget.selected || !snowdesktop::single_cell_widget::IsIconMode(widget) ||
            services.grouped(widget)) continue;
        DesktopItem value;
        value.selected = true;
        value.gridCell = widget.gridCell;
        value.gridSpan = widget.gridSpan;
        geometry.push_back(std::move(value));
        owners.push_back({index, true});
    }
    const auto plan = snowdesktop::selected_grid_move::Build(geometry, gridPages_, std::move(targetCell),
        [&](const GridCell& cell, GridSpan span) { return Occupied(state, cell, span, services); });
    std::vector<Move> moves;
    moves.reserve(plan.size());
    for (const auto& move : plan)
        moves.push_back({owners[move.index].first, move.cell, owners[move.index].second});
    return moves;
}
template<class Moves>
std::vector<DropLanding> Landings(State state, const DragSourceList& sourceList, const Moves& moves)
{
    std::vector<DropLanding> landings;
        for (const auto& entry : sourceList.entries)
        {
            auto it = std::find_if(moves.begin(), moves.end(),
                [&](const auto& move) {
                    return entry.desktopWidgetIcon
                        ? move.widget && move.index == static_cast<size_t>(std::find_if(state.widgets.begin(), state.widgets.end(), [&](const auto& widget) { return widget.id == entry.widgetId; }) - state.widgets.begin())
                        : !move.widget && move.index == entry.desktopIndex;
                });
            if (it == moves.end()) continue;
            DropLanding landing;
            landing.kind = DropLandingKind::DesktopCell;
            landing.sourceIndex = entry.sourceIndex;
            landing.cell = it->cell;
            landing.span = entry.originalSpan;
            landings.push_back(landing);
        }
    return landings;
}
}
