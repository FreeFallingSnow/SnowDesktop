#pragma once

#include <string_view>

namespace snowdesktop::shell_item_action_rules
{

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
