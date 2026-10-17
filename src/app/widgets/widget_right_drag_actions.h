#pragma once

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
