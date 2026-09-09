#include "tray_icon_controller.h"
#include "tray_notification_window.h"

#include "../constants.h"
#include "../resource.h"

#include <shellapi.h>
#include <appmodel.h>
#include <propsys.h>
#include <propkey.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <filesystem>

namespace
{
HRESULT SetNotificationIdentity(HWND window, const wchar_t* applicationId)
{
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    const HRESULT result = SHGetPropertyStoreForWindow(window,
        IID_PPV_ARGS(&properties));
    if (FAILED(result))
        return result;
    PROPVARIANT value{};
    if (applicationId)
    {
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<wchar_t*>(applicationId);
    }
    // SetValue copies the string; VT_EMPTY releases the window property.
    return properties->SetValue(PKEY_AppUserModel_ID, value);
}

void RegisterPortableNotificationApplication()
{
    if (snowdesktop::tray_notification::ApplicationId() !=
        snowdesktop::tray_notification::PortableApplicationId)
        return; // MSIX display metadata belongs to the package manifest.

    HKEY applications = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\AppUserModelId", 0, nullptr, 0,
            KEY_CREATE_SUB_KEY, nullptr, &applications, nullptr) != ERROR_SUCCESS)
        return;

    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
        static_cast<DWORD>(executable.size()));
    std::wstring iconPath;
    if (length > 0 && length < executable.size())
    {
        executable.resize(length);
        const auto asset = std::filesystem::path(executable).parent_path() /
            L"Assets" / L"App" / L"SnowDesktop.png";
        std::error_code error;
        if (std::filesystem::is_regular_file(asset, error))
            iconPath = asset.wstring();

        PWSTR programs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        {
            const auto shortcut = std::filesystem::path(programs) / L"SnowDesktop.lnk";
            const HRESULT registered = snowdesktop::tray_notification::EnsureApplicationShortcut(
                shortcut.wstring(), executable);
            if (registered == S_OK)
            {
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, shortcut.c_str(), nullptr);
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
            }
            CoTaskMemFree(programs);
        }
    }
    snowdesktop::tray_notification::RegisterApplication(applications,
        snowdesktop::tray_notification::PortableApplicationId, iconPath);
    RegCloseKey(applications);
}

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
    if (message == WM_DESTROY)
        SetNotificationIdentity(window, nullptr);
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return DefWindowProcW(window, message, wParam, lParam);
}
}

std::wstring snowdesktop::tray_notification::ApplicationId()
{
    UINT32 length = 0;
    const LONG result = GetCurrentApplicationUserModelId(&length, nullptr);
    if (result == APPMODEL_ERROR_NO_APPLICATION || result == APPMODEL_ERROR_NO_PACKAGE)
        return PortableApplicationId;
    if (result != ERROR_INSUFFICIENT_BUFFER || length == 0)
        return {};
    std::wstring identity(length, L'\0');
    if (GetCurrentApplicationUserModelId(&length, identity.data()) != ERROR_SUCCESS)
        return {};
    identity.resize(length - 1);
    return identity;
}

