#pragma once

#include <windows.h>
#include <cwchar>
#include <string>

namespace snowdesktop::tray::first_show
{
inline constexpr wchar_t kArmMessage[] = L"SnowDesktop.Tray.MenuFirstShow.v1";
inline constexpr DWORD kLifetimeMs = 1500;

struct Request
{
    UINT_PTR receiver = 0;
    ULONGLONG expires = 0;
    POINT anchor{};
    RECT icon{};
};

inline std::wstring RequestName(DWORD owner, ULONG_PTR serial)
{
    return L"Local\\SnowDesktop.Tray.MenuFirstShow.v1." +
        std::to_wstring(owner) + L"." + std::to_wstring(serial);
}

inline bool IsWeixinReceiver(HWND window)
{
    wchar_t name[80]{};
    GetClassNameW(window, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
    return wcscmp(name, L"Qt51514WxTrayIconMessageWindowClass") == 0;
}

void Arm(const CWPSTRUCT& message);
}
