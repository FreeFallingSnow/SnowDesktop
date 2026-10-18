#pragma once

#include "common/constants.h"
#include "common/types.h"
#include "desktop/desktop_membership_recovery.h"
#include "dock/dock_folder_rules.h"
#include "shell/shell_item_visibility.h"
#include "widgets/collection_group_rules.h"
#include <algorithm>
#include <functional>
#include <unordered_set>

namespace snowdesktop::dock_drop_model
{
struct State
{
    std::vector<DockEntry>& entries;
    std::vector<DesktopWidget>& widgets;
};

// Shell classification, the host's item identity cache and live capacity are
// external to the model transaction. Both host entry points use this commit.
struct Services
{
    std::function<std::wstring(const std::wstring&)> normalize;
    std::function<bool(const DockEntry&)> recycle;
    std::function<bool(const DockEntry&)> folder;
    std::function<bool(size_t)> capacity;
    std::function<DesktopItem*(const std::wstring&)> item;
};

inline void Normalize(std::vector<DockEntry>& entries, const Services& services)
{
    dock_folder_rules::NormalizeKeepingUserOrder(entries, [&](const auto& entry) {
        if (services.recycle(entry)) return dock_folder_rules::EntryGroup::Recycle;
        return services.folder(entry) ? dock_folder_rules::EntryGroup::Folder : dock_folder_rules::EntryGroup::Main;
    });
}

inline void MakeExclusive(State state, const DockEntry& entry, const Services& services)
{
    if (entry.type == DockEntryType::DesktopItem)
    {
        for (auto& widget : state.widgets)
            desktop_membership_recovery::Forget(widget, {entry.reference});
        if (auto* item = services.item(entry.reference)) item->gridCell = {kDockPageId, 0, 0};
        return;
    }
    const auto child = std::find_if(state.widgets.begin(), state.widgets.end(),
        [&](const auto& widget) { return widget.id == entry.reference; });
    if (child == state.widgets.end()) return;
    if (entry.type == DockEntryType::Collection || entry.type == DockEntryType::FolderMapping ||
        entry.type == DockEntryType::DesktopFiles)
    {
        for (auto& group : state.widgets)
        {
            if (group.type != (entry.type == DockEntryType::Collection
                    ? DesktopWidgetType::CollectionGroup : DesktopWidgetType::FileGroup)) continue;
            std::erase(group.childWidgetIds, entry.reference);
            group.activeCategoryId = collection_group_rules::ResolveActiveItem(group.childWidgetIds, group.activeCategoryId);
        }
    }
    child->gridCell = {kDockPageId, 0, 0};
}

// The source decoder has already resolved desktop/widget identities. File
// materialization uses AddMaterialized instead, preserving its distinct policy.
inline bool Pin(State state, const std::vector<DockEntry>& additions, size_t insertIndex, const Services& services)
{
    if (additions.empty()) return false;
    const auto matches = [&](const auto& current, const auto& addition) {
        return current.type == addition.type && services.normalize(current.reference) == services.normalize(addition.reference);
    };
    size_t genuinelyNew = 0;
    for (const auto& addition : additions)
        if (std::none_of(state.entries.begin(), state.entries.end(), [&](const auto& entry) { return matches(entry, addition); }))
            ++genuinelyNew;
    if (!services.capacity(genuinelyNew)) return false;

    for (const auto& addition : additions)
    {
        auto existing = std::find_if(state.entries.begin(), state.entries.end(), [&](const auto& entry) { return matches(entry, addition); });
        if (existing != state.entries.end())
        {
            const bool becomingExclusive = existing->keepOnDesktop && !addition.keepOnDesktop;
            existing->keepOnDesktop = addition.keepOnDesktop;
            if (IsWidgetDockEntryType(addition.type))
            {
                existing->keepOnDesktop = false;
                MakeExclusive(state, addition, services);
            }
            else if (becomingExclusive) MakeExclusive(state, addition, services);
            continue;
        }
        const auto recycle = std::find_if(state.entries.begin(), state.entries.end(), services.recycle);
        const auto sortableEnd = static_cast<size_t>(std::distance(state.entries.begin(), recycle));
        insertIndex = services.recycle(addition) ? state.entries.size() : std::min(insertIndex, sortableEnd);
        state.entries.insert(state.entries.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
        if (!services.recycle(addition)) ++insertIndex;
        if (!(addition.keepOnDesktop && addition.type == DockEntryType::DesktopItem)) MakeExclusive(state, addition, services);
    }
    Normalize(state.entries, services);
    return true;
}

inline bool AddMaterialized(State state, const std::vector<std::wstring>& paths,
    size_t insertIndex, bool keepOnDesktop, const Services& services)
{
    std::unordered_set<std::wstring> newKeys;
    for (const auto& path : paths)
    {
        const auto key = services.normalize(path);
        if (key.empty() || shell_item_visibility::IsAlwaysHidden(key)) continue;
        if (std::none_of(state.entries.begin(), state.entries.end(), [&](const auto& entry) {
                return entry.type == DockEntryType::DesktopItem && services.normalize(entry.reference) == key;
            })) newKeys.insert(key);
    }
    if (!services.capacity(newKeys.size())) return false;
    bool changed = false;
    for (const auto& path : paths)
    {
        const auto key = services.normalize(path);
        if (key.empty() || shell_item_visibility::IsAlwaysHidden(key)) continue;
        auto existing = std::find_if(state.entries.begin(), state.entries.end(), [&](const auto& entry) {
            return entry.type == DockEntryType::DesktopItem && services.normalize(entry.reference) == key;
        });
        if (existing != state.entries.end())
        {
            if (keepOnDesktop && !existing->keepOnDesktop)
            {
                existing->keepOnDesktop = true;
                if (auto* item = services.item(key); item && item->gridCell.pageId == kDockPageId) item->gridCell = {};
                changed = true;
            }
            continue;
        }
        const auto recycle = std::find_if(state.entries.begin(), state.entries.end(), services.recycle);
        insertIndex = std::min(insertIndex, static_cast<size_t>(std::distance(state.entries.begin(), recycle)));
        state.entries.insert(state.entries.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            DockEntry{DockEntryType::DesktopItem, key, keepOnDesktop});
        ++insertIndex;
        changed = true;
    }
    if (!changed) return keepOnDesktop && !paths.empty();
    Normalize(state.entries, services);
    return true;
}

enum class ReorderResult { NoChange, MixedSections, Changed };

inline ReorderResult Reorder(std::vector<DockEntry>& entries, std::vector<size_t> indices,
    size_t insertIndex, const Services& services, bool preserveCapturedOrder = false)
{
    std::erase_if(indices, [&](size_t index) { return index >= entries.size() || services.recycle(entries[index]); });
    if (!preserveCapturedOrder) std::sort(indices.begin(), indices.end());
    std::unordered_set<size_t> seen;
    std::erase_if(indices, [&](size_t index) { return !seen.insert(index).second; });
    if (indices.empty()) return ReorderResult::NoChange;
    std::vector<DockEntry> movingMain, movingFolders;
    for (size_t index : indices)
        (services.folder(entries[index]) ? movingFolders : movingMain).push_back(entries[index]);
    std::sort(indices.begin(), indices.end());
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it < insertIndex) --insertIndex;
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    const size_t mainEnd = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return !services.recycle(entry) && !services.folder(entry); }));
    const size_t folderEnd = mainEnd + static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return !services.recycle(entry) && services.folder(entry); }));
    // Insert the later partition first: the main insertion then shifts the
    // complete file section while preserving each partition's captured order.
    const auto mainInsert = std::min(insertIndex, mainEnd);
    const auto folderInsert = std::clamp(insertIndex, mainEnd, folderEnd);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(folderInsert), movingFolders.begin(), movingFolders.end());
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(mainInsert), movingMain.begin(), movingMain.end());
    dock_folder_rules::CaptureUserOrder(entries);
    Normalize(entries, services);
    return ReorderResult::Changed;
}

inline bool RemoveMappings(std::vector<DockEntry>& entries, std::vector<size_t> indices, const Services& services)
{
    std::erase_if(indices, [&](size_t index) { return index >= entries.size() || !entries[index].keepOnDesktop; });
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty()) return false;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*it));
    Normalize(entries, services);
    return true;
}
}