bool snowdesktop::tray_notification::RegisterApplication(HKEY applicationsRoot,
    const std::wstring& applicationId, const std::wstring& iconPath)
{
    if (!applicationsRoot || applicationId.empty() ||
        applicationId.find_first_of(L"\\/") != std::wstring::npos)
        return false;
    HKEY application = nullptr;
    if (RegCreateKeyExW(applicationsRoot, applicationId.c_str(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &application, nullptr) != ERROR_SUCCESS)
        return false;
    constexpr wchar_t displayName[] = L"SnowDesktop";
    const LONG nameResult = RegSetValueExW(application, L"DisplayName", 0,
        REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(displayName), sizeof(displayName));
    LONG iconResult = ERROR_SUCCESS;
    if (!iconPath.empty())
        iconResult = RegSetValueExW(application, L"IconUri", 0, REG_EXPAND_SZ,
            reinterpret_cast<const BYTE*>(iconPath.c_str()),
            static_cast<DWORD>((iconPath.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(application);
    return nameResult == ERROR_SUCCESS && iconResult == ERROR_SUCCESS;
}

HRESULT snowdesktop::tray_notification::EnsureApplicationShortcut(
    const std::wstring& shortcutPath, const std::wstring& executablePath)
{
    if (shortcutPath.empty() || executablePath.empty())
        return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link));
    if (FAILED(result)) return result;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(result = link.As(&file))) return result;
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(result = link.As(&properties))) return result;

    const DWORD attributes = GetFileAttributesW(shortcutPath.c_str());
    const bool existing = attributes != INVALID_FILE_ATTRIBUTES;
    if (existing)
    {
        if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY))
            return E_ACCESSDENIED;
        if (FAILED(result = file->Load(shortcutPath.c_str(), STGM_READWRITE))) return result;
        wchar_t target[32768]{};
        if (FAILED(result = link->GetPath(target, static_cast<int>(std::size(target)),
                nullptr, SLGP_RAWPATH))) return result;
        if (CompareStringOrdinal(target, -1, executablePath.c_str(), -1, TRUE) != CSTR_EQUAL)
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        PROPVARIANT identity{};
        result = properties->GetValue(PKEY_AppUserModel_ID, &identity);
        const bool hasIdentity = SUCCEEDED(result) && identity.vt == VT_LPWSTR &&
            identity.pwszVal && identity.pwszVal[0];
        const bool matches = hasIdentity && wcscmp(identity.pwszVal, PortableApplicationId) == 0;
        // Shell exposes the executable path as the default identity even when
        // the user has not assigned an explicit AppUserModelID to the shortcut.
        const bool defaultIdentity = hasIdentity && CompareStringOrdinal(
            identity.pwszVal, -1, executablePath.c_str(), -1, TRUE) == CSTR_EQUAL;
        PropVariantClear(&identity);
        if (FAILED(result)) return result;
        if (matches) return S_FALSE;
        if (hasIdentity && !defaultIdentity) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        // Preserve the existing target, arguments, working directory and icon.
    }
    else
    {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) return HRESULT_FROM_WIN32(error);
        if (FAILED(result = link->SetPath(executablePath.c_str())) ||
            FAILED(result = link->SetWorkingDirectory(
                std::filesystem::path(executablePath).parent_path().c_str())) ||
            FAILED(result = link->SetIconLocation(executablePath.c_str(), 0)))
            return result;
    }
    PROPVARIANT identity{};
    identity.vt = VT_LPWSTR;
    identity.pwszVal = const_cast<wchar_t*>(PortableApplicationId);
    if (FAILED(result = properties->SetValue(PKEY_AppUserModel_ID, identity)) ||
        FAILED(result = properties->Commit())) return result;

    GUID unique{};
    if (FAILED(result = CoCreateGuid(&unique))) return result;
    wchar_t suffix[40]{};
    StringFromGUID2(unique, suffix, static_cast<int>(std::size(suffix)));
    const std::wstring temporary = shortcutPath + suffix + L".tmp";
    result = file->Save(temporary.c_str(), FALSE);
    if (SUCCEEDED(result) && !MoveFileExW(temporary.c_str(), shortcutPath.c_str(),
            MOVEFILE_WRITE_THROUGH | (existing ? MOVEFILE_REPLACE_EXISTING : 0)))
        result = HRESULT_FROM_WIN32(GetLastError());
    DeleteFileW(temporary.c_str());
    return result;
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
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        type.lpszClassName, L"SnowDesktop", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, type.hInstance, callbackWindow);
    if (!window)
        return nullptr;
    const auto identity = ApplicationId();
    if (identity.empty() || FAILED(SetNotificationIdentity(window, identity.c_str())))
    {
        DestroyWindow(window);
        return nullptr;
    }
    return window;
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

    RegisterPortableNotificationApplication();

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
    // Branding belongs to Windows' attribution header, not the content image.
    data.dwInfoFlags = NIIF_NONE;
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
