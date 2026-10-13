#pragma once

#include <string>

namespace snowdesktop::winui
{

// Presentation-only context for a requested editor whose real prerequisite is
// not satisfied. Resolvers report the current blocking control without changing
// its value. An empty key means the requested editor is available as-is.
struct SettingsFocusPrerequisite
{
    std::string messageKey;
    std::wstring controlName;
};

} // namespace snowdesktop::winui
