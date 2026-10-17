#pragma once

#include "core/drop_model.h"
#include "dock/dock_drop_rules.h"
#include "widgets/widget_pair_drop.h"

#include <functional>
#include <vector>

namespace snowdesktop::right_drag_actions
{
struct Choice
{
    UINT command;
    DWORD effect;
    DWORD modifiers;
    const char* label;
    widget_pair_drop::Action pairAction = widget_pair_drop::Action::None;
};

// These are observations captured by the host before opening its shared menu.
// The menu itself remains a UI service; action admission and selection are the
// same production path exercised by the native filesystem regressions.
struct Context
{
    DWORD allowedEffects = 0;
    bool handleDrag = false, nativeMove = false, itemTarget = false;
    bool luaFileDrop = false, dockRemoval = false, externalSource = false;
    bool targetDock = false, sourceDock = false, displayDockTarget = false;
    bool sourceWidgets = false, sourceDesktopIcons = false, sourceFolderEntries = false;
    bool desktopWidgetIcons = false, handoff = false, sourceHasPaths = false;
    size_t externalFileCount = 0;
    std::function<DropPreviewList(DWORD)> preview;
};

inline bool Accepts(const Context& context, const Choice& choice)
{
    if ((context.allowedEffects & choice.effect) == 0) return false;
    if (choice.pairAction != widget_pair_drop::Action::None) return true;
    if (context.handleDrag) return context.nativeMove && choice.effect == DROPEFFECT_MOVE;
    if (!context.itemTarget) return false;
    if (context.luaFileDrop) return choice.effect == DROPEFFECT_COPY;
    if (context.dockRemoval) return choice.effect == DROPEFFECT_MOVE;
    if (context.targetDock)
    {
        if (context.externalSource)
            return choice.effect == dock_drop_rules::ChooseExternalMappingEffect(context.allowedEffects);
        if (context.sourceDock) return choice.effect == DROPEFFECT_MOVE;
        if (context.sourceWidgets && !context.sourceDesktopIcons && !context.sourceFolderEntries)
            return choice.effect == DROPEFFECT_MOVE;
        if (context.sourceFolderEntries) return choice.effect == DROPEFFECT_LINK;
        return choice.effect == DROPEFFECT_MOVE || choice.effect == DROPEFFECT_COPY;
    }
    if (context.sourceWidgets)
    {
        if (choice.effect == DROPEFFECT_MOVE) return true;
        if (!context.desktopWidgetIcons) return false;
        const auto preview = context.preview(choice.modifiers);
        return !preview.Empty() && preview.action == DropActionFromMods(static_cast<int>(choice.modifiers));
    }
    if (context.externalSource) return context.externalFileCount > 0 || choice.effect == DROPEFFECT_COPY;
    if (context.handoff) return context.sourceHasPaths;
    const auto preview = context.preview(choice.modifiers);
    const auto action = DropActionFromMods(static_cast<int>(choice.modifiers));
    return !preview.Empty() && (preview.action == action ||
        (action == DropAction::Move && preview.consumeDockSource));
}

template<class PairChoices>
std::vector<Choice> Build(const Context& context, const PairChoices& pairs)
{
    const bool hasWidgets = context.handleDrag || context.sourceWidgets;
    std::vector<Choice> choices{
        {1, DROPEFFECT_MOVE, hasWidgets ? DWORD{0} : DWORD{MK_SHIFT}, "widget.base.move"},
        {2, DROPEFFECT_COPY, MK_CONTROL, "widget.base.copy_label"},
        {3, DROPEFFECT_LINK, MK_ALT, "widget.base.create_shortcut"},
    };
    for (const auto& pair : pairs)
        choices.push_back({pair.command, DROPEFFECT_MOVE, pair.modifiers, pair.label, pair.action});
    std::erase_if(choices, [&](const auto& choice) { return !Accepts(context, choice); });
    for (auto& choice : choices)
    {
        if (context.externalSource && context.displayDockTarget)
            choice.label = "widget.base.create_shortcut";
        else if (context.displayDockTarget && !context.luaFileDrop && !context.handoff &&
            choice.effect == DROPEFFECT_COPY)
            choice.label = "app.dock.create_mapping_action";
    }
    return choices;
}

inline DWORD SelectedKeys(const Choice& choice) noexcept
{
    // A chosen verb uses a left-button handoff so Shell cannot open a second
    // right-drag menu for an operation the user has already selected.
    return MK_LBUTTON | choice.modifiers;
}

inline const Choice* Select(const std::vector<Choice>& choices, UINT command) noexcept
{
    const auto found = std::find_if(choices.begin(), choices.end(),
        [&](const auto& choice) { return choice.command == command; });
    return found == choices.end() ? nullptr : &*found;
}
}
