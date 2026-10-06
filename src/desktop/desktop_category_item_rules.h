#pragma once

#include "common/types.h"

namespace snowdesktop::category_collection_rules
{
// Shell desktop entries (including User Files with a filesystem-backed PIDL)
// are launchable namespace identities, not ordinary files or folders.
inline bool IsDesktopNamespaceProgram(const DesktopItem& item)
{
    return !item.desktopIconClsid.empty() ||
        item.parsingName.starts_with(L"::{");
}

inline std::wstring DesktopNamespaceCategory(
    const DesktopItem& item, bool programsCategoryEnabled)
{
    if (!IsDesktopNamespaceProgram(item)) return {};
    return programsCategoryEnabled ? L"programs" : L"others";
}

inline bool IsCollectableDesktopItem(const DesktopItem& item,
    bool collectProgramsEnabled, bool filesystemFolder,
    std::wstring_view extension,
    const std::vector<std::wstring>& programExtensions)
{
    if (item.layoutKey.empty()) return false;
    // Check namespace identity first: User Files must not bypass the opt-in
    // merely because its PIDL resolves to a directory.
    if (IsDesktopNamespaceProgram(item)) return collectProgramsEnabled;
    return filesystemFolder || collectProgramsEnabled ||
        !IsProgramItem(extension, item.isApplicationShortcut,
            programExtensions, item.shortcutTarget);
}
}
