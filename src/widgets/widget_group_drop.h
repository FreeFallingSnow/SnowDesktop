#pragma once

#include "core/drop_model.h"
#include "collection_group_rules.h"
#include "layout/grid_occupancy.h"

#include <unordered_set>

namespace snowdesktop::widget_group_drop
{
constexpr bool AcceptsChild(DesktopWidgetType group, DesktopWidgetType child)
{
    return group == DesktopWidgetType::CollectionGroup ? child == DesktopWidgetType::Collection
        : group == DesktopWidgetType::FileGroup &&
            (child == DesktopWidgetType::FileCategories || child == DesktopWidgetType::FolderMapping);
}

// One model transaction shared by desktop entrances, Dock entries and
// group labels. Validate the entire membership change before detaching anything.
inline bool Apply(std::vector<DesktopWidget>& widgets, std::vector<DockEntry>& dock,
    size_t groupIndex, const std::vector<std::wstring>& requested, size_t insertIndex)
{
    if (groupIndex >= widgets.size() || widgets[groupIndex].id.empty() ||
        !IsGroupWidgetType(widgets[groupIndex].type) || requested.empty()) return false;
    const auto groupType = widgets[groupIndex].type;
    const auto uniqueIndex = [&](const std::wstring& id) {
        size_t result = widgets.size();
        for (size_t i = 0; i < widgets.size(); ++i)
            if (widgets[i].id == id)
            {
                if (result != widgets.size()) return widgets.size();
                result = i;
            }
        return result;
    };
    if (uniqueIndex(widgets[groupIndex].id) != groupIndex) return false;
    std::vector<std::wstring> moving;
    for (const auto& id : requested)
    {
        const size_t index = uniqueIndex(id);
        if (id.empty() || index >= widgets.size() || index == groupIndex ||
            !AcceptsChild(groupType, widgets[index].type)) return false;
        size_t owners = 0;
        for (const auto& widget : widgets)
            if (IsGroupWidgetType(widget.type))
                for (const auto& child : widget.childWidgetIds)
                    if (child == id && (++owners > 1 || widget.type != groupType)) return false;
        if (std::find(moving.begin(), moving.end(), id) == moving.end()) moving.push_back(id);
    }
    auto& target = widgets[groupIndex];
    const std::wstring active = target.activeCategoryId;
    insertIndex = std::min(insertIndex, target.childWidgetIds.size());
    const std::unordered_set<std::wstring> movingSet(moving.begin(), moving.end());
    size_t removedBefore = 0;
    for (size_t i = 0; i < insertIndex; ++i)
        if (movingSet.contains(target.childWidgetIds[i])) ++removedBefore;
    for (auto& widget : widgets)
    {
        if (widget.type != groupType) continue;
        std::erase_if(widget.childWidgetIds, [&](const auto& id) { return movingSet.contains(id); });
        widget.activeCategoryId = collection_group_rules::ResolveActiveItem(widget.childWidgetIds, widget.activeCategoryId);
    }
    const size_t insertAt = std::min(insertIndex - removedBefore, target.childWidgetIds.size());
    target.childWidgetIds.insert(target.childWidgetIds.begin() + static_cast<std::ptrdiff_t>(insertAt), moving.begin(), moving.end());
    target.activeCategoryId = collection_group_rules::ResolveActiveItem(target.childWidgetIds, active);
    std::erase_if(dock, [&](const auto& entry) {
        return (entry.type == DockEntryType::Collection || entry.type == DockEntryType::DesktopFiles ||
            entry.type == DockEntryType::FolderMapping) && movingSet.contains(entry.reference);
    });
    for (auto& widget : widgets)
        if (movingSet.contains(widget.id)) widget.selected = false;
    return true;
}

// Internal label drops used to have a second, unchecked remove/insert path.
// Keep the captured preview binding, then use the same validated transaction
// as component entrances and Dock sources. Never dereference a stale pointer.
inline bool ApplyPlan(std::vector<DesktopWidget>& widgets, std::vector<DockEntry>& dock,
    const DragSourceList& sources, const DropPreviewList& preview)
{
    if (preview.fileBacked || preview.targetKind != DropTargetKind::KeyedWidget) return false;
    const auto target = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return &widget == preview.targetWidget; });
    if (target == widgets.end()) return false;
    if (preview.landings.empty() || std::any_of(preview.landings.begin(), preview.landings.end(),
        [&](const auto& landing) {
            return landing.kind != DropLandingKind::WidgetIndex || landing.widget != preview.targetWidget ||
                landing.widgetId != target->id;
        })) return false;
    const bool collections = sources.hasCollectionGroupEntries && target->type == DesktopWidgetType::CollectionGroup;
    const bool files = sources.hasFileGroupEntries && target->type == DesktopWidgetType::FileGroup;
    if (!collections && !files) return false;
    std::vector<std::wstring> movingIds;
    for (const auto& source : sources.entries)
        if (source.kind == (collections ? DropSourceKind::CollectionGroupEntry : DropSourceKind::FileGroupEntry) &&
            !source.widgetId.empty()) movingIds.push_back(source.widgetId);
    return Apply(widgets, dock, static_cast<size_t>(target - widgets.begin()), movingIds, preview.insertIndex);
}

