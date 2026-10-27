#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>

namespace snowdesktop::performance
{
// Captures are regular files on a local disk. Reject Win32 namespace aliases,
// alternate streams and names whose Win32 normalization changes their meaning.
inline bool IsLocalCapturePath(const std::filesystem::path& path)
{
    const auto name = path.native();
    if (name.size() < 4 || name[1] != L':' ||
        !((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'a' && name[0] <= L'z')) ||
        !path.is_absolute() || path != path.lexically_normal()) return false;
    const auto drive = GetDriveTypeW(path.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) return false;
    for (const auto& part : path.relative_path())
    {
        auto component = part.native();
        if (component.empty() || component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L"<>:\"|?*") != std::wstring::npos) return false;
        for (auto& ch : component)
        {
            if (ch < 32) return false;
            if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
        }
        const auto stem = std::wstring_view(component).substr(0, component.find(L'.'));
        if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
            stem == L"CONIN$" || stem == L"CONOUT$") return false;
        if (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
            ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == L'\u00b9' || stem[3] == L'\u00b2' || stem[3] == L'\u00b3')) return false;
    }
    return true;
}
}
