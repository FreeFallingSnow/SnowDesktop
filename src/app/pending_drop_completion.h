#pragma once

#include "../core/drop_model.h"
#include "../shell_file_operation_worker.h"

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
    std::erase_if(cache.entries, [&](PendingLandingEntry& entry) {
        const auto output = std::find_if(result.outputs.begin(), result.outputs.end(),
            [&](const auto& item) { return _wcsicmp(item.source.c_str(), entry.sourcePath.c_str()) == 0; });
        if (output == result.outputs.end()) return true;
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
}
