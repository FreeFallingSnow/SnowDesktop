#pragma once
#include "ui/render/file_search_result_row.h"

#include "common/l10n.h"

#include <windows.h>
#include <imm.h>

#include <string>
#include <algorithm>
#include <cstdint>
#include <cmath>

inline constexpr size_t kQuickNavigationAppResultLimit = 80;
inline constexpr size_t kQuickNavigationAppCollapsedResultCount = 5;
inline constexpr std::uintptr_t kQuickNavigationBackdropPanelOwner = 1;

// Allocation belongs to the open window, not to an individual animation frame.
// Retain it through the settling frame; DestroyQuickNavigationWindow releases it.
inline RECT QuickNavigationRetainedAllocation(const RECT& previous, const RECT& required)
{
    if (previous.right <= previous.left || previous.bottom <= previous.top)
        return required;
    if (required.right <= required.left || required.bottom <= required.top)
        return previous;
    return {std::min(previous.left, required.left), std::min(previous.top, required.top),
        std::max(previous.right, required.right), std::max(previous.bottom, required.bottom)};
}

// Reserve the invocation monitor before the first visible frame. Retaining only
// the current panel makes the HWND origin move on the first expansion, while
// DWM can still display content whose offset belongs to the previous origin.
// This reserves native coordinates only; the content surface stays panel-sized.
inline RECT QuickNavigationHostAllocation(const RECT& previous, const RECT& required,
    const RECT& placementBounds)
{
    RECT allocation = QuickNavigationRetainedAllocation(required, placementBounds);
    InflateRect(&allocation, 2, 2);
    return QuickNavigationRetainedAllocation(previous, allocation);
}

inline RECT QuickNavigationWindowFence(const RECT& panel, const RECT& animationPath,
    const RECT& host, bool expanded)
{
    RECT fence = expanded ? QuickNavigationRetainedAllocation(panel, animationPath) : panel;
    if (expanded) InflateRect(&fence, 2, 2);
    OffsetRect(&fence, -host.left, -host.top);
    return fence;
}

inline RECT QuickNavigationSearchInputRect(RECT search, int inset)
{
    InflateRect(&search, -inset, -inset);
    search.right = std::max<LONG>(search.left + 1, search.right);
    search.bottom = std::max<LONG>(search.top + 1, search.bottom);
    return search;
}

inline float QuickNavigationItemTextLineSpacing(float fontSize)
{
    return std::max(1.0f, std::floor(fontSize * 1.08f));
}

inline int QuickNavigationItemTextHeight(float fontSize, int lines)
{
    return std::max(1, static_cast<int>(std::ceil(
        QuickNavigationItemTextLineSpacing(fontSize) * static_cast<float>(std::max(1, lines)))));
}

inline RECT QuickNavigationItemTextRect(RECT item, RECT icon, int pad, int gap, int height)
{
    const LONG top = std::max(item.top, icon.bottom + gap);
    return {item.left + pad, top, item.right - pad, std::min<LONG>(item.bottom, top + height)};
}

// Keep drawing, hover, activation and keyboard bounds clear of the row gap.
inline RECT QuickNavigationResultRowRect(RECT slot, int inset)
{
    const LONG bounded = std::min<LONG>(std::max(0, inset),
        std::max<LONG>(0, (slot.bottom - slot.top - 1) / 2));
    slot.top += bounded;
    slot.bottom -= bounded;
    return slot;
}

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

// Empty search sources contribute neither a heading nor its surrounding space.
inline int QuickNavigationSearchDesktopSectionHeight(int rows, int gridHeight,
    int headerHeight, int gap)
{
    return rows > 0 ? headerHeight + gap + gridHeight + gap : 0;
}

inline bool QuickNavigationHasFileTime(const FILETIME& value)
{
    return value.dwLowDateTime != 0 || value.dwHighDateTime != 0;
}

inline std::wstring QuickNavigationFormatModifiedTime(const FILETIME& value)
{
    return snowdesktop::file_search_result_row::ModifiedTime(value);
}
