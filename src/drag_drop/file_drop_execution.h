#pragma once

#include "pending_drop_completion.h"
#include <atomic>
#include <functional>
#include <memory>

namespace snowdesktop::file_drop_execution
{
using Completion = std::function<void(bool)>;
struct Services
{
    std::function<std::unordered_set<std::wstring>()> snapshotDesktopKeys;
    std::function<PendingFolderPlacement(const DesktopWidget&, size_t, const DragSourceList*)> folderPlacement;
    std::function<std::wstring(const std::wstring&)> fileName;
    std::function<bool(const std::wstring&, const std::wstring&)> pathsEqual;
    std::function<bool(const DragSourceList&, DropAction, bool, Completion, bool,
        std::shared_ptr<ShellFileOperationResult>)> desktop;
    std::function<bool(const DragSourceList&, const std::wstring&, DropAction, Completion,
        bool, std::shared_ptr<ShellFileOperationResult>)> folder;
    std::function<bool(const std::vector<std::wstring>&, size_t)> pin;
    std::function<void()> savePins;
    std::function<void(const std::vector<std::wstring>&)> forgetMovedKeys;
    std::function<void()> refresh;
};

// One production transaction owns the captured placement, actual file outputs,
// source membership cleanup and destination completion. UI services are copied
// into the callback; no references to a temporary preview survive scheduling.
inline bool Execute(const DragSourceList& sources, const DropPreviewList& preview,
    pending_drop::Queue& placements, Services services, Completion completion,
    bool synchronously, std::shared_ptr<ShellFileOperationResult> result)
{
    if (!result) result = std::make_shared<ShellFileOperationResult>();
    const bool folderTarget = preview.targetKind == DropTargetKind::FolderMapping && preview.targetWidget;
    PendingLandingCache cache = folderTarget ? PendingLandingCache{} :
        pending_drop::BuildLanding(sources, preview, services.snapshotDesktopKeys(), GetTickCount(), services.fileName);
    if (folderTarget)
        cache.folderPlacements.push_back(services.folderPlacement(*preview.targetWidget, preview.insertIndex, &sources));

    auto completed = std::make_shared<std::atomic_bool>(false);
    auto committed = std::make_shared<bool>(false);
    auto operationCompletion = [&placements, services, result, completed, committed, cache = std::move(cache),
        entries = sources.entries, action = preview.action,
        pinToDock = preview.pinMaterializedItemsToDock, dockInsertIndex = preview.dockInsertIndex,
        completion = std::move(completion)](bool succeeded) mutable {
        if (completed->exchange(true)) return;
        if (!result->outputs.empty())
        {
            if (pinToDock)
            {
                std::vector<std::wstring> paths;
                for (const auto& source : entries)
                    for (const auto& output : result->outputs)
                        if (services.pathsEqual(source.filePath, output.source)) paths.push_back(output.destination);
                if (services.pin(paths, dockInsertIndex)) services.savePins();
                else succeeded = false;
            }
            else
                pending_drop::Complete(placements, std::move(cache), *result);
            if (action == DropAction::Move)
            {
                std::vector<std::wstring> movedKeys;
                for (const auto& source : entries)
                    for (const auto& output : result->outputs)
                        if (services.pathsEqual(source.filePath, output.source) && !source.desktopKey.empty())
                            movedKeys.push_back(source.desktopKey);
                services.forgetMovedKeys(movedKeys);
            }
            services.refresh();
        }
        *committed = succeeded;
        if (completion) completion(succeeded);
    };
    const bool duplicateNames = preview.action == DropAction::Copy && sources.hasDesktopIcons && !sources.hasExternalFiles;
    const bool queued = folderTarget
        ? services.folder(sources, preview.targetWidget->sourceFolderPath, preview.action, operationCompletion, synchronously, result)
        : services.desktop(sources, preview.action, duplicateNames, operationCompletion, synchronously, result);
    if (!queued) operationCompletion(false);
    return queued && (!synchronously || *committed);
}
}
