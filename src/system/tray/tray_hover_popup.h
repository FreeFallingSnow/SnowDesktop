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

    void Watch(HWND receiver, HWND target, DWORD process, POINT anchor);
    void Reset();
    bool Visible() const;

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    bool IsNearAnchor(HWND window) const;

    HWND receiver_ = nullptr;
    HWND target_ = nullptr;
    DWORD process_ = 0;
    DWORD started_ = 0;
    RECT vicinity_{};
    HWINEVENTHOOK hook_ = nullptr;
    std::array<HWND, 4> shown_{};
};
}
