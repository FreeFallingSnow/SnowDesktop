#include "shortcut_icon_resource.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include "shortcut_application_rules.h"

#include <climits>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace snowdesktop::shortcut_icon_resource
{
namespace
{
std::wstring_view Trim(std::wstring_view value)
{
    while (!value.empty() &&
        (value.front() == L' ' || value.front() == L'\t' ||
            value.front() == L'\r' || value.front() == L'\n'))
        value.remove_prefix(1);
    while (!value.empty() &&
        (value.back() == L' ' || value.back() == L'\t' ||
            value.back() == L'\r' || value.back() == L'\n'))
        value.remove_suffix(1);
    return value;
}

std::wstring ExpandIconPath(std::wstring_view path)
{
    path = Trim(path);
    if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
    {
        path.remove_prefix(1);
        path.remove_suffix(1);
    }

    const std::wstring value(path);
    const DWORD required = ExpandEnvironmentStringsW(
        value.c_str(), nullptr, 0);
    if (required == 0)
        return value;

    std::vector<wchar_t> expanded(required);
    if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(),
            required) == 0)
        return value;
    return expanded.data();
}

bool IsRelativeIconPath(std::wstring_view path)
{
    if (path.empty())
        return false;
    if (path.size() >= 2 &&
        ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) &&
        path[1] == L':')
        return path.size() < 3 || (path[2] != L'\\' && path[2] != L'/');
    return path.front() != L'\\' && path.front() != L'/';
}

std::wstring ResolveRelativeIconPath(
    std::wstring_view shortcutPath, std::wstring iconPath)
{
    if (iconPath.empty() || !IsRelativeIconPath(iconPath))
        return iconPath;

    const size_t separator = shortcutPath.find_last_of(L"\\/");
    if (separator == std::wstring_view::npos)
        return iconPath;
    return std::wstring(shortcutPath.substr(0, separator + 1)) + iconPath;
}

int ParseIconIndex(std::wstring_view value)
{
    value = Trim(value);
    if (value.empty())
        return 0;

    const std::wstring text(value);
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str())
        return 0;
    while (*end == L' ' || *end == L'\t' || *end == L'\r' ||
        *end == L'\n')
        ++end;
    if (*end != L'\0' || parsed < INT_MIN || parsed > INT_MAX)
        return 0;
    return static_cast<int>(parsed);
}

