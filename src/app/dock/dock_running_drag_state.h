#pragma once

#include "app/app.h"

// Shared only by the Dock application's gesture/queued-commit implementation
// and the windowless native commit fixture. This is not a public component API.
struct DesktopApp::DockRunningDragState
{
    DockRunningAppInfo running;
    std::unique_ptr<DockRunningItem> visual;
    std::shared_ptr<DockRunningPinTarget> application;
    std::shared_ptr<DockRunningPinTarget> folder;
    bool explorer = false;
    bool existingDesktopItem = false;
    bool resolved = false;
    std::wstring desktopFolderKey;
    std::uint64_t serial = 0;
    snowdesktop::dock_running_drag::Target target;
};
