#pragma once

#include "shell/shell_launch_policy.h"

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
    bool dockMapping,
    bool protectedDesktopIcon = false) noexcept
{
    if (dockMapping)
        return RemovalAction::RemoveDockMapping;
    if (protectedDesktopIcon)
        return RemovalAction::Disabled;
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

using shell_launch_policy::IsAdministratorRunnableExtension;

} // namespace snowdesktop::shell_item_action_rules
