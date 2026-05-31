#pragma once

#include "item.h"
#include "widget.h"
#include "types.h"
#include "utils.h"

#include <string>
#include <vector>

enum class DropAction
{
    Move,
    Copy,
    Link,
};

inline DropAction DropActionFromMods(int mods, DropAction defaultAction = DropAction::Move)
{
    if (mods & MK_ALT) return DropAction::Link;
    if (mods & MK_CONTROL) return DropAction::Copy;
    if (mods & MK_SHIFT) return DropAction::Move;
    return defaultAction;
}

inline int DropActionToMods(DropAction action)
{
    switch (action)
    {
    case DropAction::Copy: return MK_CONTROL;
    case DropAction::Link: return MK_ALT;
    case DropAction::Move:
    default:
        return 0;
    }
}

struct DropPayload
{
    std::vector<Item*> items;
    std::vector<std::wstring> filePaths;
    std::vector<std::wstring> desktopKeys;
    bool hasWidgets = false;
    bool hasDesktopIcons = false;
    bool hasFolderEntries = false;
    bool hasExternalFiles = false;

    static DropPayload From(const std::vector<Item*>& sourceItems)
    {
        DropPayload payload;
        payload.items = sourceItems;
        for (auto* src : sourceItems)
        {
            if (!src) continue;

            if (dynamic_cast<Widget*>(src))
            {
                payload.hasWidgets = true;
                continue;
            }

            if (auto* icon = dynamic_cast<DesktopIcon*>(src))
            {
                payload.hasDesktopIcons = true;
                DesktopItem* item = icon->GetDesktopItem();
                if (item && !item->layoutKey.empty())
                    payload.desktopKeys.push_back(ToUpperInvariant(item->layoutKey));

                std::wstring path = icon->GetPath();
                if (item && item->desktopIconClsid.empty() && !path.empty())
                    payload.filePaths.push_back(path);
                continue;
            }

            if (dynamic_cast<FolderEntryIcon*>(src))
                payload.hasFolderEntries = true;
            else if (dynamic_cast<ExternalFileItem*>(src))
                payload.hasExternalFiles = true;

            std::wstring path = src->GetPath();
            if (!path.empty())
                payload.filePaths.push_back(path);
        }
        return payload;
    }
};
