#pragma once
#include <string>
#include <string_view>

namespace snowdesktop::icon_override
{
inline constexpr std::wstring_view start = L"control:start";
inline constexpr std::wstring_view search = L"control:search";
inline constexpr unsigned beautify = 43780, inherit = 43781;
inline constexpr unsigned change = 43772, restore = 43773;
inline constexpr unsigned emptyChange = 43774, emptyRestore = 43775;
inline constexpr unsigned fullChange = 43776, fullRestore = 43777;
// The caller supplies the existing canonical layout key, shared by all views.
inline std::wstring Target(std::wstring_view key, int recycleState = -1)
{
    if (key == start || key == search) return std::wstring(key);
    return std::wstring(recycleState == 0 ? L"recycle-empty:" :
        recycleState == 1 ? L"recycle-full:" : L"item:") + std::wstring(key);
}
}
