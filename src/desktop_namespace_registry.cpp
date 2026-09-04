#include "desktop_namespace_registry.h"

#include "constants.h"

#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <iterator>
#include <unordered_set>
#include <utility>
#include <vector>

namespace snowdesktop
{
namespace
{

constexpr wchar_t kDesktopNamespaceKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Desktop\\NameSpace";

bool EqualsInsensitive(
    std::wstring_view left,
    std::wstring_view right)
{
    if (left.size() != right.size())
        return false;
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) ==
        CSTR_EQUAL;
}

std::wstring TrimTrailingPathSeparators(std::wstring_view path)
{
    std::wstring result(path);
    while (result.size() > 3 &&
        (result.back() == L'\\' || result.back() == L'/'))
    {
        result.pop_back();
    }
    return result;
}

bool PathsEqual(
    std::wstring_view left,
    std::wstring_view right)
{
    const std::wstring normalizedLeft =
        TrimTrailingPathSeparators(left);
    const std::wstring normalizedRight =
        TrimTrailingPathSeparators(right);
    return !normalizedLeft.empty() &&
        !normalizedRight.empty() &&
        EqualsInsensitive(normalizedLeft, normalizedRight);
}

std::wstring CanonicalizeClsid(std::wstring_view text)
{
    std::wstring owned(text);
    CLSID value{};
    if (FAILED(CLSIDFromString(owned.c_str(), &value)))
        return {};

    wchar_t canonical[39]{};
    if (StringFromGUID2(value, canonical,
            static_cast<int>(std::size(canonical))) <= 0)
    {
        return {};
    }
    std::wstring result(canonical);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](wchar_t ch) {
            return static_cast<wchar_t>(std::towupper(ch));
        });
    return result;
}

std::wstring ReadRegistryString(
    HKEY key,
    const wchar_t* valueName)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(
            key, valueName, nullptr, &type,
            nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        bytes == 0)
    {
        return {};
    }

    std::vector<wchar_t> buffer(
        bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(
            key, valueName, nullptr, &type,
            reinterpret_cast<BYTE*>(buffer.data()),
            &bytes) != ERROR_SUCCESS)
    {
        return {};
    }

    std::wstring value(buffer.data());
    if (type != REG_EXPAND_SZ || value.empty())
        return value;

    const DWORD required = ExpandEnvironmentStringsW(
        value.c_str(), nullptr, 0);
    if (required == 0)
        return value;
    std::vector<wchar_t> expanded(required, L'\0');
    if (ExpandEnvironmentStringsW(
            value.c_str(), expanded.data(), required) == 0)
    {
        return value;
    }
    return expanded.data();
}

std::wstring ReadClassString(
    const std::wstring& clsid,
    const wchar_t* relativePath,
    const wchar_t* valueName,
    REGSAM registryView)
{
    std::wstring subKey = L"CLSID\\" + clsid;
    if (relativePath && relativePath[0])
    {
        subKey += L'\\';
        subKey += relativePath;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CLASSES_ROOT, subKey.c_str(), 0,
            KEY_QUERY_VALUE | registryView, &key) != ERROR_SUCCESS)
    {
        return {};
    }
    std::wstring value = ReadRegistryString(key, valueName);
    RegCloseKey(key);
    return value;
}

std::wstring ResolveIndirectDisplayName(std::wstring value)
{
    if (value.empty() || value.front() != L'@')
        return value;
    wchar_t resolved[512]{};
    if (SUCCEEDED(SHLoadIndirectString(
            value.c_str(), resolved,
            static_cast<UINT>(std::size(resolved)),
            nullptr)) && resolved[0])
    {
        return resolved;
    }
    return value;
}

