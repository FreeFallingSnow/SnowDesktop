#pragma once

#include <windows.h>
#include <oleidl.h>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::default_file_drop_action
{
inline bool Requested(DWORD keys, DWORD allowed) noexcept
{
    return (keys & (MK_CONTROL | MK_SHIFT | MK_ALT)) == 0 &&
        (allowed & (DROPEFFECT_MOVE | DROPEFFECT_COPY)) == (DROPEFFECT_MOVE | DROPEFFECT_COPY);
}

inline std::optional<ULONGLONG> Volume(const std::wstring& path, bool source) noexcept
{
    if (path.empty()) return std::nullopt;
    // Resolve the actual destination directory, including a junction/mount.
    // A source reparse point itself is the object being moved, not its target.
    const HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | (source ? FILE_FLAG_OPEN_REPARSE_POINT : 0), nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
    FILE_ID_INFO identity{};
    const bool read = GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)) != FALSE;
    CloseHandle(handle);
    return read ? std::optional<ULONGLONG>{identity.VolumeSerialNumber} : std::nullopt;
}

inline std::optional<DWORD> Preferred(const std::vector<std::wstring>& paths,
    const std::wstring& destination) noexcept
{
    if (paths.empty()) return std::nullopt;
    const auto target = Volume(destination, false);
    if (!target) return std::nullopt;
    bool crossVolume = false;
    for (const auto& path : paths)
    {
        const auto source = Volume(path, true);
        if (!source) return std::nullopt;
        crossVolume |= *source != *target;
    }
    return crossVolume ? DROPEFFECT_COPY : DROPEFFECT_MOVE;
}
}
