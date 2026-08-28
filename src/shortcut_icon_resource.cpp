#include "shortcut_icon_resource.h"

#include <windows.h>

#include <climits>
#include <cwchar>
#include <iterator>
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
} // namespace snowdesktop::shortcut_icon_resource
