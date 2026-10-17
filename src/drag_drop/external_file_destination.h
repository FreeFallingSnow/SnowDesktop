#pragma once

#include "core/drop_model.h"
#include "shell/shell_file_operation_worker.h"

#include <functional>
#include <memory>

namespace snowdesktop::external_file_destination
{
// Capture values while hovering; rebind the destination only after the source
// has finished reading. No container or widget pointers cross that interval.
struct Destination
{
    DropPreviewList preview;
    std::wstring widgetId;
    DesktopWidgetType widgetType = DesktopWidgetType::Collection;
    std::wstring folderPath;
    bool dockFolderPopup = false;
};

using Completion = std::function<void(bool)>;
struct Services
{
    std::function<DesktopWidget*(const std::wstring&, bool)> resolveTarget;
    std::function<DropPreviewList(const GridCell&, size_t)> desktopPreview;
    std::function<bool(const std::vector<std::wstring>&)> canPinExisting;
    std::function<bool(const std::vector<std::wstring>&, size_t)> pinExisting;
    std::function<bool(size_t)> dockHasCapacity;
    std::function<void()> afterPin;
    std::function<std::wstring(const std::wstring&)> fileName;
    std::function<bool(const DragSourceList&, const DropPreviewList&, Completion,
        bool, std::shared_ptr<ShellFileOperationResult>)> execute;
};

// The host's destination also carries Lua-specific fields. Those are handled
// before this normal-file commit boundary; both host and native tests use this
// exact rebinding, batch expansion and ownership policy.
template<class CapturedDestination>
bool Commit(const CapturedDestination& destination,
    const std::vector<std::wstring>& paths, bool owned, bool copyOnly,
    Completion completion, bool synchronously,
    std::shared_ptr<ShellFileOperationResult> result, const Services& services)
{
    if (paths.empty()) return false;
    DropPreviewList preview = destination.preview;
    if (!destination.widgetId.empty() || destination.dockFolderPopup)
    {
        auto* widget = services.resolveTarget(destination.widgetId, destination.dockFolderPopup);
        if (!widget || widget->id != destination.widgetId ||
            widget->type != destination.widgetType ||
            widget->sourceFolderPath != destination.folderPath) return false;
        preview.targetWidget = widget;
        preview.landings.clear();
        for (size_t index = 0; index < paths.size(); ++index)
        {
            DropLanding landing;
            landing.kind = preview.targetKind == DropTargetKind::FolderMapping
                ? DropLandingKind::Folder : DropLandingKind::WidgetIndex;
            landing.sourceIndex = index;
            landing.widget = widget;
            landing.widgetId = widget->id;
            landing.insertIndex = preview.insertIndex + index;
            landing.cell = widget->gridCell;
            preview.landings.push_back(std::move(landing));
        }
    }
    else if (preview.pinMaterializedItemsToDock)
    {
        // Existing desktop files retain their Shell identity when pinned.
        if (!owned && services.canPinExisting(paths))
        {
            if (!services.pinExisting(paths, preview.dockInsertIndex)) return false;
            services.afterPin();
            if (completion) completion(true);
            return true;
        }
        if (!services.dockHasCapacity(paths.size())) return false;
        const auto action = preview.action;
        const auto insertIndex = preview.dockInsertIndex;
        preview = services.desktopPreview(preview.anchorCell, paths.size());
        preview.pinMaterializedItemsToDock = true;
        preview.dockInsertIndex = insertIndex;
        preview.action = action;
    }
    else if (preview.targetKind == DropTargetKind::Desktop)
    {
        const auto action = preview.action;
        preview = services.desktopPreview(preview.anchorCell, paths.size());
        preview.action = action;
    }
    else return false;

    DragSourceList sources;
    sources.hasExternalFiles = true;
    for (const auto& path : paths)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sources.entries.size();
        entry.filePath = path;
        entry.displayName = services.fileName(path);
        sources.entries.push_back(std::move(entry));
    }
    if (owned || (copyOnly && !preview.pinMaterializedItemsToDock))
        preview.action = DropAction::Copy;
    preview.fileBacked = true;
    return services.execute(sources, preview, std::move(completion), synchronously, std::move(result));
}
}
