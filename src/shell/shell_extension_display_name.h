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
    if (text.empty() || text.front() == L'@') return false;
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
}
