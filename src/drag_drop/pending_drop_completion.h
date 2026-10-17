#pragma once

#include "core/drop_model.h"
#include "shell/shell_file_operation_worker.h"
#include "drag_drop/pending_drop_rules.h"
#include "desktop/desktop_membership_recovery.h"

#include <unordered_map>

namespace snowdesktop::pending_drop
{
using Queue = std::vector<PendingLandingCache>;

template<class FileName>
PendingLandingCache BuildLanding(const DragSourceList& sources, const DropPreviewList& preview,
    const std::unordered_set<std::wstring>& existingKeys, DWORD tick, FileName fileName,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource = nullptr)
{
    PendingLandingCache cache;
    cache.existingDesktopKeys = existingKeys;
    cache.tick = tick;
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell && landing.kind != DropLandingKind::WidgetIndex)
            continue;
        const auto source = std::find_if(sources.entries.begin(), sources.entries.end(),
            [&](const auto& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (source == sources.entries.end()) continue;
        PendingLandingEntry entry;
        entry.sourceIndex = source->sourceIndex;
        entry.action = preview.action;
        entry.kind = landing.kind;
        entry.sourcePath = source->filePath;
        entry.sourceName = source->filePath.empty() ? source->displayName : fileName(source->filePath);
        if (createdPathsBySource)
        {
            const auto created = createdPathsBySource->find(source->sourceIndex);
            if (created != createdPathsBySource->end()) entry.createdPath = created->second;
        }
        entry.cell = landing.cell;
        entry.insertIndex = landing.insertIndex;
        entry.widgetId = landing.widgetId;
        cache.entries.push_back(std::move(entry));
    }
    cache.active = !cache.entries.empty();
    return cache;
}

template<class Normalize, class FileName>
PendingFolderPlacement BuildFolder(const DesktopWidget& widget, size_t insertIndex,
    const DragSourceList* sources, const std::wstring& widgetId, const std::wstring& popupSourceId,
    Normalize normalize, FileName fileName)
{
    PendingFolderPlacement placement;
    placement.sourceFolderPath = widget.sourceFolderPath;
    placement.insertIndex = insertIndex;
    placement.widgetId = widgetId;
    placement.popupSourceId = popupSourceId;
    for (const auto& entry : widget.folderEntries)
        if (!entry.fullPath.empty()) placement.existingPaths.insert(normalize(entry.fullPath));
    if (sources)
    {
        placement.sourceNames.reserve(sources->entries.size());
        for (const auto& entry : sources->entries)
        {
            placement.sourcePaths.push_back(entry.filePath);
            std::wstring name = entry.filePath.empty() ? entry.displayName : fileName(entry.filePath);
            if (!name.empty()) placement.sourceNames.push_back(std::move(name));
        }
    }
    return placement;
}

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
    {
        if (placement.sourcePaths.empty())
        {
            for (const auto& output : result.outputs)
                placement.createdPaths.push_back(output.destination);
        }
        else
            for (const auto& source : placement.sourcePaths)
                for (const auto& output : result.outputs)
                    if (_wcsicmp(source.c_str(), output.source.c_str()) == 0)
                        placement.createdPaths.push_back(output.destination);
    }
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
        desktop_membership_recovery::Forget(widget, {key});
    pending_drop_rules::InsertAt(target->itemKeys, landing.insertIndex, std::vector<std::wstring>{key});
    item.gridCell = target->gridCell;
    return true;
}

}
