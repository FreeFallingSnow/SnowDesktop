#pragma once

#include <windows.h>
#include <array>

namespace snowdesktop::tray
{
UINT HoverPopupChangedMessage();

// UI-thread observer for a single hovered tray icon.
class HoverPopup
{
public:
    HoverPopup() = default;
    ~HoverPopup();
    HoverPopup(const HoverPopup&) = delete;
    HoverPopup& operator=(const HoverPopup&) = delete;

    void Watch(HWND receiver, HWND target, DWORD process, RECT iconBounds, RECT row, bool preferAbove, bool firstShowArmed);
    void Reset();
    bool Visible() const;
    bool Covers(HWND window) const;

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    bool IsCandidate(HWND window) const;
    bool IsNearAnchor(HWND window) const;
    struct Popup
    {
        HWND window = nullptr;
        bool positionRequested = false;
    };
    void AvoidRow(Popup& popup, const RECT& bounds) const;
    HWND receiver_ = nullptr;
    HWND target_ = nullptr;
    DWORD process_ = 0;
    DWORD started_ = 0;
    RECT vicinity_{};
    RECT row_{};
    RECT workArea_{};
    LONG gap_ = 0;
    bool preferAbove_ = false;
    bool firstShowArmed_ = false;
    HWINEVENTHOOK hook_ = nullptr;
    std::array<Popup, 4> shown_{};
};
}
