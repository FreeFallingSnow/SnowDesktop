#pragma once
#include <windows.h>
#include <objbase.h>
#include <string>
#include <string_view>
#include <iterator>
#include <cwctype>

namespace snowdesktop::shell_extensions
{
// Live, source-scoped proof for a metadata query. This does not enumerate the
// machine's unrelated file classes or grant any command execution authority.
inline bool SourceRegistered(const std::wstring &clsid, const std::wstring &scope,
                             HKEY classes = HKEY_CLASSES_ROOT)
{
    GUID expected{};
    if (scope.empty() || FAILED(CLSIDFromString(clsid.c_str(), &expected))) return false;
    HKEY handlers = nullptr;
    if (RegOpenKeyExW(classes, (scope + L"\\shellex\\ContextMenuHandlers").c_str(), 0, KEY_READ, &handlers) != ERROR_SUCCESS)
        return false;
    bool registered = false;
    for (DWORD index = 0; !registered && index < 65536; ++index)
    {
        wchar_t name[256]{}; DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(handlers, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t value[256]{}; DWORD bytes = sizeof(value);
        const auto read = RegGetValueW(handlers, name, nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, value, &bytes);
        if (read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND) continue;
        const wchar_t *identity = read == ERROR_SUCCESS && value[0] ? value : name;
        GUID actual{};
        registered = SUCCEEDED(CLSIDFromString(identity, &actual)) && IsEqualGUID(actual, expected);
    }
    RegCloseKey(handlers);
    return registered;
}

// Carry provenance from a source-scoped read even when the application's
// global attribution inventory has not finished. Enabled policy remains the
// caller's separate live check; this identity never grants execution authority.
inline std::string SourceRegistrationId(const std::wstring& clsid, const std::wstring& scope,
                                      HKEY classes = HKEY_CLASSES_ROOT)
{
    if (!SourceRegistered(clsid, scope, classes)) return {};
    GUID identity{};
    if (FAILED(CLSIDFromString(clsid.c_str(), &identity))) return {};
    wchar_t canonical[40]{};
    if (!StringFromGUID2(identity, canonical, static_cast<int>(std::size(canonical)))) return {};
    std::string result = "clsid:";
    for (const auto character : std::wstring_view(canonical))
        result.push_back(static_cast<char>(std::towlower(character)));
    return result;
}
}
