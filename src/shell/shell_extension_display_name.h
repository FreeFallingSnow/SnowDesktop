#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <string_view>
#include <cwctype>

namespace snowdesktop::shell_extensions
{
inline bool UsableShellDisplayName(std::wstring_view text)
{
    if (text.empty() || text.front() == L'@' || text.find_first_not_of(L" \t\r\n") == std::wstring_view::npos ||
        text.find_first_of(L"<>") != std::wstring_view::npos) return false;
    std::wstring lower(text);
    for (auto& c : lower) c = towlower(c);
    if (lower.starts_with(L"ms-resource:") || lower.starts_with(L"package:") ||
        lower.starts_with(L"progid:") || lower.starts_with(L"clsid:")) return false;
    auto guid = std::wstring(text);
    if (guid.size() == 36) guid = L"{" + guid + L"}";
    GUID parsed{};
    if (guid.size() == 38 && SUCCEEDED(CLSIDFromString(guid.c_str(), &parsed))) return false;
    return !(text.size() >= 3 && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) &&
        !text.starts_with(L"\\\\");
}
inline int CompareShellDisplayNames(const std::wstring& left, const std::wstring& right)
{
    const auto result = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
        left.c_str(), -1, right.c_str(), -1, nullptr, nullptr, 0);
    return result ? result - CSTR_EQUAL : left.compare(right);
}
}
