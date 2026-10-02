#pragma once

#include "../dock_app_identity_rules.h"
#include "../types.h"

#include <windows.h>
#include <shlobj.h>
#include <propkey.h>
#include <wrl/client.h>

#include <filesystem>
#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace snowdesktop::dock_running_app_pin
{
struct ApplicationIdentity
{
    std::wstring executablePath;
    std::wstring appUserModelId;
};

inline ApplicationIdentity ReadApplicationIdentity(PCIDLIST_ABSOLUTE target)
{
    ApplicationIdentity identity;
    Microsoft::WRL::ComPtr<IShellItem2> item;
    if (!target || FAILED(SHCreateItemFromIDList(target, IID_PPV_ARGS(&item))))
        return identity;
    auto readString = [&](REFPROPERTYKEY property) {
        PWSTR raw = nullptr;
        std::wstring value;
        if (SUCCEEDED(item->GetString(property, &raw)) && raw) value = raw;
        if (raw) CoTaskMemFree(raw);
        return value;
    };
    identity.appUserModelId = readString(PKEY_AppUserModel_ID);
    const auto path = readString(PKEY_Link_TargetParsingPath);
    if (_wcsicmp(std::filesystem::path(path).extension().c_str(), L".exe") == 0)
        identity.executablePath = path;
    return identity;
}

// Exact application IDs outrank executable-family matches. Reject ambiguous
// catalog entries (for example two command prompts with different arguments).
inline std::optional<size_t> FindApplication(
    std::span<const ApplicationIdentity> applications,
    const std::wstring& executablePath, const std::wstring& appUserModelId,
    std::span<const std::wstring> ancestors = {})
{
    std::optional<size_t> match;
    int bestRank = 0;
    bool ambiguous = false;
    for (size_t i = 0; i < applications.size(); ++i)
    {
        const auto& app = applications[i];
        const bool exactId = !appUserModelId.empty() &&
            app.appUserModelId == appUserModelId;
        const bool conflictingId = !appUserModelId.empty() &&
            !app.appUserModelId.empty() && !exactId;
        int rank = exactId ? 2 : 0;
        if (!exactId && !conflictingId &&
            dock_app_identity_rules::MatchesRunningApp(
                DockAppIdentityKind::Executable, app.executablePath,
                app.appUserModelId, {}, executablePath, appUserModelId,
                ancestors))
            rank = 1;
        if (rank == 0 || rank < bestRank) continue;
        if (rank == bestRank)
        {
            ambiguous = true;
            continue;
        }
        match = i;
        bestRank = rank;
        ambiguous = false;
    }
    return ambiguous ? std::nullopt : match;
}

// Reserve a new file atomically. Never overwrite an existing shortcut, even
// when every ordinary name is occupied or another operation creates one first.
inline std::wstring CreateShortcut(const std::filesystem::path& directory,
    const std::wstring& stem, PCIDLIST_ABSOLUTE target)
{
    if (directory.empty() || stem.empty() || !target) return {};
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&link))) || FAILED(link->SetIDList(target)) ||
        FAILED(link.As(&file)))
        return {};
    const auto application = ReadApplicationIdentity(target);
    if (!application.appUserModelId.empty())
    {
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        PROPVARIANT value{};
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<PWSTR>(application.appUserModelId.c_str());
        if (FAILED(link.As(&properties)) ||
            FAILED(properties->SetValue(PKEY_AppUserModel_ID, value)) ||
            FAILED(properties->Commit()))
            return {};
    }

    for (int suffix = 1; suffix <= 1000; ++suffix)
    {
        const auto name = stem + (suffix == 1 ? std::wstring{} :
            L" (" + std::to_wstring(suffix) + L")") + L".lnk";
        const auto path = (directory / name).wstring();
        const HANDLE reservation = CreateFileW(path.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)
                continue;
            return {};
        }
        CloseHandle(reservation);
        if (SUCCEEDED(file->Save(path.c_str(), TRUE))) return path;
        DeleteFileW(path.c_str());
        return {};
    }
    return {};
}

// Materialize the file just created by this operation before publishing its
// Dock entry. Waiting for desktop enumeration would expose an empty Dock slot.
inline std::optional<DesktopItem> ReadShortcutItem(const std::wstring& path,
    const std::wstring& name)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return std::nullopt;
    PIDLIST_ABSOLUTE absolute = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, nullptr)))
        return std::nullopt;
    DesktopItem item;
    item.absolutePidl.reset(absolute);
    // Local desktop enumeration uses full PIDLs for filesystem sources too.
    item.childPidl.reset(ILCloneFull(absolute));
    if (!item.childPidl.get()) return std::nullopt;
    item.name = name;
    item.parsingName = path;
    item.modifiedTime = attributes.ftLastWriteTime;
    item.fileSize = (static_cast<std::uint64_t>(attributes.nFileSizeHigh) << 32) |
        attributes.nFileSizeLow;
    item.isShortcut = true;
    item.isApplicationShortcut = true;
    item.slot = -1;
    return item;
}

// Commit the visible handoff using the live running model, not the menu's
// shallow snapshot. The bitmap has one owner and sibling windows remain usable
// immediately, before any asynchronous identity query or periodic discovery.
template<class RunningApps, class WindowState, class ReleaseBitmap>
bool AdoptRunningPresentation(DesktopItem& item, WindowState& windowState,
    RunningApps& runningApps, const std::wstring& identityKey,
    ReleaseBitmap releaseBitmap)
{
    const auto running = std::find_if(runningApps.begin(), runningApps.end(),
        [&](const auto& app) { return app.identityKey == identityKey; });
    if (running == runningApps.end()) return false;
    if (!item.iconBitmap && running->iconBitmap)
    {
        item.iconBitmap = std::exchange(running->iconBitmap, nullptr);
        item.iconBitmapSize = running->iconBitmapSize;
        item.iconState = IconState::IconReady;
    }
    windowState.window = running->window;
    windowState.running = true;
    windowState.minimized = running->minimized;
    windowState.foreground = running->foreground;
    windowState.trackedWindows = std::move(running->trackedWindows);
    if (running->iconBitmap)
        releaseBitmap(std::exchange(running->iconBitmap, nullptr));
    runningApps.erase(running);
    return true;
}

// A failed capacity check must create nothing. Failed Dock insertion removes
// only the shortcut created by this operation; persistence follows insertion.
template<class HasCapacity, class Create, class Pin, class Remove, class Save>
bool CreateAndPin(HasCapacity hasCapacity, Create create, Pin pin,
    Remove remove, Save save)
{
    if (!hasCapacity()) return false;
    const auto path = create();
    if (path.empty()) return false;
    if (!pin(path))
    {
        remove(path);
        return false;
    }
    save();
    return true;
}
} // namespace snowdesktop::dock_running_app_pin
