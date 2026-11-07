#pragma once

#include "dock/dock_app_identity_rules.h"
#include "dock/dock_running_animation.h"
#include <windows.h>
#include <string>
#include <vector>

enum class DockWindowVisualState
{
    Closed,
    Running,
    Foreground,
    Minimized,
};

enum class FloatingDockCloseFocusPolicy
{
    RestorePrevious,
    PreserveCurrent,
};

struct DockAppIdentity
{
    DockAppIdentityKind kind = DockAppIdentityKind::None;
    std::wstring sourceParsingName;
    std::wstring executablePath;
    std::wstring appUserModelId;
    std::wstring steamAppId;
    std::wstring steamInstallDirectory;
    std::vector<std::wstring> validatedPayloadPaths;
};

struct DockWindowPreviewTarget
{
    DockAppIdentity identity;
    std::wstring identityKey;
    std::wstring targetToken;
    RECT anchorScreen{};
    bool floatingLayer = false;
};

struct DockWindowInfo
{
    HWND window = nullptr;
    bool minimized = false;
    bool running = false;
    bool foreground = false;
    // All identity-matched task windows, including non-representative siblings.
    std::vector<HWND> trackedWindows;
};

struct DockRunningAppInfo
{
    snowdesktop::dock_running_animation::Presence presence;
    std::wstring identityKey;
    std::wstring title;
    std::wstring executablePath;
    std::wstring appUserModelId;
    std::vector<std::wstring>
        ancestorExecutablePaths;
    HWND window = nullptr;
    HBITMAP iconBitmap = nullptr;
    SIZE iconBitmapSize{};
    int iconRequestedSize = 0;
    bool minimized = false;
    bool foreground = false;
    bool selected = false;
    std::vector<HWND> trackedWindows;
};
