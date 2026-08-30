#pragma once

#include "core/drop_model.h"
#include "item_location.h"

#include <windows.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::dock_drop_rules
{

// External resources pinned to Dock are represented by a shortcut on the
// managed desktop. The source file must never be moved into that directory.
inline DropAction ExternalMappingAction() noexcept
{
    return DropAction::Link;
}

// Prefer the native link cursor. Some drag sources expose only copy/move; copy
// is still safe because SnowDesktop creates its own link and the source must
// retain the original. Move-only sources are rejected.
inline DWORD ChooseExternalMappingEffect(DWORD allowed) noexcept
{
    const DWORD available =
        allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (available & DROPEFFECT_LINK)
        return DROPEFFECT_LINK;
    if (available & DROPEFFECT_COPY)
        return DROPEFFECT_COPY;
    return DROPEFFECT_NONE;
}

// Items with a dedicated Dock position, such as Recycle Bin, ignore sortable
// insertion indices and therefore must not show the fixed-area insertion bar.
inline bool ShouldDrawSortableInsertionIndicator(
    bool fixedPlacementSource) noexcept
{
    return !fixedPlacementSource;
}

// Dock controls and generated running/frequent items are not sortable slots.
// Partition their pointer area by the midpoints of the current sortable group
// so every otherwise inert surface resolves to a real insertion boundary.
template <typename MidpointAt>
inline std::size_t ResolveRedirectedInsertionIndex(
    long pointerAxis,
    std::size_t beginIndex,
    std::size_t endIndex,
    const MidpointAt& midpointAt)
{
    for (std::size_t index = beginIndex;
         index < endIndex; ++index)
    {
        if (pointerAxis < midpointAt(index))
            return index;
    }
    return endIndex;
}

inline bool IsFolderSourceTarget(
    snowdesktop::item_location::FolderTargetKind kind) noexcept
{
    return kind !=
        snowdesktop::item_location::FolderTargetKind::None;
}

inline std::vector<std::wstring> OrderedMaterializedPaths(
    const std::vector<size_t>& sourceOrder,
    const std::unordered_map<size_t, std::wstring>& pathsBySource)
{
    std::vector<std::wstring> paths;
    paths.reserve(sourceOrder.size());
    for (const size_t sourceIndex : sourceOrder)
    {
        const auto found = pathsBySource.find(sourceIndex);
        if (found != pathsBySource.end() && !found->second.empty())
            paths.push_back(found->second);
    }
    return paths;
}

// Desktop shortcut creation is asynchronous. Keep names selected by queued
// requests unavailable to later requests until their completion callback runs,
// otherwise two same-name drops can both choose the same not-yet-created path.
class MaterializedPathReservations
{
public:
    bool TryReserve(const std::wstring& path)
    {
        return !path.empty() &&
            paths_.insert(Normalize(path)).second;
    }

    bool Contains(const std::wstring& path) const
    {
        return !path.empty() &&
            paths_.contains(Normalize(path));
    }

    void Release(const std::vector<std::wstring>& paths)
    {
        for (const auto& path : paths)
            paths_.erase(Normalize(path));
    }

private:
    static std::wstring Normalize(const std::wstring& path)
    {
        std::wstring normalized = path;
        if (!normalized.empty())
        {
            CharUpperBuffW(
                normalized.data(),
                static_cast<DWORD>(normalized.size()));
        }
        return normalized;
    }

    std::unordered_set<std::wstring> paths_;
};

// Reordering owns the icon center only while a Dock drag stays inside the
// same visual group. Crossing between main and folder entries must leave the
// center available for the target item's handoff action.
inline bool ShouldPreferMetadataReorder(
    bool sourceIsDock,
    bool sourceFoldersOnly,
    bool targetIsFolder) noexcept
{
    return sourceIsDock &&
        sourceFoldersOnly == targetIsFolder;
}

// Widget entries are layout objects rather than file payloads. A folder
// mapping can still hand its resolved path to an ordinary Shell target, but
// neither a filesystem folder nor a Collection can consume the widget object
// through their current drop pipelines.
inline bool SupportsHandoffTarget(
    bool sourceHasWidgets,
    bool targetIsFolder,
    bool targetIsCollection) noexcept
{
    return !sourceHasWidgets ||
        (!targetIsFolder && !targetIsCollection);
}

// A Collection popup uses the same item-placement pipeline as its Collection.
// Widget and grouped-entry payloads are not executable there, so they must not
// arm the dwell opener or expose insertion slots inside an already-open popup.
// External OLE drags have no DragSourceList entries until drop materialization,
// but remain valid Collection payloads while their controller session is active.
inline bool CanUseCollectionPopup(
    bool dragActive,
    bool externalDragActive,
    bool sourceEmpty,
    bool sourceHasWidgets,
    bool sourceHasCollectionGroupEntries,
    bool sourceHasFileGroupEntries) noexcept
{
    if (!dragActive || sourceHasWidgets ||
        sourceHasCollectionGroupEntries ||
        sourceHasFileGroupEntries)
        return false;
    return externalDragActive || !sourceEmpty;
}

// Resetting an already-empty dwell state on every pointer sample only repeats
// a User32 timer lookup. The index is the primary state: every arming path sets
// it before SetTimer, and only the reset path restores the sentinel.
inline bool IsDockHandoffDwellIdle(
    size_t index, DWORD startTick, bool ready) noexcept
{
    return index == static_cast<size_t>(-1) &&
        startTick == 0 &&
        !ready;
}

} // namespace snowdesktop::dock_drop_rules
