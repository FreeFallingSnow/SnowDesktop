#pragma once
#include <windows.h>
#include <algorithm>
#include <iterator>

namespace snowdesktop::modern_menu::dismissal_rules
{
template<class Popups, class Bounds>
bool OutsidePress(UINT message, POINT point, const Popups& popups, const Bounds& bounds)
{
    const bool press = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN ||
        message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN ||
        message == WM_NCMBUTTONDOWN || message == WM_NCXBUTTONDOWN;
    return press && std::begin(popups) != std::end(popups) &&
        std::none_of(std::begin(popups),std::end(popups),[&](const auto& popup) {
            const RECT rect = bounds(popup);
            return point.x >= rect.left && point.x < rect.right &&
                point.y >= rect.top && point.y < rect.bottom;
        });
}
}