struct ReleasePlacement
{
    GridCell cell;
    GridSpan span;
};

// Page selection, clamping and collision decisions are shared with the host's
// release preview. Native span limits and configured page order remain services.
template<class ClampSpan, class FindFree>
std::optional<ReleasePlacement> PlanRelease(const DesktopWidget& child, GridCell preferred,
    const GridCell& fallback, const std::vector<GridPage>& pages, grid_occupancy::Slots& used,
    bool allowSearch, ClampSpan clampSpan, FindFree findFree)
{
    const auto findPage = [&](const auto& id) -> const GridPage* {
        const auto found = std::find_if(pages.begin(), pages.end(), [&](const auto& page) { return page.id == id; });
        return found == pages.end() ? nullptr : &*found;
    };
    const GridPage* page = findPage(preferred.pageId);
    if (!page) { preferred = fallback; page = findPage(preferred.pageId); }
    if (!page) return std::nullopt;
    const GridSpan span = clampSpan(child, child.gridSpan, page->columns, page->rows);
    preferred.column = std::clamp(preferred.column, 0, std::max(0, page->columns - span.columns));
    preferred.row = std::clamp(preferred.row, 0, std::max(0, page->rows - span.rows));
    GridCell landing = preferred;
    if (!grid_occupancy::Fits(*page, landing, span) || grid_occupancy::Occupied(used, landing, span))
    {
        const int start = preferred.column * std::max(1, page->rows) + preferred.row;
        if (!allowSearch || (!findFree(span, used, landing, preferred.pageId, start) &&
            !findFree(span, used, landing, L"", 0))) return std::nullopt;
    }
    grid_occupancy::Mark(used, landing, span);
    return ReleasePlacement{std::move(landing), span};
}

// A rejected release is a no-op, including transient selection. Validate and
// plan before detaching the child so failure needs no partial model rollback.
template<class Plan>
bool Release(std::vector<DesktopWidget>& widgets, const std::wstring& id,
    DesktopWidgetType groupType, Plan plan)
{
    size_t childIndex = widgets.size(), ownerIndex = widgets.size(), membershipIndex = 0;
    for (size_t index = 0; index < widgets.size(); ++index)
    {
        const auto& widget = widgets[index];
        if (widget.id == id)
        {
            if (childIndex != widgets.size()) return false;
            childIndex = index;
        }
        for (size_t member = 0; member < widget.childWidgetIds.size(); ++member)
            if (widget.childWidgetIds[member] == id)
            {
                if (ownerIndex != widgets.size() || widget.type != groupType) return false;
                ownerIndex = index; membershipIndex = member;
            }
    }
    if (id.empty() || childIndex >= widgets.size() || ownerIndex >= widgets.size() ||
        childIndex == ownerIndex || !AcceptsChild(groupType, widgets[childIndex].type)) return false;
    if (widgets[ownerIndex].id.empty() || std::count_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return widget.id == widgets[ownerIndex].id; }) != 1) return false;
    const auto placement = plan(childIndex, ownerIndex);
    if (!placement) return false;
    auto& group = widgets[ownerIndex];
    auto& child = widgets[childIndex];
    group.childWidgetIds.erase(group.childWidgetIds.begin() + static_cast<std::ptrdiff_t>(membershipIndex));
    if (group.activeCategoryId == id)
        group.activeCategoryId = group.childWidgetIds.empty() ? L"" :
            group.childWidgetIds[std::min(membershipIndex, group.childWidgetIds.size() - 1)];
    child.gridCell = placement->cell;
    child.gridSpan = placement->span;
    child.selected = false;
    return true;
}

template<class ReleaseChild>
bool ReleasePlan(const DragSourceList& sources, const DropPreviewList& preview, ReleaseChild release)
{
    if (preview.fileBacked || preview.targetKind != DropTargetKind::Desktop ||
        (!sources.hasCollectionGroupEntries && !sources.hasFileGroupEntries)) return false;
    bool changed = false;
    for (const auto& landing : preview.landings)
    {
        const auto source = std::find_if(sources.entries.begin(), sources.entries.end(),
            [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (source == sources.entries.end() || source->widgetId.empty()) continue;
        changed = release(source->widgetId, landing.cell, sources.hasCollectionGroupEntries) || changed;
    }
    return changed;
}
}
