#pragma once
#include <cwctype>
#include <string_view>

namespace snowdesktop::status_bar_input_method::native_menu
{
inline bool MatchesModeButton(std::wstring_view name, std::wstring_view prefix)
{
    return !prefix.empty() && name.starts_with(prefix) &&
        (name.size() == prefix.size() || iswspace(name[prefix.size()]));
}
}
