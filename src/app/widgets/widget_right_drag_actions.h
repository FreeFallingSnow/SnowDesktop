#pragma once

#include "core/drop_model.h"
#include "widgets/single_cell_widget_rules.h"
#include "widgets/widget_pair_drop.h"
#include <vector>

namespace snowdesktop::widget_right_drag
{
struct PairChoice
{
    UINT command;
    DWORD modifiers;
    widget_pair_drop::Action action;
    const char* label;
};

// Shift is the ordinary file-move verb, but it means merge for components.
constexpr DWORD MoveModifiers(bool hasWidgets) noexcept
{
    return hasWidgets ? 0 : MK_SHIFT;
}

// A menu pumps messages while runtime containers may be rebuilt. Validate only
// captured model identities before rebinding; never dereference the old Items.
inline bool CapturedIconsCurrent(const DragSourceList& source,
    const std::vector<DesktopWidget>& widgets)
{
    if (!source.hasWidgets || source.hasOriginWidget ||
        source.SourceSurfaceKind() != slot_contract::SlotSurfaceKind::Desktop)
        return false;
    bool captured = false;
    for (const auto& entry : source.entries)
    {
        if (!entry.desktopWidgetIcon) continue;
        captured = true;
        if (entry.kind != DropSourceKind::Widget || entry.fromDock || entry.widgetId.empty()) return false;
        const auto found = std::find_if(widgets.begin(), widgets.end(),
            [&](const auto& widget) { return widget.id == entry.widgetId; });
        if (found == widgets.end() ||
            std::count_if(widgets.begin(), widgets.end(),
                [&](const auto& widget) { return widget.id == entry.widgetId; }) != 1 ||
            entry.widgetType != found->type || !single_cell_widget::IsIconMode(*found) ||
            found->gridCell.pageId != entry.originalCell.pageId ||
            found->gridCell.column != entry.originalCell.column || found->gridCell.row != entry.originalCell.row ||
            found->gridSpan.columns != entry.originalSpan.columns || found->gridSpan.rows != entry.originalSpan.rows ||
            !widget_pair_drop::CanUseSource(widgets, static_cast<size_t>(found - widgets.begin())))
            return false;
    }
    return captured;
}

inline std::vector<PairChoice> PairChoices(DesktopWidgetType source,
    widget_pair_drop::Options options, bool groupFits)
{
    using Action = widget_pair_drop::Action;
    std::vector<PairChoice> choices;
    if (options.merge)
        choices.push_back({5, MK_SHIFT, Action::Merge,
            source == DesktopWidgetType::Collection
                ? "app.widget_drop.merge_collections" : "app.widget_drop.merge_desktop_files"});
    if (options.group != Action::None && groupFits)
        choices.push_back({6, MK_CONTROL, options.group,
            options.group == Action::CreateCollectionGroup
                ? "app.widget_drop.create_collection_group" : "app.widget_drop.create_file_group"});
    return choices;
}
}
