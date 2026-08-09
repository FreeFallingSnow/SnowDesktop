#include "tray_icon_controller.h"

#include "../constants.h"
#include "../resource.h"

#include <shellapi.h>

TrayIconController::~TrayIconController()
{
    Remove();
    if (icon_)
        DestroyIcon(icon_);
}

bool TrayIconController::Add(HWND owner, bool force)
{
    if (!owner || !IsWindow(owner))
        return false;
    if (added_ && !force)
        return true;

    if (force)
        Remove(owner);

    if (!icon_)
    {
        icon_ = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_APPICON_SMALL),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            0));
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"SparkDesktop");
    if (!Shell_NotifyIconW(NIM_ADD, &data))
        return false;

    added_ = true;
    owner_ = owner;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void TrayIconController::Remove(HWND fallbackOwner)
{
    if (!added_)
        return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_ ? owner_ : fallbackOwner;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
    owner_ = nullptr;
}

bool TrayIconController::ShowBalloon(
    HWND owner,
    const std::wstring& title,
    const std::wstring& message)
{
    if (!owner || !IsWindow(owner) || !Add(owner))
        return false;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    data.uTimeout = 10000;
    return Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
}

TrayCallbackAction TrayIconController::ClassifyCallback(
    LPARAM value)
{
    switch (LOWORD(value))
    {
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
        return TrayCallbackAction::ShowContextMenu;
    case WM_LBUTTONDBLCLK:
        return TrayCallbackAction::ReloadItems;
    default:
        return TrayCallbackAction::None;
    }
}
