#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace snowdesktop::shell_item_action_rules
{

enum class RemovalAction : std::uint8_t
{
    Disabled,
    DeleteFiles,
    HideDesktopNamespace,
    RemoveDockMapping,
};

constexpr RemovalAction ResolveRemovalAction(
    std::size_t selectedCount,
    std::size_t selectedFileCount,
    std::size_t selectedNamespaceCount,
    bool dockMapping) noexcept
{
    if (dockMapping)
        return RemovalAction::RemoveDockMapping;
    if (selectedCount == 1 && selectedNamespaceCount == 1)
        return RemovalAction::HideDesktopNamespace;
    if (selectedCount > 0 &&
        selectedNamespaceCount == 0 &&
        selectedFileCount == selectedCount)
    {
        return RemovalAction::DeleteFiles;
    }
    return RemovalAction::Disabled;
}

constexpr bool IsAdministratorRunnableExtension(
    std::wstring_view extension) noexcept
{
    return extension == L".exe" ||
        extension == L".com" ||
        extension == L".bat" ||
        extension == L".cmd" ||
        extension == L".msi" ||
        extension == L".msc" ||
        extension == L".cpl" ||
        extension == L".scr" ||
        extension == L".lnk";
}

} // namespace snowdesktop::shell_item_action_rules
