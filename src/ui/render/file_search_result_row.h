#pragma once

#include "common/l10n.h"
#include <windows.h>
#include <algorithm>
#include <string>

namespace snowdesktop::file_search_result_row
{
struct Metrics
{
    int icon, inset, textGap, nameTop, nameHeight, bottom, dateMin, dateMax, dateGap;
};
struct Geometry { RECT icon, name, path, modified; };
inline Geometry Resolve(RECT row, const Metrics& m, bool hasModified)
{
    Geometry result{};
    result.icon = {row.left + m.inset, (row.top + row.bottom - m.icon) / 2,
        row.left + m.inset + m.icon, (row.top + row.bottom + m.icon) / 2};
    result.name = {result.icon.right + m.textGap, row.top + m.nameTop,
        row.right - m.inset, row.top + m.nameTop + m.nameHeight};
    result.path = {result.name.left, result.name.bottom, result.name.right, row.bottom - m.bottom};
    if (hasModified)
    {
        const int available = std::max<LONG>(1, result.path.right - result.path.left);
        const int maximum = std::min(m.dateMax, available);
        const int width = std::clamp(available / 3, std::min(m.dateMin, maximum), maximum);
        result.modified = result.path;
        result.modified.left = std::max<LONG>(result.modified.left, result.modified.right - width);
        result.path.right = std::max<LONG>(result.path.left, result.modified.left - m.dateGap);
    }
    return result;
}
inline std::wstring ModifiedTime(const FILETIME& value)
{
    if (value.dwLowDateTime == 0 && value.dwHighDateTime == 0) return {};
    FILETIME local{}; SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&value, &local) || !FileTimeToSystemTime(&local, &time)) return {};
    const auto pad = [](unsigned number, size_t width) {
        auto text = std::to_wstring(number);
        if (text.size() < width) text.insert(text.begin(), width - text.size(), L'0');
        return text;
    };
    return _LFW("app.nav.file_modified", pad(time.wYear, 4), pad(time.wMonth, 2),
        pad(time.wDay, 2), pad(time.wHour, 2), pad(time.wMinute, 2));
}
}
