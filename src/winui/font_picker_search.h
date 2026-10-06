#pragma once

#include "ui/render/app_font.h"

#include <windows.h>
#include <limits>

namespace snowdesktop::winui::font_picker
{
inline bool Contains(std::wstring_view name, std::wstring_view query)
{
    if (query.empty()) return true;
    if (name.size() < query.size() || name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
    return FindStringOrdinal(FIND_FROMSTART, name.data(), static_cast<int>(name.size()),
        query.data(), static_cast<int>(query.size()), TRUE) >= 0;
}

// Keep indices into the source catalogue: equal display names can belong to
// different packages, and a filtered list must still select the right family.
inline std::vector<std::size_t> Filter(const std::vector<app_fonts::Choice>& fonts,
    std::wstring_view query, std::wstring_view systemName)
{
    const auto first = query.find_first_not_of(L" \t\r\n\u3000");
    query = first == std::wstring_view::npos ? std::wstring_view{}
        : query.substr(first, query.find_last_not_of(L" \t\r\n\u3000") - first + 1);
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < fonts.size(); ++i)
    {
        const auto& choice = fonts[i];
        if (Contains(choice.name, query) ||
            (choice.selection.package == "system" && Contains(systemName, query)))
        {
            result.push_back(i);
            continue;
        }
        const auto& utf8 = choice.selection.family;
        const int size = static_cast<int>(utf8.size());
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, nullptr, 0);
        if (length <= 0) continue;
        std::wstring canonical(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), size, canonical.data(), length);
        if (Contains(canonical, query)) result.push_back(i);
    }
    return result;
}
} // namespace snowdesktop::winui::font_picker
