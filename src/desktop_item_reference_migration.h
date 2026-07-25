#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace snowdesktop::desktop_item_reference_migration
{

struct MigrationResult
{
    size_t widgetReferences = 0;
    size_t dockReferences = 0;

    [[nodiscard]] bool Changed() const
    {
        return widgetReferences != 0 || dockReferences != 0;
    }
};

inline bool KeysEqual(
    const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(
        left.c_str(), static_cast<int>(left.size()),
        right.c_str(), static_cast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

inline bool ReplaceMatchingKey(
    std::wstring& value,
    const std::wstring& oldKey,
    const std::wstring& newKey)
{
    if (!KeysEqual(value, oldKey))
        return false;
    value = newKey;
    return true;
}

inline MigrationResult MigrateReferences(
    std::vector<DesktopWidget>& widgets,
    std::vector<DockEntry>& dockEntries,
    const std::wstring& oldKey,
    const std::wstring& newKey)
{
    MigrationResult result;
    if (oldKey.empty() || newKey.empty())
        return result;

    for (DesktopWidget& widget : widgets)
    {
        for (std::wstring& key : widget.itemKeys)
        {
            if (ReplaceMatchingKey(key, oldKey, newKey))
                ++result.widgetReferences;
        }
    }

    for (DockEntry& entry : dockEntries)
    {
        if (entry.type == DockEntryType::DesktopItem &&
            ReplaceMatchingKey(
                entry.reference, oldKey, newKey))
        {
            ++result.dockReferences;
        }
    }
    return result;
}

inline bool IsDockMapping(const DockEntry& entry)
{
    return entry.type == DockEntryType::DesktopItem &&
        entry.keepOnDesktop;
}

inline bool RemoveDockMappingAt(
    std::vector<DockEntry>& dockEntries, size_t index)
{
    if (index >= dockEntries.size() ||
        !IsDockMapping(dockEntries[index]))
    {
        return false;
    }
    dockEntries.erase(
        dockEntries.begin() +
        static_cast<std::ptrdiff_t>(index));
    return true;
}

} // namespace snowdesktop::desktop_item_reference_migration
