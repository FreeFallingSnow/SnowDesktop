#pragma once

#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

namespace snowdesktop::tray::first_show
{
inline constexpr wchar_t kArmMessage[] = L"SnowDesktop.Tray.PopupPlacement.v4";
inline constexpr wchar_t kArmHelperSwitch[] = L"--tray-popup-placement";
inline constexpr DWORD kLifetimeMs = 1500;

enum class Surface : std::uint32_t { WeixinMenu, QQHover, WpsHover, DouyinHover, AppBarOnly };

struct Request
{
    std::uint64_t receiver = 0;
    ULONGLONG expires = 0;
    POINT anchor{};
    RECT icon{};
    RECT row{};
    RECT work{};
    Surface surface = Surface::WeixinMenu;
    LONG gap = 0;
    BOOL preferAbove = FALSE;
    DWORD process = 0;
    volatile LONG armed = 0;
    std::uint64_t presenter = 0;
    std::uint64_t bar = 0;
};
static_assert(sizeof(Request) == 112);

inline std::wstring RequestName(DWORD owner, ULONG_PTR serial)
{
    return L"Local\\SnowDesktop.Tray.PopupPlacement.v4." +
        std::to_wstring(owner) + L"." + std::to_wstring(serial);
}

inline bool IsWeixinReceiver(HWND window)
{
    wchar_t name[80]{};
    GetClassNameW(window, name, static_cast<int>(sizeof(name) / sizeof(name[0])));
    return wcscmp(name, L"Qt51514WxTrayIconMessageWindowClass") == 0;
}

void Arm(const CWPSTRUCT& message);

inline void SendArm(HMODULE module, HWND receiver, DWORD process, DWORD owner, DWORD serial)
{
#ifdef _WIN64
    constexpr char exportName[] = "SnowDesktopTrayHookProc";
#else
    constexpr char exportName[] = "_SnowDesktopTrayHookProc@12";
#endif
    DWORD actualProcess = 0;
    const auto thread = GetWindowThreadProcessId(receiver, &actualProcess);
    const UINT message = RegisterWindowMessageW(kArmMessage);
    const auto procedure = reinterpret_cast<HOOKPROC>(GetProcAddress(module, exportName));
    if (!message || !procedure || !thread || actualProcess != process) return;
    if (const auto hook = SetWindowsHookExW(WH_CALLWNDPROC, procedure, module, thread))
    {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(receiver, message, owner, serial,
            SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 80, &ignored);
        UnhookWindowsHookEx(hook);
    }
}
}
