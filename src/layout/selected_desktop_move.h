#pragma once

#include "core/drop_model.h"
#include "selected_grid_move.h"
#include "widgets/single_cell_widget_rules.h"
#include "widgets/collection_group_rules.h"
#include "desktop/desktop_membership_recovery.h"
#include "desktop/desktop_drop_search.h"
#include <functional>
#include <map>

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
struct Selection { std::vector<bool> items, widgets; };

inline Selection Resolve(State state, const Services& services, const DragSourceList* sources)
{
    Selection result{std::vector<bool>(state.items.size()), std::vector<bool>(state.widgets.size())};
    if (!sources)
    {
        for (size_t index = 0; index < state.items.size(); ++index)
            result.items[index] = state.items[index].selected && !services.owned(state.items[index]);
        for (size_t index = 0; index < state.widgets.size(); ++index)
            result.widgets[index] = state.widgets[index].selected && single_cell_widget::IsIconMode(state.widgets[index]) &&
                !services.grouped(state.widgets[index]);
        return result;
    }
    std::map<std::wstring, size_t, desktop_membership_recovery::KeyLess> files;
    std::unordered_map<std::wstring, size_t> widgets;
    bool indexedFiles = false, indexedWidgets = false;
    for (const auto& source : sources->entries)
    {
        if (source.desktopWidgetIcon)
        {
            if (source.widgetId.empty()) continue;
            if (!indexedWidgets)
            {
                for (size_t index = 0; index < state.widgets.size(); ++index) widgets.try_emplace(state.widgets[index].id, index);
                indexedWidgets = true;
            }
            const auto found = widgets.find(source.widgetId);
            if (found != widgets.end() && single_cell_widget::IsIconMode(state.widgets[found->second]) && !services.grouped(state.widgets[found->second]))
                result.widgets[found->second] = true;
            continue;
        }
        if (source.kind != DropSourceKind::DesktopIcon || source.desktopKey.empty()) continue;
        size_t index = source.desktopIndex;
        // Keep the normal captured-index path cheap, but verify its identity.
        if (index >= state.items.size() || CompareStringOrdinal(state.items[index].layoutKey.c_str(), -1, source.desktopKey.c_str(), -1, TRUE) != CSTR_EQUAL)
        {
            if (!indexedFiles)
            {
                for (size_t item = 0; item < state.items.size(); ++item) files.try_emplace(state.items[item].layoutKey, item);
                indexedFiles = true;
            }
            const auto found = files.find(source.desktopKey);
            if (found == files.end()) continue;
            index = found->second;
        }
        if (!services.owned(state.items[index])) result.items[index] = true;
    }
    return result;
}

inline bool Occupied(State state, const GridCell& cell, GridSpan span, const Services& services, const Selection* ignored = nullptr)
{
    const auto& items_ = state.items; const auto& widgets_ = state.widgets;
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto& item = items_[index];
        if ((ignored ? ignored->items[index] : item.selected) || item.name.empty()) continue;
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
    for (size_t index = 0; index < widgets_.size(); ++index)
    {
        const auto& w = widgets_[index];
        if (ignored ? ignored->widgets[index] : w.selected && snowdesktop::single_cell_widget::IsIconMode(w) &&
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
    const auto selected = Resolve(state, services, sources);
    const auto& items_ = state.items; const auto& widgets_ = state.widgets; const auto& gridPages_ = state.pages;
    // The existing grid planner operates on geometry, so project both model
    // kinds into it and retain a stable mapping for the atomic commit.
    std::vector<DesktopItem> geometry;
    std::vector<std::pair<size_t, bool>> owners;
    geometry.reserve(items_.size() + widgets_.size());
    for (size_t index = 0; index < items_.size(); ++index)
    {
        const auto& item = items_[index];
        if (!selected.items[index]) continue;
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
        if (!selected.widgets[index]) continue;
        DesktopItem value;
        value.selected = true;
        value.gridCell = widget.gridCell;
        value.gridSpan = widget.gridSpan;
        geometry.push_back(std::move(value));
        owners.push_back({index, true});
    }
    const auto plan = snowdesktop::selected_grid_move::Build(geometry, gridPages_, std::move(targetCell),
        [&](const GridCell& cell, GridSpan span) { return Occupied(state, cell, span, services, &selected); });
    std::vector<Move> moves;
    moves.reserve(plan.size());
    for (const auto& move : plan)
        moves.push_back({owners[move.index].first, move.cell, owners[move.index].second});
    return moves;
}

struct SearchContext
{
    bool desktopOrigin = true;
    bool cacheActive = false;
    POINT current{};
    POINT mouseDown{};
    std::uint64_t sceneRevision = 0;
};

inline GridCell FindBest(State state, const Services& services, const DragSourceList& sources,
    GridCell target, desktop_drop_cache::BestCellEntry& cache, const SearchContext& context)
{
    const bool exactPlacement = context.desktopOrigin &&
        std::any_of(sources.entries.begin(), sources.entries.end(), [&](const DragSourceEntry& entry) {
            return entry.kind == DropSourceKind::DesktopIcon && !entry.fromDock &&
                entry.desktopIndex < state.items.size() && state.items[entry.desktopIndex].largeIcon.has_value();
        });
    const auto direction = desktop_drop_cache::ResolveSearchDirection(
        static_cast<long long>(context.current.x) - context.mouseDown.x,
        static_cast<long long>(context.current.y) - context.mouseDown.y);
    const desktop_drop_cache::BestCellKey key{target, direction, context.sceneRevision};
    const auto page = std::find_if(state.pages.begin(), state.pages.end(), [&](const auto& value) { return value.id == target.pageId; });
    return desktop_drop_cache::FindBestCell(cache, context.cacheActive, key, exactPlacement,
        page == state.pages.end() ? 0 : page->columns, page == state.pages.end() ? 0 : page->rows,
        [&](const GridCell& cell) { return !Build(state, cell, services, &sources).empty(); });
}

template<class Moves>
std::vector<DropLanding> Landings(State state, const DragSourceList& sourceList, const Moves& moves)
{
    std::vector<DropLanding> landings;
    using Value = typename Moves::value_type;
    std::map<std::wstring, const Value*, desktop_membership_recovery::KeyLess> files;
    std::unordered_map<std::wstring, const Value*> widgets;
    for (const auto& move : moves)
    {
        if (move.widget && move.index < state.widgets.size()) widgets.try_emplace(state.widgets[move.index].id, &move);
        else if (!move.widget && move.index < state.items.size()) files.try_emplace(state.items[move.index].layoutKey, &move);
    }
    for (const auto& entry : sourceList.entries)
    {
        const Value* move = nullptr;
        if (entry.desktopWidgetIcon)
        {
            const auto found = widgets.find(entry.widgetId);
            if (found != widgets.end()) move = found->second;
        }
        else if (entry.kind == DropSourceKind::DesktopIcon)
        {
            const auto found = files.find(entry.desktopKey);
            if (found != files.end()) move = found->second;
        }
        if (!move) continue;
        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = entry.sourceIndex;
        landing.cell = move->cell;
        landing.span = move->widget ? state.widgets[move->index].gridSpan : state.items[move->index].gridSpan;
        landings.push_back(std::move(landing));
    }
    return landings;
}
}