// MS-SHLLINK sections 2.1-2.5. This is only an icon hint reader: it does not
// resolve targets, track moves, interpret PIDLs, or activate advertised links.
// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-shllink/17b69472-0f34-4bcf-b290-eccdb8de224b
struct LinkBytes
{
    std::span<const std::uint8_t> data;
    bool Has(size_t offset, size_t count) const
    { return offset <= data.size() && count <= data.size() - offset; }
    std::uint32_t U32(size_t offset) const
    {
        return static_cast<std::uint32_t>(data[offset]) |
            (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
            (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
            (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    }
    unsigned U16(size_t offset) const
    { return data[offset] | (static_cast<unsigned>(data[offset + 1]) << 8); }
    bool Text(size_t offset, size_t characters, bool unicode, std::wstring& value) const
    {
        if (!Has(offset, characters * (unicode ? 2 : 1))) return false;
        value.clear();
        if (unicode)
        {
            for (size_t i = 0; i < characters; ++i)
                value.push_back(static_cast<wchar_t>(U16(offset + i * 2)));
        }
        else if (characters)
        {
            const auto* bytes = reinterpret_cast<const char*>(data.data() + offset);
            const int count = static_cast<int>(characters);
            const int needed = MultiByteToWideChar(CP_ACP, 0, bytes, count, nullptr, 0);
            if (!needed) return false;
            value.resize(needed);
            if (!MultiByteToWideChar(CP_ACP, 0, bytes, count, value.data(), needed)) return false;
        }
        return value.find(L'\0') == std::wstring::npos;
    }
    bool Terminated(size_t offset, bool unicode, std::wstring& value) const
    {
        const size_t unit = unicode ? 2 : 1;
        for (size_t end = offset; Has(end, unit); end += unit)
            if (unicode ? U16(end) == 0 : data[end] == 0)
                return Text(offset, (end - offset) / unit, unicode, value);
        return false;
    }
};

struct LocalLinkHints
{
    std::wstring icon, target, relative;
    int index = 0;
};

bool ParseLocalLink(LinkBytes bytes, LocalLinkHints& hints)
{
    constexpr std::array<std::uint8_t, 16> clsid{
        0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (!bytes.Has(0, 0x4c) || bytes.U32(0) != 0x4c ||
        std::memcmp(bytes.data.data() + 4, clsid.data(), clsid.size()) != 0) return false;
    const auto flags = bytes.U32(20);
    hints.index = std::bit_cast<std::int32_t>(bytes.U32(56));
    size_t position = 0x4c;
    if (flags & 1) // Skip, never deserialize the target IDList through Shell.
    {
        if (!bytes.Has(position, 2)) return false;
        const size_t size = bytes.U16(position);
        position += 2;
        if (!bytes.Has(position, size)) return false;
        position += size;
    }
    if (flags & 2)
    {
        if (!bytes.Has(position, 4)) return false;
        const auto size = bytes.U32(position);
        if (size < 0x1c || !bytes.Has(position, size)) return false;
        LinkBytes info{bytes.data.subspan(position, size)};
        const auto header = info.U32(4);
        if ((header != 0x1c && header < 0x24) || header > size) return false;
        if (!(flags & 0x100) && (info.U32(8) & 1)) // ForceNoLinkInfo wins.
        {
            const bool unicode = header >= 0x24 && info.U32(28) && info.U32(32);
            const auto base = info.U32(unicode ? 28 : 16);
            const auto suffix = info.U32(unicode ? 32 : 24);
            std::wstring basePath, suffixPath;
            if (base < header || suffix < header ||
                !info.Terminated(base, unicode, basePath) ||
                !info.Terminated(suffix, unicode, suffixPath)) return false;
            hints.target = basePath;
            if (!suffixPath.empty())
            {
                if (!hints.target.empty() && hints.target.back() != L'\\' &&
                    hints.target.back() != L'/' && suffixPath.front() != L'\\' &&
                    suffixPath.front() != L'/') hints.target += L'\\';
                hints.target += suffixPath;
            }
        }
        position += size;
    }
    for (unsigned bit = 2; bit <= 6; ++bit)
    {
        if (!(flags & (1u << bit))) continue;
        if (!bytes.Has(position, 2)) return false;
        const size_t characters = bytes.U16(position);
        position += 2;
        std::wstring text;
        if (!bytes.Text(position, characters, (flags & 0x80) != 0, text)) return false;
        position += characters * ((flags & 0x80) ? 2 : 1);
        if (bit == 3) hints.relative = std::move(text);
        if (bit == 6) hints.icon = std::move(text);
    }
    bool terminal = false;
    while (bytes.Has(position, 4))
    {
        const auto size = bytes.U32(position);
        if (size < 4) { terminal = true; break; }
        if (size < 8 || !bytes.Has(position, size)) return false;
        LinkBytes block{bytes.data.subspan(position, size)};
        const auto signature = block.U32(4);
        const bool icon = signature == 0xa0000007 && (flags & 0x4000);
        const bool target = signature == 0xa0000001 && (flags & 0x200) && !(flags & 0x100000);
        if (icon || target)
        {
            if (size != 0x314) return false;
            std::wstring value;
            LinkBytes wide{block.data.subspan(268, 520)};
            LinkBytes ansi{block.data.subspan(8, 260)};
            if (!wide.Terminated(0, true, value)) return false;
            if (value.empty() && !ansi.Terminated(0, false, value)) return false;
            if (icon && !value.empty()) hints.icon = std::move(value);
            if (target && ((flags & 0x2000000) || hints.target.empty()))
                hints.target = std::move(value);
        }
        position += size;
    }
    return terminal;
}

// Reject network drives, device/ADS paths and reparse/offline ancestors before
// opening a file. This is a conservative performance filter, not a security
// boundary against concurrent filesystem changes or slow local filter drivers.
std::wstring LocalFile(std::wstring path)
{
    if (path.size() < 3 || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/') ||
        path.find(L':', 2) != std::wstring::npos || path.find(L'\0') != std::wstring::npos)
        return {};
    std::array<wchar_t, 32768> normalized{};
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(normalized.size()),
        normalized.data(), nullptr);
    if (!length || length >= normalized.size()) return {};
    path.assign(normalized.data(), length);
    const std::wstring drive = path.substr(0, 3);
    if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) return {};
    for (size_t end = 3; end <= path.size(); ++end)
    {
        if (end != path.size() && path[end] != L'\\') continue;
        const auto part = path.substr(0, end);
        const DWORD attrs = GetFileAttributesW(part.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES ||
            (attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
                FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS))) return {};
        if (end == path.size() && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return {};
    }
    return path;
}

std::vector<std::uint8_t> ReadLinkFile(const std::wstring& path)
{
    struct File
    {
        HANDLE handle;
        ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    } file{CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.handle == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.handle, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE |
            FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) ||
        info.nFileSizeHigh || info.nFileSizeLow < 0x4c || info.nFileSizeLow > 1024 * 1024)
        return {};
    std::vector<std::uint8_t> bytes(info.nFileSizeLow);
    DWORD read = 0;
    if (!ReadFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) return {};
    return bytes;
}
} // namespace

