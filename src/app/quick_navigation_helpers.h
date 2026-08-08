#pragma once

#include "../l10n.h"

#include <imm.h>
#include <windows.h>

#include <string>

inline constexpr size_t kQuickNavigationAppResultLimit = 80;
inline constexpr size_t kQuickNavigationAppCollapsedResultCount = 5;

inline std::wstring QuickNavigationReadImeCompositionString(HWND hwnd)
{
    HIMC context = ImmGetContext(hwnd);
    if (!context)
        return {};

    std::wstring result;
    const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    if (bytes > 0)
    {
        result.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
        ImmGetCompositionStringW(context, GCS_COMPSTR, result.data(), bytes);
        while (!result.empty() && result.back() == L'\0')
            result.pop_back();
    }

    ImmReleaseContext(hwnd, context);
    return result;
}

inline int QuickNavigationRowsHeight(int rows, int cellHeight, int rowGap)
{
    if (rows <= 0)
        return 0;
    return rows * cellHeight + (rows - 1) * rowGap;
}

inline bool QuickNavigationHasFileTime(const FILETIME& value)
{
    return value.dwLowDateTime != 0 || value.dwHighDateTime != 0;
}

inline std::wstring QuickNavigationFormatModifiedTime(const FILETIME& value)
{
    if (!QuickNavigationHasFileTime(value))
        return {};

    FILETIME localTime{};
    SYSTEMTIME systemTime{};
    if (!FileTimeToLocalFileTime(&value, &localTime) ||
        !FileTimeToSystemTime(&localTime, &systemTime))
        return {};

    auto padNumber = [](unsigned value, size_t width) {
        std::wstring text = std::to_wstring(value);
        if (text.size() < width)
            text.insert(text.begin(), width - text.size(), L'0');
        return text;
    };
    return _LFW("app.nav.file_modified",
        padNumber(static_cast<unsigned>(systemTime.wYear), 4),
        padNumber(static_cast<unsigned>(systemTime.wMonth), 2),
        padNumber(static_cast<unsigned>(systemTime.wDay), 2),
        padNumber(static_cast<unsigned>(systemTime.wHour), 2),
        padNumber(static_cast<unsigned>(systemTime.wMinute), 2));
}
