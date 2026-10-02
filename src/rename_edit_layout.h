#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <windows.h>
#include "text_input_window.h"

namespace snowdesktop::rename_edit_layout
{
enum class HeightAnchor { Top, Center, Bottom };

// Grid labels wrap within their original title region; rows and widget titles
// keep a single-line viewport with horizontal scrolling.
inline DWORD EditStyle(bool leftAligned = false, bool multiline = false)
{
    return WS_POPUP | (multiline ? ES_MULTILINE | ES_AUTOVSCROLL : ES_AUTOHSCROLL) |
        (leftAligned ? ES_LEFT : ES_CENTER);
}

inline RECT CalculateRect(const RECT& anchor, const RECT& workArea,
    int desiredHeight, HeightAnchor heightAnchor = HeightAnchor::Top)
{
    const int width = std::clamp<int>(anchor.right - anchor.left,
        1, std::max<int>(1, workArea.right - workArea.left));
    const int height = std::clamp(desiredHeight,
        1, std::max<int>(1, workArea.bottom - workArea.top));
    int top = anchor.top;
    if (heightAnchor == HeightAnchor::Bottom)
        top = anchor.bottom - height;
    else if (heightAnchor == HeightAnchor::Center)
        top = anchor.top + (anchor.bottom - anchor.top - height) / 2;
    const int left = std::clamp<int>(anchor.left, workArea.left,
        std::max<int>(workArea.left, workArea.right - width));
    top = std::clamp<int>(top, workArea.top,
        std::max<int>(workArea.top, workArea.bottom - height));
    return { left, top, left + width, top + height };
}

class EditorLayout
{
public:
    void Reset()
    {
        edit_ = nullptr;
        updating_ = false;
    }

    void Begin(HWND edit, HeightAnchor heightAnchor = HeightAnchor::Center)
    {
        Reset();
        if (!edit || !GetWindowRect(edit, &anchor_))
            return;
        edit_ = edit;
        multiline_ = (GetWindowLongPtrW(edit, GWL_STYLE) & ES_MULTILINE) != 0;
        heightAnchor_ = multiline_ ? HeightAnchor::Top : heightAnchor;
        // Filenames remain one logical string even when the grid editor wraps.
        text_input::SetLogicalSingleLine(edit, true);
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (GetMonitorInfoW(MonitorFromRect(&anchor_,
                MONITOR_DEFAULTTONEAREST), &monitorInfo))
            workArea_ = monitorInfo.rcWork;
        else if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea_, 0))
            workArea_ = anchor_;
        const int margin = std::max(1,
            MulDiv(4, static_cast<int>(GetDpiForWindow(edit)), 96));
        if (workArea_.right - workArea_.left > margin * 2 &&
            workArea_.bottom - workArea_.top > margin * 2)
            InflateRect(&workArea_, -margin, -margin);

        // Preserve the full grid title region before adapting to longer names.
        const RECT initial = CalculateRect(anchor_, workArea_,
            anchor_.bottom - anchor_.top, heightAnchor_);
        updating_ = true;
        Position(initial);
        updating_ = false;
        Update(edit);
    }

    // Rows keep one line; wrapped grids never shrink below their title region.
    void Update(HWND edit)
    {
        if (!edit || edit != edit_ || updating_) return;
        const int desiredHeight = text_input::DesiredHeight(edit);
        const RECT next = CalculateRect(anchor_, workArea_,
            multiline_ ? std::max<int>(anchor_.bottom - anchor_.top, desiredHeight) : desiredHeight,
            heightAnchor_);
        updating_ = true;
        Position(next);
        updating_ = false;
    }
private:
    void Position(const RECT& rect)
    {
        SetWindowPos(edit_, nullptr, rect.left, rect.top,
            rect.right - rect.left, rect.bottom - rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }

    HWND edit_ = nullptr;
    RECT anchor_{};
    RECT workArea_{};
    HeightAnchor heightAnchor_ = HeightAnchor::Top;
    bool updating_ = false;
    bool multiline_ = false;
};
} // namespace snowdesktop::rename_edit_layout
