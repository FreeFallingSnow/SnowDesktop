#pragma once

#include <array>
#include <string_view>

namespace snowdesktop::shortcut_application_rules
{
inline wchar_t UpperAscii(wchar_t value)
{
    return value >= L'a' && value <= L'z'
        ? static_cast<wchar_t>(value - L'a' + L'A')
        : value;
}

inline std::wstring_view Trim(std::wstring_view value)
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

inline bool EqualsIgnoreCase(
    std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size())
        return false;
    for (size_t index = 0; index < left.size(); ++index)
    {
        if (UpperAscii(left[index]) != UpperAscii(right[index]))
            return false;
    }
    return true;
}

inline bool StartsWithIgnoreCase(
    std::wstring_view value, std::wstring_view prefix)
{
    return value.size() >= prefix.size() &&
        EqualsIgnoreCase(value.substr(0, prefix.size()), prefix);
}

inline bool ContainsIgnoreCase(
    std::wstring_view value, std::wstring_view token)
{
    if (token.empty())
        return true;
    if (value.size() < token.size())
        return false;
    for (size_t offset = 0; offset + token.size() <= value.size(); ++offset)
    {
        if (EqualsIgnoreCase(value.substr(offset, token.size()), token))
            return true;
    }
    return false;
}

inline bool HasExtension(
    std::wstring_view path, std::wstring_view extension)
{
    path = Trim(path);
    return path.size() >= extension.size() &&
        EqualsIgnoreCase(path.substr(path.size() - extension.size()), extension);
}

inline bool LooksLikeApplicationsParsingName(std::wstring_view value)
{
    value = Trim(value);
    return ContainsIgnoreCase(value, L"SHELL:APPSFOLDER") ||
        ContainsIgnoreCase(value, L"APPSFOLDER\\") ||
        ContainsIgnoreCase(
            value, L"{4234D49B-0245-4DF3-B780-3893943456E1}");
}

/**
 * Shell thumbnail providers can wrap application artwork in a neutral frame
 * at large requested sizes. Keep icon-bearing application entries on the
 * ICONONLY path while allowing ordinary documents and media to use previews.
 */
inline bool ShouldUseShellIconOnly(std::wstring_view parsingName)
{
    parsingName = Trim(parsingName);
    if (LooksLikeApplicationsParsingName(parsingName))
        return true;

    constexpr std::array iconOnlyExtensions{
        std::wstring_view(L".EXE"),
        std::wstring_view(L".COM"),
        std::wstring_view(L".BAT"),
        std::wstring_view(L".CMD"),
        std::wstring_view(L".CPL"),
        std::wstring_view(L".MSC"),
        std::wstring_view(L".SCR"),
        std::wstring_view(L".LNK"),
        std::wstring_view(L".URL"),
        std::wstring_view(L".APPREF-MS"),
        std::wstring_view(L".APPLICATION"),
    };
    for (const std::wstring_view extension : iconOnlyExtensions)
    {
        if (HasExtension(parsingName, extension))
            return true;
    }
    return false;
}

inline bool LooksLikeAppUserModelId(std::wstring_view value)
{
    value = Trim(value);
    const size_t separator = value.find(L'!');
    if (separator == std::wstring_view::npos || separator == 0 ||
        separator + 1 >= value.size())
        return false;
    return value.find_first_of(L"\\/:") == std::wstring_view::npos;
}

inline bool IsExplorerExecutable(std::wstring_view path)
{
    path = Trim(path);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator != std::wstring_view::npos)
        path.remove_prefix(separator + 1);
    return EqualsIgnoreCase(path, L"EXPLORER.EXE");
}

inline bool IsApplicationsShellLinkTarget(
    std::wstring_view resolvedPath,
    std::wstring_view arguments,
    std::wstring_view appUserModelId,
    std::wstring_view targetParsingPath,
    bool applicationsPidlTarget)
{
    if (applicationsPidlTarget || !Trim(appUserModelId).empty())
        return true;
    if (LooksLikeApplicationsParsingName(targetParsingPath))
        return true;
    if (Trim(resolvedPath).empty() &&
        LooksLikeAppUserModelId(targetParsingPath))
        return true;
    return IsExplorerExecutable(resolvedPath) &&
        LooksLikeApplicationsParsingName(arguments);
}

inline bool IsSteamApplicationUrl(std::wstring_view url)
{
    url = Trim(url);
    constexpr std::array prefixes{
        std::wstring_view(L"STEAM://RUNGAMEID/"),
        std::wstring_view(L"STEAM://RUN/"),
    };
    for (const std::wstring_view prefix : prefixes)
    {
        if (!StartsWithIgnoreCase(url, prefix))
            continue;
        size_t cursor = prefix.size();
        const size_t firstDigit = cursor;
        while (cursor < url.size() &&
            url[cursor] >= L'0' && url[cursor] <= L'9')
            ++cursor;
        if (cursor == firstDigit)
            return false;
        if (cursor == url.size())
            return true;
        const wchar_t suffix = url[cursor];
        return suffix == L'/' || suffix == L'?' || suffix == L'#' ||
            suffix == L' ' || suffix == L'\t' || suffix == L'\r' ||
            suffix == L'\n';
    }
    return false;
}
} // namespace snowdesktop::shortcut_application_rules
