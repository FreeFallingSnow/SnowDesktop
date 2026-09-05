#pragma once

#include <algorithm>
#include <limits>
#include <windows.h>

namespace snowdesktop::rename_edit_layout
{
enum class HeightAnchor { Top, Center, Bottom };

// Start hidden so the initial full-name measurement precedes the first paint.
inline DWORD EditStyle(bool leftAligned = false)
{
    return WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
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

    void Begin(HWND edit, HeightAnchor heightAnchor = HeightAnchor::Top)
    {
        Reset();
        if (!edit || !GetWindowRect(edit, &anchor_))
            return;
        edit_ = edit;
        heightAnchor_ = heightAnchor;
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

        // Width must be final before asking EDIT for its wrapped line count.
        const RECT initial = CalculateRect(anchor_, workArea_,
            anchor_.bottom - anchor_.top, heightAnchor_);
        updating_ = true;
        Position(initial);
        updating_ = false;
        Update(edit);
    }

    // Called on EN_UPDATE: EDIT has already wrapped the text, but has not
    // painted it yet. This covers typing, paste, undo and IME changes alike.
    void Update(HWND edit)
    {
        if (!edit || edit != edit_ || updating_)
            return;
        RECT window{}, client{}, formatting{};
        if (!GetWindowRect(edit, &window) || !GetClientRect(edit, &client))
            return;
        SendMessageW(edit, EM_GETRECT, 0,
            reinterpret_cast<LPARAM>(&formatting));
        const HDC dc = GetDC(edit);
        if (!dc)
            return;
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
        const HGDIOBJ previous = SelectObject(dc,
            font ? font : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW metrics{};
        const bool measured = GetTextMetricsW(dc, &metrics) != FALSE;
        SelectObject(dc, previous);
        ReleaseDC(edit, dc);
        if (!measured)
            return;

        const auto lineCount = std::max<LRESULT>(1,
            SendMessageW(edit, EM_GETLINECOUNT, 0, 0));
        const int borderHeight = (window.bottom - window.top) -
            (client.bottom - client.top);
        // EDIT's formatting bottom can exclude a partial line; use the top
        // inset symmetrically instead of treating that remainder as padding.
        const int padding = 2 * std::max<int>(1, formatting.top - client.top);
        const auto textHeight = static_cast<long long>(lineCount) *
            std::max<LONG>(1, metrics.tmHeight) + borderHeight + padding;
        const int desiredHeight = static_cast<int>(std::clamp<long long>(
            textHeight, anchor_.bottom - anchor_.top,
            std::numeric_limits<int>::max()));
        const RECT next = CalculateRect(anchor_, workArea_,
            desiredHeight, heightAnchor_);
        if (EqualRect(&window, &next))
            return;

        updating_ = true;
        Position(next);
        if (textHeight <= next.bottom - next.top)
        {
            // A formerly scrolled long name must show its first line once all
            // lines fit again. Keep the selection and undo history untouched.
            const LRESULT first = SendMessageW(edit, EM_GETFIRSTVISIBLELINE, 0, 0);
            if (first > 0)
                SendMessageW(edit, EM_LINESCROLL, 0, -first);
        }
        else
            SendMessageW(edit, EM_SCROLLCARET, 0, 0);
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
};
} // namespace snowdesktop::rename_edit_layout
