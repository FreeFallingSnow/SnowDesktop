#pragma once

#include "item.h"
#include "widget.h"
#include "types.h"
#include "utils.h"

#include <string>
#include <unordered_set>
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

enum class DropSourceKind
{
    DesktopIcon,
    FolderEntry,
    ExternalFile,
    Widget,
    Unknown,
};

enum class DropTargetKind
{
    Desktop,
    KeyedWidget,
    FolderMapping,
    External,
    Handoff,
    Unknown,
};

enum class DropLandingKind
{
    None,
    DesktopCell,
    WidgetIndex,
    Folder,
    External,
};

struct DragSourceEntry
{
    DropSourceKind kind = DropSourceKind::Unknown;
    Item* item = nullptr;
    size_t sourceIndex = 0;
    size_t desktopIndex = static_cast<size_t>(-1);
    size_t memberIndex = static_cast<size_t>(-1);
    std::wstring desktopKey;
    std::wstring filePath;
    std::wstring displayName;
    GridCell originalCell;
    GridSpan originalSpan;
    bool protectedDesktopIcon = false;
};

struct DragSourceList
{
    std::vector<DragSourceEntry> entries;
    Container* origin = nullptr;
    bool hasOriginWidget = false;
    std::wstring originWidgetId;
    DesktopWidgetType originWidgetType = DesktopWidgetType::Collection;
    bool hasDesktopIcons = false;
    bool hasFolderEntries = false;
    bool hasExternalFiles = false;
    bool hasWidgets = false;

    bool Empty() const { return entries.empty(); }

    std::vector<std::wstring> FilePaths() const
    {
        std::vector<std::wstring> paths;
        for (const auto& entry : entries)
            if (!entry.filePath.empty())
                paths.push_back(entry.filePath);
        return paths;
    }

    std::vector<std::wstring> DesktopKeys() const
    {
        std::vector<std::wstring> keys;
        for (const auto& entry : entries)
            if (!entry.desktopKey.empty())
                keys.push_back(entry.desktopKey);
        return keys;
    }
};

struct DropLanding
{
    DropLandingKind kind = DropLandingKind::None;
    size_t sourceIndex = 0;
    GridCell cell;
    size_t insertIndex = 0;
    DesktopWidget* widget = nullptr;
    std::wstring widgetId;
};

struct DropPreviewList
{
    DropTargetKind targetKind = DropTargetKind::Unknown;
    DropAction action = DropAction::Move;
    Container* targetContainer = nullptr;
    DesktopWidget* targetWidget = nullptr;
    GridCell anchorCell;
    size_t insertIndex = 0;
    bool fileBacked = false;
    std::vector<DropLanding> landings;

    bool Empty() const { return landings.empty(); }
};

struct PendingLandingEntry
{
    size_t sourceIndex = 0;
    DropAction action = DropAction::Move;
    DropLandingKind kind = DropLandingKind::None;
    std::wstring sourcePath;
    std::wstring sourceName;
    GridCell cell;
    size_t insertIndex = 0;
    DesktopWidget* widget = nullptr;
    std::wstring widgetId;
};

struct PendingLandingCache
{
    std::vector<PendingLandingEntry> entries;
    std::unordered_set<std::wstring> existingDesktopKeys;
    bool active = false;
    DWORD tick = 0;

    void Clear()
    {
        entries.clear();
        existingDesktopKeys.clear();
        active = false;
        tick = 0;
    }
};
