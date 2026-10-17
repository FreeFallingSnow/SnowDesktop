#pragma once

#include "drag_drop/pending_drop_completion.h"
#include "layout/folder_sort_rules.h"
#include "layout/grid_occupancy.h"
#include <functional>

namespace snowdesktop::pending_drop
{
template<class PageGeometry, class FindFreeCell, class SlotForCell>
inline bool PlaceDesktop(DesktopItem& item, const PendingLandingEntry& landing,
    grid_occupancy::Slots& used, PageGeometry pageGeometry, FindFreeCell findFree,
    SlotForCell slotForCell)
{
    GridSpan span{std::max(1, item.gridSpan.columns), std::max(1, item.gridSpan.rows)};
    GridCell cell = landing.cell;
    const auto page = pageGeometry(cell.pageId);
    const bool fits = page && grid_occupancy::Fits(*page, cell, span);
    if ((!fits || grid_occupancy::Occupied(used, cell, span)) &&
        !findFree(span, used, cell, landing.cell.pageId, slotForCell(landing.cell))) return false;
    item.gridCell = cell;
    item.slot = slotForCell(cell);
    item.selected = true;
    grid_occupancy::Mark(used, cell, span);
    return true;
}

// Reconcile actual operation outputs against a freshly enumerated model. The
// caller supplies locale/path comparisons and acceptance/rendering services;
// matching, stable target selection, membership and insertion are shared.
template<class MatchName, class NormalizeKey, class PathsEqual>
inline bool ApplyFolder(PendingLandingCache& cache, DesktopWidget& targetWidget,
    const std::wstring& widgetId, const std::wstring& popupSourceId,
    MatchName matchName, NormalizeKey normalizeKey, PathsEqual pathsEqual)
{
    if (!cache.active ||
        cache.folderPlacements.empty())
        return false;
    bool changed = false;
    std::vector<PendingFolderPlacement> remaining;
    remaining.reserve(
        cache.folderPlacements.size());
    for (auto& placement :
        cache.folderPlacements)
    {
        const bool widgetMatches =
            !widgetId.empty() &&
            !placement.widgetId.empty() &&
            placement.widgetId == widgetId;
        const bool popupMatches =
            !popupSourceId.empty() &&
            !placement.popupSourceId.empty() &&
            placement.popupSourceId == popupSourceId;
        const bool pathMatches =
            placement.sourceFolderPath.empty() ||
            pathsEqual(
                placement.sourceFolderPath,
                targetWidget.sourceFolderPath);
        if ((!widgetMatches && !popupMatches) || !pathMatches)
        {
            remaining.push_back(std::move(placement));
            continue;
        }

        std::vector<FolderEntry> inserted =
            snowdesktop::pending_drop_rules::ExtractMatching(
                targetWidget.folderEntries,
                [&](const FolderEntry& entry) {
                    if (placement.existingPaths.contains(normalizeKey(entry.fullPath))) return false;
                    if (!placement.createdPaths.empty())
                        return std::any_of(placement.createdPaths.begin(), placement.createdPaths.end(),
                            [&](const auto& path) { return snowdesktop::pending_drop::MatchesExactPath(entry.fullPath, path); });
                    return placement.sourceNames.empty() ||
                        std::any_of(placement.sourceNames.begin(), placement.sourceNames.end(),
                            [&](const auto& name) { return matchName(entry.name, name); });
                });

        if ((!placement.createdPaths.empty() || !placement.sourceNames.empty()) &&
            inserted.size() > 1)
        {
            std::vector<FolderEntry> ordered;
            ordered.reserve(inserted.size());
            for (const auto& sourceName :
                (placement.createdPaths.empty() ? placement.sourceNames : placement.createdPaths))
            {
                auto match = std::find_if(
                    inserted.begin(), inserted.end(),
                    [&](const FolderEntry& entry) {
                        return placement.createdPaths.empty() ? matchName(entry.name, sourceName) :
                            snowdesktop::pending_drop::MatchesExactPath(entry.fullPath, sourceName);
                    });
                if (match == inserted.end())
                    continue;
                ordered.push_back(std::move(*match));
                inserted.erase(match);
            }
            std::move(
                inserted.begin(), inserted.end(),
                std::back_inserter(ordered));
            inserted = std::move(ordered);
        }

        if (!inserted.empty())
        {
            snowdesktop::pending_drop_rules::InsertAt(
                targetWidget.folderEntries,
                placement.insertIndex,
                std::move(inserted));
            snowdesktop::folder_sort_rules::RewriteOrderKeys(
                targetWidget.folderEntries,
                targetWidget.itemKeys);
            targetWidget.folderSortMode =
                snowdesktop::folder_sort_rules::kManual;
            targetWidget.contentSortColumn =
                snowdesktop::list_detail_rules::Column::None;
            changed = true;
        }
        // A successful file operation gets exactly one reconciliation pass.
        // If it overwrote an existing path there is no new member to move, and
        // retaining this snapshot could capture an unrelated later file.
    }

    cache.folderPlacements =
        std::move(remaining);
    cache.active =
        !cache.entries.empty() ||
        !cache.folderPlacements.empty();
    return changed;
}

template<class NormalizeKey, class MatchesLegacyName, class AllowsKey,
    class PlaceDesktop, class KeyCommitted>
inline bool ApplyEntries(PendingLandingCache& cache, std::vector<DesktopWidget>& widgets,
    std::vector<DesktopItem>& items, std::unordered_set<std::wstring>& claimed,
    NormalizeKey normalizeKey, MatchesLegacyName matchesLegacyName, AllowsKey allowsKey,
    PlaceDesktop placeDesktop, KeyCommitted keyCommitted)
{
    if (!cache.active) return false;
    bool changed = false;
    for (const auto& landing : cache.entries)
    {
        for (auto& item : items)
        {
            const auto key = normalizeKey(item.layoutKey);
            if (key.empty() || claimed.contains(key) || cache.existingDesktopKeys.contains(key)) continue;
            const bool matches = !landing.createdPath.empty()
                ? MatchesExactPath(item.parsingName, landing.createdPath)
                : matchesLegacyName(item, landing.sourceName);
            if (!matches) continue;
            if (landing.kind == DropLandingKind::WidgetIndex && !landing.widgetId.empty())
            {
                const auto target = std::find_if(widgets.begin(), widgets.end(),
                    [&](const auto& widget) { return widget.id == landing.widgetId; });
                if (target == widgets.end() || !allowsKey(landing, key)) break;
                if (!CommitKeyedLanding(widgets, item, landing, key)) break;
                keyCommitted(landing.widgetId);
            }
            else if (landing.kind == DropLandingKind::DesktopCell)
            {
                if (!placeDesktop(item, landing)) break;
            }
            else break;
            claimed.insert(key);
            changed = true;
            break;
        }
    }
    cache.entries.clear();
    cache.active = !cache.folderPlacements.empty();
    return changed;
}
}
