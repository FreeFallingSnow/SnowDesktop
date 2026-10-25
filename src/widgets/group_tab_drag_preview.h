#pragma once

#include "core/drag_session.h"
#include "core/widget.h"
#include "widget_group_drop.h"
#include <climits>

namespace snowdesktop::group_tab_drag_preview
{
inline bool IsTabInsertion(const Slot* slot, HitRegion region)
{
    if (!slot || (region != HitRegion::SortBefore && region != HitRegion::SortAfter)) return false;
    const auto role = slot->GetFeedbackIdentity().role;
    return role == SlotFeedbackRole::CollectionGroupTab || role == SlotFeedbackRole::FileGroupSourceTab;
}

inline bool IsLabelDrag(const DragSourceList& source, DesktopWidgetType group)
{
    return group == DesktopWidgetType::CollectionGroup ? source.hasCollectionGroupEntries
        : group == DesktopWidgetType::FileGroup && source.hasFileGroupSourceLabels;
}

// Project the whole source component onto its tab strip. Use the unshifted
// geometry for hit testing, as quick navigation does, so making room cannot
// move a boundary back under a stationary pointer and oscillate the order.
template<class TabRect>
size_t NearestTab(size_t count, POINT point, TabRect tabRect)
{
    size_t nearest = static_cast<size_t>(-1);
    LONG distance = LONG_MAX;
    for (size_t i = 0; i < count; ++i)
    {
        const RECT tab = tabRect(i);
        if (IsRectEmpty(&tab)) continue;
        const LONG delta = point.x < tab.left ? tab.left - point.x
            : point.x > tab.right ? point.x - tab.right : 0;
        if (delta < distance) { distance = delta; nearest = i; }
    }
    return nearest;
}

struct Tab
{
    std::wstring id;
    std::wstring label;
    RECT bounds{};
    bool placeholder = false;
};

struct Layout
{
    std::vector<Tab> tabs;
    int totalWidth = 0;
    int scroll = 0;
    bool active = false;
};

// This is a presentation plan only. It neither reorders the model nor creates
// temporary components; cancellation and leaving a target need no rollback.
template<class Label>
Layout Build(ScrollingItemWidget& view, const DragSession& session,
    const std::vector<DesktopWidget>& widgets, const std::vector<std::wstring>& visible,
    RECT strip, Label label)
{
    Layout result;
    const auto* group = view.GetWidgetData();
    if (!group || !session.IsActive() || session.TargetContainer() != &view ||
        !IsTabInsertion(session.TargetSlot(), session.TargetRegion()) || IsRectEmpty(&strip)) return result;
    const auto& source = session.SourceList();
    if (!IsLabelDrag(source, group->type) && session.Action() != DropAction::Move) return result;
    std::vector<Tab> moving;
    for (const auto& entry : source.entries)
    {
        if (entry.kind != DropSourceKind::CollectionGroupEntry && entry.kind != DropSourceKind::FileGroupEntry &&
            entry.kind != DropSourceKind::Widget) return result;
        const auto& id = entry.fromDock ? entry.dockReference : entry.widgetId;
        const auto child = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& widget) { return widget.id == id; });
        if (child == widgets.end() || !widget_group_drop::AcceptsChild(group->type, child->type)) return result;
        if (std::none_of(moving.begin(), moving.end(), [&](const auto& tab) { return tab.id == id; }))
            moving.push_back({id, label(*child), {}, true});
    }
    if (moving.empty()) return result;
    const size_t rawInsertion = std::min(view.GetDropInsertIndex(session.TargetSlot(), session.TargetRegion()),
        group->childWidgetIds.size());
    size_t insertion = 0;
    for (size_t i = 0; i < group->childWidgetIds.size(); ++i)
    {
        const auto& id = group->childWidgetIds[i];
        if (std::find(visible.begin(), visible.end(), id) == visible.end() ||
            std::any_of(moving.begin(), moving.end(), [&](const auto& tab) { return tab.id == id; })) continue;
        const auto child = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& widget) { return widget.id == id; });
        if (child == widgets.end()) continue;
        result.tabs.push_back({id, label(*child), {}, false});
        if (i < rawInsertion) ++insertion;
    }
    result.tabs.insert(result.tabs.begin() + static_cast<std::ptrdiff_t>(insertion), moving.begin(), moving.end());
    std::vector<std::wstring> labels;
    for (const auto& tab : result.tabs) labels.push_back(tab.label);
    const auto widths = view.BuildCategorizedTabWidths(labels, strip.right - strip.left);
    for (const int width : widths) result.totalWidth += width;
    result.scroll = collection_group_rules::ClampIndependentTabScroll(group->tabScrollOffset,
        result.totalWidth, strip.right - strip.left);
    LONG left = strip.left - result.scroll;
    for (size_t i = 0; i < result.tabs.size(); ++i)
    {
        result.tabs[i].bounds = {left, strip.top, left + widths[i], strip.bottom};
        InflateRect(&result.tabs[i].bounds, -view.Cu(2.0f), -view.Cu(2.0f));
        left += widths[i];
    }
    result.active = true;
    return result;
}

inline void Draw(ScrollingItemWidget& view, ID2D1DeviceContext* context,
    const Layout& layout, RECT strip, const std::wstring& activeId)
{
    for (const auto& tab : layout.tabs)
    {
        RECT visible{};
        if (!IntersectRect(&visible, &tab.bounds, &strip)) continue;
        view.DrawCategorizedTab(context, visible, tab.bounds, tab.label,
            tab.id == activeId, false, tab.placeholder);
    }
}
}
