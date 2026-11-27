#pragma once

#include "common/constants.h"
#include "common/types.h"
#include "desktop/desktop_membership_recovery.h"
#include "dock/dock_folder_rules.h"
#include "shell/shell_item_visibility.h"
#include "widgets/collection_group_rules.h"
#include <algorithm>
#include <functional>
#include <iterator>
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
    bool freeItemOrder = false;
    std::optional<DockEntrySection> dropSection;
};

inline dock_folder_rules::EntryGroup GroupOf(const DockEntry& entry, const Services& services)
{
    using Group = dock_folder_rules::EntryGroup;
    if (entry.type == DockEntryType::QuickSearch) return Group::Folder;
    const bool recycle = services.recycle(entry);
    if (entry.freeOrderSection && (services.freeItemOrder || entry.type == DockEntryType::Separator || recycle))
        return *entry.freeOrderSection == DockEntrySection::Files ? Group::Folder : Group::Main;
    return recycle || services.folder(entry) ? Group::Folder : Group::Main;
}

inline void Normalize(std::vector<DockEntry>& entries, const Services& services)
{
    dock_folder_rules::NormalizeKeepingUserOrder(entries, [&](const auto& entry) { return GroupOf(entry, services); });
}

inline dock_folder_rules::InsertRange SectionRange(const std::vector<DockEntry>& entries,
    DockEntrySection section, const Services& services)
{
    using Group = dock_folder_rules::EntryGroup;
    const size_t mainEnd = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return GroupOf(entry, services) == Group::Main; }));
    return section == DockEntrySection::Files ? dock_folder_rules::InsertRange{mainEnd, entries.size()} :
        dock_folder_rules::InsertRange{0, mainEnd};
}

inline void InsertInSection(std::vector<DockEntry>& entries, size_t index,
    dock_folder_rules::InsertRange range, DockEntry entry)
{
    dock_folder_rules::EnsureUserOrder(entries);
    index = std::clamp(index, range.begin, range.end);
    const size_t rank = index < range.end ? *entries[index].userOrder :
        index > range.begin ? *entries[index - 1].userOrder + 1 : 0;
    for (auto& existing : entries)
        if (*existing.userOrder >= rank) ++*existing.userOrder;
    entry.userOrder = rank;
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(index), std::move(entry));
}

