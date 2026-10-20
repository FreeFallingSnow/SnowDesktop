#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace snowdesktop::dock_folder_rules
{

enum class EntryGroup
{
    Main,
    Folder,
    Recycle,
};

constexpr int Rank(EntryGroup group)
{
    switch (group)
    {
    case EntryGroup::Main:
        return 0;
    case EntryGroup::Folder:
        return 1;
    case EntryGroup::Recycle:
        return 2;
    }
    return 0;
}

template <typename Entry, typename Classifier>
void StableNormalize(
    std::vector<Entry>& entries,
    Classifier classify)
{
    std::stable_sort(
        entries.begin(), entries.end(),
        [&](const Entry& left, const Entry& right) {
            return Rank(classify(left)) <
                Rank(classify(right));
        });
}

// Explicit reorders replace the saved order; metadata refreshes never do.
// A rank belongs to the entry, so renaming its path cannot lose its position.
template <typename Entry>
void CaptureUserOrder(std::vector<Entry>& entries)
{
    for (std::size_t i = 0; i < entries.size(); ++i)
        entries[i].userOrder = i;
}

template <typename Entry>
void EnsureUserOrder(std::vector<Entry>& entries)
{
    if (std::none_of(entries.begin(), entries.end(),
            [](const Entry& entry) { return !entry.userOrder.has_value(); }))
        return;

    std::vector<Entry*> ordered;
    ordered.reserve(entries.size());
    for (auto& entry : entries)
        if (entry.userOrder) ordered.push_back(&entry);
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const Entry* left, const Entry* right) {
            return left->userOrder < right->userOrder;
        });
    // New entries inherit their insertion boundary in the current view,
    // without recapturing the temporarily grouped order of existing entries.
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        if (entries[i].userOrder) continue;
        auto next = std::find_if(entries.begin() + static_cast<std::ptrdiff_t>(i + 1),
            entries.end(), [](const Entry& entry) { return entry.userOrder.has_value(); });
        const auto at = next == entries.end()
            ? ordered.end() : std::find(ordered.begin(), ordered.end(), &*next);
        ordered.insert(at, &entries[i]);
    }
    for (std::size_t i = 0; i < ordered.size(); ++i)
        ordered[i]->userOrder = i;
}

template <typename Entry, typename Classifier>
void NormalizeKeepingUserOrder(
    std::vector<Entry>& entries, Classifier classify)
{
    EnsureUserOrder(entries);

    std::stable_sort(entries.begin(), entries.end(),
        [&](const Entry& left, const Entry& right) {
            const int leftRank = Rank(classify(left));
            const int rightRank = Rank(classify(right));
            if (leftRank != rightRank) return leftRank < rightRank;
            return left.userOrder < right.userOrder;
        });
}

// Persist the user's array, including entries whose first Shell result has
// not arrived. Serializing the grouped view would lose its cold-start order.
template <typename Entry>
std::vector<const Entry*> EntriesInUserOrder(const std::vector<Entry>& entries)
{
    std::vector<const Entry*> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) result.push_back(&entry);
    std::stable_sort(result.begin(), result.end(),
        [](const Entry* left, const Entry* right) {
            if (left->userOrder.has_value() != right->userOrder.has_value())
                return left->userOrder.has_value();
            return left->userOrder < right->userOrder;
        });
    return result;
}

struct InsertRange
{
    std::size_t begin = 0;
    std::size_t end = 0;
};

constexpr InsertRange GroupInsertRange(
    bool folderSource,
    std::size_t mainCount,
    std::size_t folderCount)
{
    return folderSource
        ? InsertRange{
            mainCount,
            mainCount + folderCount }
        : InsertRange{ 0, mainCount };
}

constexpr std::size_t SlotInsertionIndex(
    InsertRange range,
    std::size_t slotIndex,
    bool after)
{
    // Resolve the hit slot's edge before constraining it to the source group.
    // Clamping first would turn the right half of any preceding fixed icon
    // into an insertion after the first file icon.
    if (after && slotIndex < range.end) ++slotIndex;
    return std::clamp(slotIndex, range.begin, range.end);
}

constexpr bool OpenPopupNeedsRefreshAfterDrop(
    bool popupOpen,
    bool targetContainerIsPopup,
    bool targetItemContainerIsPopup,
    bool targetMatchesDockEntry,
    bool targetMatchesFolderPath)
{
    return popupOpen &&
        (targetContainerIsPopup ||
         targetItemContainerIsPopup ||
         targetMatchesDockEntry ||
         targetMatchesFolderPath);
}

constexpr long long SharedScrollableExtent(
    std::size_t mainCount,
    std::size_t runningCount,
    std::size_t frequentCount,
    std::size_t folderCount,
    int itemPitch,
    int separatorGap)
{
    int separatorCount = 0;
    if (runningCount > 0 && mainCount > 0)
        ++separatorCount;
    if (frequentCount > 0 &&
        (mainCount > 0 || runningCount > 0))
        ++separatorCount;
    if (folderCount > 0 &&
        (mainCount > 0 || runningCount > 0 ||
         frequentCount > 0))
        ++separatorCount;
    return static_cast<long long>(
               mainCount + runningCount +
               frequentCount + folderCount) *
            itemPitch +
        static_cast<long long>(separatorCount) *
            separatorGap;
}

constexpr long long ScrollableExtentForLayout(
    bool edgeAttached,
    std::size_t mainCount,
    std::size_t runningCount,
    std::size_t frequentCount,
    std::size_t folderCount,
    int itemPitch,
    int separatorGap)
{
    return SharedScrollableExtent(
        mainCount, runningCount, frequentCount,
        edgeAttached ? 0 : folderCount,
        itemPitch, separatorGap);
}

constexpr long long EdgeAttachedTrailingReserve(
    std::size_t folderCount,
    std::size_t trailingControlCount,
    bool hasLeadingItems,
    int itemPitch,
    int separatorGap)
{
    return static_cast<long long>(
               folderCount + trailingControlCount) *
            itemPitch +
        (hasLeadingItems ? separatorGap : 0);
}

constexpr long long FolderAxisStartBeforeSearch(
    long long searchStart,
    std::size_t folderCount,
    std::size_t folderIndex,
    int itemPitch)
{
    return searchStart -
        static_cast<long long>(folderCount - folderIndex) *
            itemPitch;
}

} // namespace snowdesktop::dock_folder_rules
