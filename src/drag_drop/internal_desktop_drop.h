#pragma once

#include "core/drop_model.h"
#include <functional>

namespace snowdesktop::internal_desktop_drop
{
struct PageState
{
    std::vector<std::wstring>& ids;
    std::unordered_map<std::wstring, int>& columns;
    std::unordered_map<std::wstring, int>& rows;
};
struct Services
{
    std::function<void(const std::vector<std::wstring>&)> removeKeys;
    std::function<size_t(const std::wstring&)> findWidget, findItem;
    std::function<int(const GridCell&)> slot;
    std::function<void()> prepareLayout, mapPages, committed;
};

// The host's internal desktop commit. Group-label releases retain
// their dedicated route before this boundary; file IO is never submitted here.
inline bool Execute(const DragSourceList& sources, const DropPreviewList& preview,
    std::vector<DesktopItem>& items, std::vector<DesktopWidget>& widgets, PageState pages, const Services& services)
{
    struct Move { size_t index; bool widget; GridCell cell; std::optional<GridSpan> plannedPageSize; };
    std::vector<Move> moves;
    std::vector<std::wstring> removedKeys;
    for (const auto& landing : preview.landings)
    {
        const auto source = std::find_if(sources.entries.begin(), sources.entries.end(),
            [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (source == sources.entries.end()) continue;
        if (source->desktopWidgetIcon)
        {
            const auto index = services.findWidget(source->widgetId);
            if (index < widgets.size()) moves.push_back({index, true, landing.cell, landing.plannedPageSize});
            continue;
        }
        if (source->desktopKey.empty()) continue;
        const auto matches = [&](const auto& item) {
            return CompareStringOrdinal(item.layoutKey.c_str(), -1, source->desktopKey.c_str(), -1, TRUE) == CSTR_EQUAL;
        };
        auto index = services.findItem(source->desktopKey);
        // The captured desktopIndex is an enumeration position, not identity.
        // Even the live lookup cache must be checked before changing an item.
        if (index >= items.size() || !matches(items[index]))
        {
            const auto current = std::find_if(items.begin(), items.end(), matches);
            if (current == items.end()) continue;
            index = static_cast<size_t>(current - items.begin());
        }
        moves.push_back({index, false, landing.cell, landing.plannedPageSize});
        removedKeys.push_back(items[index].layoutKey);
    }
    if (moves.empty()) return false;
    std::vector<std::wstring> newIds;
    std::unordered_map<std::wstring, GridSpan> additions;
    for (const auto& move : moves)
    {
        const auto& id = move.cell.pageId;
        const auto column = pages.columns.find(id), row = pages.rows.find(id);
        GridSpan dimensions;
        if (move.plannedPageSize)
        {
            // An unrelated page may have claimed the preview's reserved ID
            // before commit. Do not redirect the file into that replacement.
            if (column != pages.columns.end() || row != pages.rows.end() ||
                std::find(pages.ids.begin(), pages.ids.end(), id) != pages.ids.end()) return false;
            dimensions = *move.plannedPageSize;
            if (const auto previous = additions.find(id); previous != additions.end())
            {
                if (previous->second.columns != dimensions.columns || previous->second.rows != dimensions.rows) return false;
            }
            else { additions.emplace(id, dimensions); newIds.push_back(id); }
        }
        else
        {
            if (column == pages.columns.end() || row == pages.rows.end() ||
                std::find(pages.ids.begin(), pages.ids.end(), id) == pages.ids.end()) return false;
            dimensions = {column->second, row->second};
        }
        const auto span = move.widget ? widgets[move.index].gridSpan : items[move.index].gridSpan;
        if (id.empty() || span.columns < 1 || span.rows < 1 || dimensions.columns < span.columns || dimensions.rows < span.rows ||
            move.cell.column < 0 || move.cell.row < 0 || move.cell.column > dimensions.columns - span.columns ||
            move.cell.row > dimensions.rows - span.rows) return false;
    }
    for (const auto& id : newIds)
    {
        pages.ids.push_back(id);
        pages.columns[id] = additions.at(id).columns; pages.rows[id] = additions.at(id).rows;
    }
    // Only accepted identities revoke ownership. A rejected or unmatched plan
    // cannot change membership merely because sources were captured earlier.
    services.removeKeys(removedKeys);
    for (const auto& move : moves)
        if (move.widget) widgets[move.index].gridCell = move.cell;
        else
        {
            items[move.index].gridCell = move.cell;
            items[move.index].slot = services.slot(move.cell);
        }
    services.prepareLayout();
    services.mapPages();
    services.committed();
    return true;
}
}
