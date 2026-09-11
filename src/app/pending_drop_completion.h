#pragma once

#include "../core/drop_model.h"
#include "../shell_file_operation_worker.h"
#include "../pending_drop_rules.h"

namespace snowdesktop::pending_drop
{
using Queue = std::vector<PendingLandingCache>;

inline void Publish(Queue& queue, PendingLandingCache cache)
{
    if (cache.entries.empty() && cache.folderPlacements.empty()) return;
    cache.active = true;
    queue.push_back(std::move(cache));
}

// Completion reconciles only files actually produced by this request. A
// rejected/failed request has no outputs and cannot erase another completion.
inline void Complete(Queue& queue, PendingLandingCache cache,
    const ShellFileOperationResult& result)
{
    size_t omitted = 0;
    std::erase_if(cache.entries, [&](PendingLandingEntry& entry) {
        const auto output = std::find_if(result.outputs.begin(), result.outputs.end(),
            [&](const auto& item) { return _wcsicmp(item.source.c_str(), entry.sourcePath.c_str()) == 0; });
        if (output == result.outputs.end()) { ++omitted; return true; }
        if (entry.kind == DropLandingKind::WidgetIndex)
            entry.insertIndex -= std::min(entry.insertIndex, omitted);
        entry.createdPath = output->destination;
        return false;
    });
    for (auto& placement : cache.folderPlacements)
        for (const auto& output : result.outputs)
            placement.createdPaths.push_back(output.destination);
    std::erase_if(cache.folderPlacements, [](const auto& placement) { return placement.createdPaths.empty(); });
    Publish(queue, std::move(cache));
}

inline bool MatchesExactPath(const std::wstring& candidate, const std::wstring& expected)
{
    return !expected.empty() && _wcsicmp(candidate.c_str(), expected.c_str()) == 0;
}
// Model commit used after the host has checked the target's acceptance policy.
// Resolve by stable id, remove provisional auto-collection owners, then insert
// at the captured boundary. Folder-mapping keys belong to another namespace.
inline bool CommitKeyedLanding(std::vector<DesktopWidget>& widgets, DesktopItem& item,
    const PendingLandingEntry& landing, const std::wstring& key)
{
    const auto target = std::find_if(widgets.begin(), widgets.end(),
        [&](const auto& widget) { return widget.id == landing.widgetId; });
    if (target == widgets.end() || target->type == DesktopWidgetType::FolderMapping) return false;
    if (key.empty()) return false;
    for (auto& widget : widgets)
        if (widget.type != DesktopWidgetType::FolderMapping)
            std::erase_if(widget.itemKeys, [&](const auto& existing) { return _wcsicmp(existing.c_str(), key.c_str()) == 0; });
    pending_drop_rules::InsertAt(target->itemKeys, landing.insertIndex, std::vector<std::wstring>{key});
    item.gridCell = target->gridCell;
    return true;
}

}