inline void EnsureQuickSearch(std::vector<DockEntry>& entries, const Services& services)
{
    if (std::any_of(entries.begin(), entries.end(),
        [](const auto& entry) { return entry.type == DockEntryType::QuickSearch; })) return;

    auto range = SectionRange(entries, DockEntrySection::Files, services);
    const auto files = entries.begin() + static_cast<std::ptrdiff_t>(range.begin);
    const auto end = entries.begin() + static_cast<std::ptrdiff_t>(range.end);
    auto recycle = std::find_if(files, end, services.recycle);
    if (recycle != end && std::next(recycle) != end && !recycle->freeOrderSection)
    {
        // Old grouped layouts painted the default Recycle Bin at the end
        // regardless of its stored rank. Keep that placement when migrating.
        DockEntry entry = std::move(*recycle);
        entries.erase(recycle);
        --range.end;
        InsertInSection(entries, range.end, range, std::move(entry));
        recycle = entries.begin() + static_cast<std::ptrdiff_t>(range.end);
        ++range.end;
    }
    const size_t index = static_cast<size_t>(recycle - entries.begin());
    InsertInSection(entries, index, range, DockEntry{DockEntryType::QuickSearch, L"__quick_search__"});
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
        if (services.freeItemOrder && services.dropSection)
        {
            const auto range = SectionRange(state.entries, *services.dropSection, services);
            auto placed = addition;
            placed.freeOrderSection = services.dropSection;
            insertIndex = std::clamp(insertIndex, range.begin, range.end);
            InsertInSection(state.entries, insertIndex++, range, std::move(placed));
        }
        else
        {
            insertIndex = std::min(insertIndex, state.entries.size());
            state.entries.insert(state.entries.begin() + static_cast<std::ptrdiff_t>(insertIndex), addition);
            ++insertIndex;
        }
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
        DockEntry entry{DockEntryType::DesktopItem, key, keepOnDesktop};
        if (services.freeItemOrder && services.dropSection)
        {
            const auto range = SectionRange(state.entries, *services.dropSection, services);
            insertIndex = std::clamp(insertIndex, range.begin, range.end);
            entry.freeOrderSection = services.dropSection;
            InsertInSection(state.entries, insertIndex, range, std::move(entry));
        }
        else
        {
            insertIndex = std::min(insertIndex, state.entries.size());
            state.entries.insert(state.entries.begin() + static_cast<std::ptrdiff_t>(insertIndex), std::move(entry));
        }
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
    std::erase_if(indices, [&](size_t index) {
        return index >= entries.size() ||
            (!services.freeItemOrder && entries[index].type == DockEntryType::QuickSearch);
    });
    if (!preserveCapturedOrder) std::sort(indices.begin(), indices.end());
    std::unordered_set<size_t> seen;
    std::erase_if(indices, [&](size_t index) { return !seen.insert(index).second; });
    if (indices.empty()) return ReorderResult::NoChange;
    const bool movingSearch = std::any_of(indices.begin(), indices.end(), [&](size_t index) {
        return entries[index].type == DockEntryType::QuickSearch;
    });
    // A reorder within grouped sections reuses each section's positions in the
    // saved mixed order, so toggling the grouping preserves the other sections.
    std::vector<size_t> groupedRanks;
    if (!services.freeItemOrder)
    {
        dock_folder_rules::EnsureUserOrder(entries);
        groupedRanks.reserve(entries.size());
        for (const auto& entry : entries) groupedRanks.push_back(*entry.userOrder);
    }
    insertIndex = std::min(insertIndex, entries.size());
    std::vector<DockEntry> movingMain, movingFolders;
    for (size_t index : indices)
        (!services.freeItemOrder && GroupOf(entries[index], services) == dock_folder_rules::EntryGroup::Folder
            ? movingFolders : movingMain).push_back(std::move(entries[index]));
    std::sort(indices.begin(), indices.end());
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it < insertIndex) --insertIndex;
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    if (services.freeItemOrder)
    {
        const auto section = movingSearch ? DockEntrySection::Files : services.dropSection.value_or(DockEntrySection::Fixed);
        const auto range = SectionRange(entries, section, services);
        insertIndex = std::clamp(insertIndex, range.begin, range.end);
        for (auto& entry : movingMain) entry.freeOrderSection = section;
        entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(insertIndex),
            std::make_move_iterator(movingMain.begin()), std::make_move_iterator(movingMain.end()));
        dock_folder_rules::CaptureUserOrder(entries);
        return ReorderResult::Changed;
    }
    const size_t mainEnd = static_cast<size_t>(std::count_if(entries.begin(), entries.end(),
        [&](const auto& entry) { return GroupOf(entry, services) == dock_folder_rules::EntryGroup::Main; }));
    const size_t folderEnd = entries.size();
    // Insert the later partition first: the main insertion then shifts the
    // complete file section while preserving each partition's captured order.
    const auto mainInsert = std::min(insertIndex, mainEnd);
    const auto folderInsert = std::clamp(insertIndex, mainEnd, folderEnd);
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(folderInsert),
        std::make_move_iterator(movingFolders.begin()), std::make_move_iterator(movingFolders.end()));
    entries.insert(entries.begin() + static_cast<std::ptrdiff_t>(mainInsert),
        std::make_move_iterator(movingMain.begin()), std::make_move_iterator(movingMain.end()));
    for (size_t i = 0; i < entries.size(); ++i) entries[i].userOrder = groupedRanks[i];
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
