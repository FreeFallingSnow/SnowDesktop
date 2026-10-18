#pragma once

#include "folder_sort_rules.h"

namespace snowdesktop::desktop_item_sort_rules
{
// Desktop namespace items and directories can lack size/time metadata.
// Keep unavailable values last in both directions; ties use stable name order.
template<class Item>
inline bool Less(const Item& a, const Item& b, int mode, bool ascending,
    bool directoryA = false, bool directoryB = false)
{
    int comparison = 0;
    if (mode == folder_sort_rules::kType)
    {
        if (directoryA != directoryB) return directoryA;
        const bool hasA = !a.typeName.empty();
        const bool hasB = !b.typeName.empty();
        if (hasA != hasB) return hasA;
        if (hasA) comparison = folder_sort_rules::CompareInsensitive(a.typeName, b.typeName);
    }
    else if (mode == folder_sort_rules::kModified)
    {
        const bool hasA = a.modifiedTime.has_value();
        const bool hasB = b.modifiedTime.has_value();
        if (hasA != hasB) return hasA;
        if (hasA) comparison = CompareFileTime(&*a.modifiedTime, &*b.modifiedTime);
    }
    else if (mode == folder_sort_rules::kSize)
    {
        const bool hasA = a.fileSize.has_value();
        const bool hasB = b.fileSize.has_value();
        if (hasA != hasB) return hasA;
        if (hasA && *a.fileSize != *b.fileSize)
            comparison = *a.fileSize < *b.fileSize ? -1 : 1;
    }
    else
        comparison = folder_sort_rules::CompareInsensitive(a.name, b.name);

    if (comparison != 0) return ascending ? comparison < 0 : comparison > 0;
    return folder_sort_rules::CompareInsensitive(a.name, b.name) < 0;
}

template<class Item>
inline bool LessOnDesktop(const Item& a, const Item& b, int mode, bool ascending,
    bool directoryA = false, bool directoryB = false)
{
    // Preserve the desktop's existing missing-type placement. The Type
    // direction applies to type blocks; names within each block stay ascending.
    if (mode == folder_sort_rules::kType)
    {
        if (directoryA != directoryB) return directoryA;
        const int comparison = folder_sort_rules::CompareInsensitive(a.typeName, b.typeName);
        if (comparison != 0) return ascending ? comparison < 0 : comparison > 0;
        return folder_sort_rules::CompareInsensitive(a.name, b.name) < 0;
    }
    return Less(a, b, mode, ascending, directoryA, directoryB);
}
} // namespace snowdesktop::desktop_item_sort_rules
