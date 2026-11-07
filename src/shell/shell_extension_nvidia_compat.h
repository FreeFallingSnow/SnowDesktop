#pragma once
#include "shell_extension_catalogue.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <set>

namespace snowdesktop::shell_extensions
{
inline constexpr wchar_t NvidiaControlPanelClsid[] = L"{3D1975AF-48C6-4F8E-A182-BE0E08FA86A9}";
inline constexpr char NvidiaControlPanelRegistration[] = "clsid:{3d1975af-48c6-4f8e-a182-be0e08fa86a9}";
inline constexpr wchar_t NvidiaAppClsid[] = L"{F2E8B4A1-9C7D-4F6E-B3A5-8D2C1F4E9B7A}";

// The legacy nvshext factory rejects non-Explorer processes with
// CLASS_E_CLASSNOTAVAILABLE. Use the installed application's Shell open verb
// only for that exact failure; never change NVIDIA's machine-wide policy.
inline bool NvidiaDesktopHandlerRegistered(const wchar_t* clsid, HKEY classes = HKEY_CLASSES_ROOT)
{
    CLSID expected{};
    if (FAILED(CLSIDFromString(clsid, &expected))) return false;
    for (const auto *path : {L"Directory\\Background\\shellex\\ContextMenuHandlers",
                             L"DesktopBackground\\shellex\\ContextMenuHandlers"})
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(classes, path, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        bool found = false;
        for (DWORD i = 0; i < 65536 && !found; ++i)
        {
            wchar_t name[1024]{}; DWORD characters = static_cast<DWORD>(std::size(name));
            if (RegEnumKeyExW(key, i, name, &characters, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t value[128]{}; DWORD bytes = sizeof(value);
            if (RegGetValueW(key, name, nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes) != ERROR_SUCCESS)
                wcsncpy_s(value, name, _TRUNCATE);
            CLSID actual{};
            found = SUCCEEDED(CLSIDFromString(value, &actual)) && actual == expected;
        }
        RegCloseKey(key);
        if (found) return true;
    }
    return false;
}
inline bool NvidiaControlPanelRegistered(HKEY classes = HKEY_CLASSES_ROOT)
{
    return NvidiaDesktopHandlerRegistered(NvidiaControlPanelClsid, classes);
}
inline bool NvidiaCompatibilityRequired(const Request &request, bool registered, bool enabled, HRESULT factory)
{
    return !request.omitNvidiaCompatibility && request.background && ResolveContext(request) == Context::Desktop && request.sourceClsid.empty() &&
        registered && enabled && factory == CLASS_E_CLASSNOTAVAILABLE;
}

inline bool NvidiaCompatibilityShown(const std::set<std::string> &shown)
{
    // Match proven command/provider identities, independently of the NVIDIA App.
    return shown.contains(NvidiaControlPanelRegistration) ||
        shown.contains("verb:{3d1975af-48c6-4f8e-a182-be0e08fa86a9}") ||
        shown.contains("handler:{3d1975af-48c6-4f8e-a182-be0e08fa86a9}");
}

// Do not guess a command by position or translated caption. AppsFolder may
// return uninstall, elevation and file operations even for CMF_DEFAULTONLY.
inline std::optional<UINT> DefaultApplicationOpen(IContextMenu *context, HMENU menu)
{
    if (!context || !menu) return {};
    const UINT id = GetMenuDefaultItem(menu, FALSE, 0);
    if (id < 1 || id > 0x7fff) return {};
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_STATE | MIIM_FTYPE | MIIM_SUBMENU;
    if (!GetMenuItemInfoW(menu, id, FALSE, &item) || item.hSubMenu ||
        (item.fType & MFT_SEPARATOR) || (item.fState & (MFS_DISABLED | MFS_GRAYED))) return {};
    wchar_t verb[64]{};
    if (FAILED(context->GetCommandString(id - 1, GCS_VERBW, nullptr, reinterpret_cast<LPSTR>(verb),
                                         static_cast<UINT>(std::size(verb)))) || verb[std::size(verb) - 1] ||
        _wcsicmp(verb, L"open")) return {};
    return id - 1;
}
inline Microsoft::WRL::ComPtr<IShellItem> NvidiaControlPanelApplication()
{
    Microsoft::WRL::ComPtr<IShellItem> item;
    // This publisher-qualified AUMID identifies the installed DCH Control
    // Panel, independently of the NVIDIA App and localized display names.
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY,
        L"NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIACorp.NVIDIAControlPanel",
        IID_PPV_ARGS(&item)))) return item;
    // Standard drivers can instead register the classic Win32 executable.
    // Never search PATH, the current directory, or an arbitrary command value.
    for (const auto hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE})
    {
        wchar_t path[32768]{}; DWORD bytes = sizeof(path);
        if (RegGetValueW(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\nvcplui.exe",
            nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, path, &bytes) != ERROR_SUCCESS) continue;
        PathUnquoteSpacesW(path);
        const std::filesystem::path file(path);
        if (!file.is_absolute() || PathIsNetworkPathW(path) || GetDriveTypeW(file.root_path().c_str()) == DRIVE_REMOTE ||
            _wcsicmp(file.filename().c_str(), L"nvcplui.exe")) continue;
        const auto attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item)))) return item;
    }
    return {};
}
inline Microsoft::WRL::ComPtr<IShellItem> NvidiaAppApplication()
{
    // Resolve only the installation identified by this registered provider.
    // No PATH search, arbitrary command string or global driver-policy change.
    wchar_t module[32768]{}; DWORD bytes = sizeof(module);
    const std::wstring key = std::wstring(L"CLSID\\") + NvidiaAppClsid + L"\\InprocServer32";
    if (RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr, module, &bytes) != ERROR_SUCCESS) return {};
    PathUnquoteSpacesW(module);
    const std::filesystem::path provider(module);
    if (!provider.is_absolute() || PathIsNetworkPathW(module) ||
        GetDriveTypeW(provider.root_path().c_str()) == DRIVE_REMOTE ||
        _wcsicmp(provider.filename().c_str(), L"nvui.dll") ||
        _wcsicmp(provider.parent_path().filename().c_str(), L"NvCpl")) return {};
    const auto target = provider.parent_path().parent_path() / L"CEF" / L"NVIDIA App.exe";
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) return item;
    return {};
}
} // namespace snowdesktop::shell_extensions
