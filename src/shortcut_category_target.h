#pragma once

#include "category_collection_rules.h"
#include "shortcut_application_rules.h"
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <propkey.h>
#include <wrl/client.h>

namespace snowdesktop::category_collection_rules
{
// Load only the shortcut's saved target; never Resolve it, launch it, or ask
// Shell to search for a missing target. Normal UI filtering uses cached results.
inline ShortcutTarget ReadShellLinkTarget(IShellLinkW* link)
{
    namespace rules = snowdesktop::shortcut_application_rules;
    ShortcutTarget result;
    result.classified = true;
    if (!link) return result;
    wchar_t target[32768]{};
    WIN32_FIND_DATAW saved{};
    if (SUCCEEDED(link->GetPath(target, static_cast<int>(std::size(target)), &saved, 0)) && target[0])
    {
        const DWORD attributes = GetFileAttributesW(target);
        result.directory = ((attributes == INVALID_FILE_ATTRIBUTES ? saved.dwFileAttributes :
            attributes) & FILE_ATTRIBUTE_DIRECTORY) != 0;
        result.extension = PathFindExtensionW(target);
        for (auto& ch : result.extension) ch = static_cast<wchar_t>(std::towupper(ch));
    }
    auto property = [&](const PROPERTYKEY& key) {
        std::wstring value;
        Microsoft::WRL::ComPtr<IPropertyStore> store;
        PROPVARIANT variant{};
        if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store))) && SUCCEEDED(store->GetValue(key, &variant)) &&
            variant.vt == VT_LPWSTR && variant.pwszVal) value = variant.pwszVal;
        PropVariantClear(&variant);
        return value;
    };
    wchar_t arguments[32768]{};
    link->GetArguments(arguments, static_cast<int>(std::size(arguments)));
    result.application = rules::IsApplicationsShellLinkTarget(target, arguments,
        property(PKEY_AppUserModel_ID), property(PKEY_Link_TargetParsingPath), false);
    result.application = IsProgramItem(L".LNK", result.application, {}, result);
    return result;
}

inline ShortcutTarget ReadShortcutTarget(const std::wstring& path)
{
    namespace rules = snowdesktop::shortcut_application_rules;
    ShortcutTarget result;
    if (rules::HasExtension(path, L".url"))
    {
        wchar_t url[32768]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url,
            static_cast<DWORD>(std::size(url)), path.c_str());
        result.classified = true;
        result.application = rules::IsSteamApplicationUrl(url);
        return result;
    }
    if (!rules::HasExtension(path, L".lnk")) return result;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
        FAILED(file->Load(path.c_str(), STGM_READ)))
    {
        result.classified = true;
        return result;
    }
    return ReadShellLinkTarget(link.Get());
}
}