void AppendNamespaceRegistrations(
    HKEY root,
    REGSAM registryView,
    std::unordered_set<std::wstring>& seen,
    std::vector<DesktopNamespaceRegistration>& result)
{
    HKEY namespaceKey = nullptr;
    if (RegOpenKeyExW(
            root, kDesktopNamespaceKey, 0,
            KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE |
                registryView,
            &namespaceKey) != ERROR_SUCCESS)
    {
        return;
    }

    DWORD maxSubKeyLength = 0;
    RegQueryInfoKeyW(
        namespaceKey, nullptr, nullptr, nullptr,
        nullptr, &maxSubKeyLength, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr);
    std::vector<wchar_t> name(maxSubKeyLength + 2, L'\0');

    for (DWORD index = 0;; ++index)
    {
        DWORD nameLength = static_cast<DWORD>(name.size());
        FILETIME modified{};
        const LONG status = RegEnumKeyExW(
            namespaceKey, index, name.data(), &nameLength,
            nullptr, nullptr, nullptr, &modified);
        if (status == ERROR_NO_MORE_ITEMS)
            break;
        if (status != ERROR_SUCCESS)
            continue;

        const std::wstring clsid = CanonicalizeClsid(
            std::wstring_view(name.data(), nameLength));
        if (clsid.empty() || !seen.insert(clsid).second)
            continue;

        HKEY itemKey = nullptr;
        std::wstring displayName;
        if (RegOpenKeyExW(
                namespaceKey, name.data(), 0,
                KEY_QUERY_VALUE | registryView,
                &itemKey) == ERROR_SUCCESS)
        {
            displayName = ReadRegistryString(itemKey, nullptr);
            RegCloseKey(itemKey);
        }
        if (displayName.empty())
        {
            displayName = ReadClassString(
                clsid, nullptr, nullptr, registryView);
        }

        DesktopNamespaceRegistration registration;
        registration.clsid = clsid;
        registration.userScoped = root == HKEY_CURRENT_USER;
        registration.displayName =
            ResolveIndirectDisplayName(std::move(displayName));
        if (registration.displayName.empty())
            registration.displayName = clsid;
        registration.targetPath = ReadClassString(
            clsid, L"Instance\\InitPropertyBag",
            L"TargetFolderPath", registryView);

        std::wstring parsingName = L"shell:::" + clsid;
        PIDLIST_ABSOLUTE parsed = nullptr;
        if (SUCCEEDED(SHParseDisplayName(
                parsingName.c_str(), nullptr,
                &parsed, 0, nullptr)))
        {
            registration.absolutePidl.reset(parsed);
        }
        result.push_back(std::move(registration));
    }
    RegCloseKey(namespaceKey);
}

} // namespace

std::vector<DesktopNamespaceRegistration>
LoadDesktopNamespaceRegistrations()
{
    std::vector<DesktopNamespaceRegistration> registrations;
    std::unordered_set<std::wstring> seen;

    AppendNamespaceRegistrations(
        HKEY_CURRENT_USER, KEY_WOW64_64KEY,
        seen, registrations);
    AppendNamespaceRegistrations(
        HKEY_CURRENT_USER, KEY_WOW64_32KEY,
        seen, registrations);
    AppendNamespaceRegistrations(
        HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY,
        seen, registrations);
    AppendNamespaceRegistrations(
        HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY,
        seen, registrations);
    return registrations;
}

std::wstring ResolveRegisteredDesktopNamespaceClsid(
    PCIDLIST_ABSOLUTE itemPidl,
    std::wstring_view itemPath,
    const std::vector<DesktopNamespaceRegistration>& registrations,
    bool* visibleByDefault)
{
    if (visibleByDefault)
        *visibleByDefault = false;
    if (itemPidl)
    {
        for (const auto& registration : registrations)
        {
            if (registration.absolutePidl.get() &&
                ILIsEqual(itemPidl,
                    registration.absolutePidl.get()))
            {
                if (visibleByDefault)
                {
                    *visibleByDefault =
                        registration.userScoped;
                }
                return registration.clsid;
            }
        }
    }

    if (!itemPath.empty())
    {
        for (const auto& registration : registrations)
        {
            if (PathsEqual(
                    itemPath,
                    registration.targetPath))
            {
                if (visibleByDefault)
                {
                    *visibleByDefault =
                        registration.userScoped;
                }
                return registration.clsid;
            }
        }
    }
    return {};
}

bool IsStandardDesktopIconClsid(std::wstring_view clsid)
{
    return EqualsInsensitive(clsid, kDesktopIconClsidThisPC) ||
        EqualsInsensitive(clsid, kDesktopIconClsidUserFiles) ||
        EqualsInsensitive(clsid, kDesktopIconClsidNetwork) ||
        EqualsInsensitive(clsid, kDesktopIconClsidControlPanel) ||
        EqualsInsensitive(clsid, kDesktopIconClsidRecycleBin);
}

} // namespace snowdesktop
