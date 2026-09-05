#pragma once

#include "../desktop_item_reference_migration.h"
#include "../folder_sort_rules.h"
#include "../shell_file_operation_worker.h"

#include <filesystem>

namespace snowdesktop::rename_model_update
{
struct Changes
{
    std::vector<size_t> desktopItems;
    std::vector<std::wstring> folders;
    bool popup = false;
    bool needsReload = false;
};

// Apply metadata by stable path, preserving selection, position, existing
// bitmaps and unrelated items. No filesystem/Shell queries belong here.
inline Changes Apply(const ShellRenameResult& result, const std::wstring& newLayoutKey,
    std::vector<DesktopItem>& items, std::vector<DesktopWidget>& widgets,
    std::vector<DockEntry>& dockEntries, DesktopWidget* popup)
{
    Changes changes;
    if (FAILED(result.status))
        return changes;
    if (result.path.empty())
    {
        changes.needsReload = true;
        return changes;
    }
    namespace migration = desktop_item_reference_migration;
    migration::MigrateReferences(widgets, dockEntries, result.sourcePath, result.path);
    if (popup)
        for (auto& key : popup->itemKeys)
            migration::ReplaceMatchingKey(key, result.sourcePath, result.path);
    changes.needsReload = !result.metadataComplete || result.absoluteId.empty();
    const bool directory =
        (result.attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const std::optional<std::uint64_t> size = directory
        ? std::nullopt : std::optional<std::uint64_t>(
            (static_cast<std::uint64_t>(result.attributes.nFileSizeHigh) << 32) |
            result.attributes.nFileSizeLow);
    const auto* absolute = reinterpret_cast<PCIDLIST_ABSOLUTE>(result.absoluteId.data());
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto& item = items[i];
        if (!item.desktopIconClsid.empty() ||
            !migration::KeysEqual(item.parsingName, result.sourcePath))
            continue;
        item.layoutKey = newLayoutKey;
        if (changes.needsReload || result.desktopChildId.empty())
        {
            // A mapped-folder alias can also be a desktop item. Its desktop
            // namespace child must be rediscovered rather than guessed.
            changes.needsReload = true;
            continue;
        }
        Pidl absoluteCopy(ILCloneFull(absolute));
        Pidl childCopy(ILCloneFull(reinterpret_cast<PCIDLIST_ABSOLUTE>(
            result.desktopChildId.data())));
        if (!absoluteCopy.get() || !childCopy.get())
        {
            changes.needsReload = true;
            continue;
        }
        item.absolutePidl = std::move(absoluteCopy);
        item.childPidl = std::move(childCopy);
        item.parsingName = result.path;
        item.name = result.displayName;
        item.typeName = result.typeName;
        item.sysIconIndex = result.sysIconIndex;
        item.modifiedTime = result.attributes.ftLastWriteTime;
        item.fileSize = size;
        changes.desktopItems.push_back(i);
    }
    if (changes.needsReload)
        return changes;
    const std::wstring fileName = std::filesystem::path(result.path).filename().wstring();
    const auto updateFolder = [&](DesktopWidget& widget) {
        bool changed = false;
        for (auto& entry : widget.folderEntries)
        {
            if (!migration::KeysEqual(entry.fullPath, result.sourcePath))
                continue;
            entry.fullPath = result.path;
            entry.name = fileName;
            entry.typeName = result.typeName;
            entry.sysIconIndex = result.sysIconIndex;
            entry.isDirectory = directory;
            entry.lastWriteTime = result.attributes.ftLastWriteTime;
            entry.fileSize = size;
            changed = true;
        }
        if (changed)
        {
            folder_sort_rules::StableSort(widget.folderEntries,
                widget.folderSortMode, widget.folderSortAscending);
            folder_sort_rules::RewriteOrderKeys(widget.folderEntries, widget.itemKeys);
        }
        return changed;
    };
    for (auto& widget : widgets)
        if (widget.type == DesktopWidgetType::FolderMapping && updateFolder(widget))
            changes.folders.push_back(widget.id);
    if (popup)
        changes.popup = updateFolder(*popup);
    return changes;
}
}
