#pragma once
#include "core/widget.h"
#include "system/tray/tray_service.h"

// Transient content in the existing Dock popup, never saved to the layout.
struct DockTrayPopupState
{
    DesktopWidget widget;
    std::wstring executable, itemKey, launchTarget;
    RECT anchorScreen{};
    std::vector<snowdesktop::tray::Icon> targets;
    std::vector<HBITMAP> images;
    std::vector<int> rowHeights;
    std::optional<snowdesktop::tray::Icon> pressedTarget;
    bool pointerDown = false, pressedLaunch = false, context = false;
    int hovered = -1, focused = -1;
    ~DockTrayPopupState() { for (auto bitmap : images) if (bitmap) DeleteObject(bitmap); }
};
