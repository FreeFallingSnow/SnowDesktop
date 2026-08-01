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
