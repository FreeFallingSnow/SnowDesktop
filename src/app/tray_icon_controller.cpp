#include "tray_icon_controller.h"
#include "tray_notification_window.h"

#include "../constants.h"
#include "../resource.h"

#include <shellapi.h>

namespace
{
LRESULT CALLBACK TrayNotificationWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == kTrayCallbackMessage)
    {
        const HWND callbackWindow = reinterpret_cast<HWND>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (callbackWindow && IsWindow(callbackWindow))
            PostMessageW(callbackWindow, message, wParam, lParam);
        return 0;
    }
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wParam, lParam);
}
}

HWND snowdesktop::tray_notification::CreateOwnerWindow(HWND callbackWindow)
{
    if (!callbackWindow || !IsWindow(callbackWindow))
        return nullptr;
    WNDCLASSW type{};
    type.lpfnWndProc = TrayNotificationWindowProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"SnowDesktopTrayNotificationWindow";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    // Do not make the internal control window this window's owner: Shell may
    // use the root owner's caption when choosing the visible source name.
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"SnowDesktop", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, type.hInstance, callbackWindow);
}

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

    owner_ = snowdesktop::tray_notification::CreateOwnerWindow(owner);
    if (!owner_)
        return false;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = icon_;
    wcscpy_s(data.szTip, L"SnowDesktop");
    if (!Shell_NotifyIconW(NIM_ADD, &data))
    {
        Remove();
        return false;
    }

    added_ = true;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void TrayIconController::Remove(HWND)
{
    if (added_)
    {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    if (owner_)
        DestroyWindow(owner_);
    added_ = false;
    owner_ = nullptr;
    activeNotificationId_.clear();
}

bool TrayIconController::ShowBalloon(
    HWND owner,
    const std::string& notificationId,
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
    const bool shown = Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
    if (shown) activeNotificationId_ = notificationId;
    return shown;
}

bool TrayIconController::UpdateBalloon(
    HWND owner,
    const std::string& notificationId,
    const std::wstring& title,
    const std::wstring& message)
{
    // A tray balloon has no independently addressable update primitive.
    // Reissuing the same host notification ID replaces the visible balloon.
    return ShowBalloon(owner, notificationId, title, message);
}

bool TrayIconController::DismissBalloon(
    HWND owner,
    const std::string& notificationId)
{
    if (!notificationId.empty() &&
        notificationId != activeNotificationId_)
        return true;
    if (!owner || !IsWindow(owner) || !Add(owner))
        return false;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.szInfo[0] = L'\0';
    data.szInfoTitle[0] = L'\0';
    const bool dismissed =
        Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE;
    if (dismissed) activeNotificationId_.clear();
    return dismissed;
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
