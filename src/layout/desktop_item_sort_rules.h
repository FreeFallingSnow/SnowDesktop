#pragma once

#include "folder_sort_rules.h"

namespace snowdesktop::desktop_item_sort_rules
{
// Missing size/time metadata stays last in both directions, with name ties.
// Type sorting instead reverses its complete category/type/name comparison.
template<class Item>
inline bool Less(const Item& a, const Item& b, int mode, bool ascending,
    bool directoryA = false, bool directoryB = false)
{
    int comparison = 0;
    if (mode == folder_sort_rules::kType)
    {
        comparison = folder_sort_rules::CompareType(
            folder_sort_rules::TypeGroupFor(a, directoryA), a.typeName, a.name,
            folder_sort_rules::TypeGroupFor(b, directoryB), b.typeName, b.name);
        return ascending ? comparison < 0 : comparison > 0;
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
    return Less(a, b, mode, ascending, directoryA, directoryB);
}
} // namespace snowdesktop::desktop_item_sort_rules