std::optional<IconResourceLocation> ReadInternetShortcutIconResource(
    std::wstring_view shortcutPath)
{
    if (shortcutPath.empty())
        return std::nullopt;

    const std::wstring shortcut(shortcutPath);
    std::vector<wchar_t> iconFile(32768, L'\0');
    if (GetPrivateProfileStringW(L"InternetShortcut", L"IconFile", L"",
            iconFile.data(), static_cast<DWORD>(iconFile.size()),
            shortcut.c_str()) == 0)
        return std::nullopt;

    std::wstring resourcePath = ResolveRelativeIconPath(shortcutPath,
        ExpandIconPath(iconFile.data()));
    if (resourcePath.empty())
        return std::nullopt;

    wchar_t iconIndex[64]{};
    GetPrivateProfileStringW(L"InternetShortcut", L"IconIndex", L"0",
        iconIndex, static_cast<DWORD>(std::size(iconIndex)),
        shortcut.c_str());
    return IconResourceLocation{
        std::move(resourcePath), ParseIconIndex(iconIndex) };
}
std::vector<IconResourceLocation> ReadShortcutIconResources(
    std::wstring_view shortcutPath)
{
    namespace rules = shortcut_application_rules;
    std::vector<IconResourceLocation> result;
    const std::wstring path(shortcutPath);
    if (rules::HasExtension(path, L".url"))
    {
        if (auto icon = ReadInternetShortcutIconResource(path))
            result.push_back(std::move(*icon));
        wchar_t url[32768]{};
        GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url,
            static_cast<DWORD>(std::size(url)), path.c_str());
        const wchar_t* protocol = rules::StartsWithIgnoreCase(url, L"https://") ? L"https" :
            rules::StartsWithIgnoreCase(url, L"http://") ? L"http" : nullptr;
        if (protocol)
        {
            wchar_t location[32768]{};
            DWORD size = static_cast<DWORD>(std::size(location));
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_DEFAULTICON,
                    protocol, nullptr, location, &size)) && location[0])
            {
                const int index = PathParseIconLocationW(location);
                result.push_back({ExpandIconPath(location), index});
            }
            size = static_cast<DWORD>(std::size(location));
            if (SUCCEEDED(AssocQueryStringW(ASSOCF_IS_PROTOCOL, ASSOCSTR_EXECUTABLE,
                    protocol, L"open", location, &size)) && location[0])
                result.push_back({ExpandIconPath(location), 0});
        }
    }
    else if (rules::HasExtension(path, L".lnk"))
    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> file;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) ||
            FAILED(file->Load(path.c_str(), STGM_READ))) return result;
        wchar_t location[32768]{};
        int index = 0;
        if (SUCCEEDED(link->GetIconLocation(location, static_cast<int>(std::size(location)), &index)) && location[0])
            result.push_back({ResolveRelativeIconPath(path, ExpandIconPath(location)), index});
        if (SUCCEEDED(link->GetPath(location, static_cast<int>(std::size(location)), nullptr, SLGP_RAWPATH)) && location[0])
            result.push_back({ExpandIconPath(location), 0});
    }
    return result;
}

std::vector<IconResourceLocation> ReadLocalIconResources(std::wstring_view sourcePath)
{
    namespace rules = shortcut_application_rules;
    std::vector<IconResourceLocation> result;
    // Do not touch arbitrary document/folder paths on the local image lane.
    const bool link = rules::HasExtension(sourcePath, L".lnk");
    const bool url = rules::HasExtension(sourcePath, L".url");
    if (!link && !url && !rules::HasExtension(sourcePath, L".exe") &&
        !rules::HasExtension(sourcePath, L".ico")) return result;
    const auto path = LocalFile(std::wstring(sourcePath));
    if (path.empty()) return result;
    const auto add = [&](std::wstring value, int index, bool target) {
        value = ResolveRelativeIconPath(path, ExpandIconPath(value));
        if (target && !rules::HasExtension(value, L".exe") &&
            !rules::HasExtension(value, L".ico")) return;
        value = LocalFile(std::move(value));
        if (!value.empty()) result.push_back({std::move(value), index});
    };
    if (link)
    {
        const auto bytes = ReadLinkFile(path);
        LocalLinkHints hints;
        if (!ParseLocalLink({bytes}, hints)) return result;
        if (!hints.icon.empty()) add(std::move(hints.icon), hints.index, false);
        if (!hints.target.empty()) add(std::move(hints.target), 0, true);
        else if (!hints.relative.empty()) add(std::move(hints.relative), 0, true);
    }
    else if (url)
    {
        if (auto icon = ReadInternetShortcutIconResource(path))
            add(std::move(icon->path), icon->index, false);
    }
    else
        result.push_back({path, 0});
    return result;
}
} // namespace snowdesktop::shortcut_icon_resource
