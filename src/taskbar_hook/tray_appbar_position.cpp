#include "tray_appbar_position.h"

#include <MinHook.h>
#include <shellapi.h>
#include <iterator>
#include <mutex>

namespace snowdesktop::tray::first_show::appbar
{
namespace
{
struct Position
{
    HWND bar = nullptr;
    HWND receiver = nullptr;
    DWORD host = 0;
    UINT edge = ABE_TOP;
    ULONGLONG expires = 0;
};

thread_local Position current;
decltype(&SHAppBarMessage) original = nullptr;

bool IsNativeTrayWindow(HWND root)
{
    wchar_t name[64]{};
    if (!GetClassNameW(root, name, static_cast<int>(std::size(name)))) return false;
    return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(name, L"NotifyIconOverflowWindow") == 0 ||
        wcscmp(name, L"TopLevelWindowForOverflowXamlIsland") == 0;
}

bool IsNativeTrayActive()
{
    if (IsNativeTrayWindow(GetForegroundWindow())) return true;
    POINT pointer{};
    return GetCursorPos(&pointer) && IsNativeTrayWindow(GetAncestor(WindowFromPoint(pointer), GA_ROOT));
}

UINT_PTR WINAPI Query(DWORD message, PAPPBARDATA data)
{
    const auto context = current;
    if (message == ABM_GETTASKBARPOS && data && data->cbSize >= sizeof(APPBARDATA) &&
        context.bar && GetTickCount64() < context.expires)
    {
        DWORD owner = 0;
        RECT bounds{};
        if (GetWindowThreadProcessId(context.bar, &owner) && owner == context.host &&
            GetWindowThreadProcessId(context.receiver, nullptr) == GetCurrentThreadId() &&
            IsWindowVisible(context.bar) && GetWindowRect(context.bar, &bounds) &&
            !IsRectEmpty(&bounds) && !IsNativeTrayActive())
        {
            data->uEdge = context.edge;
            data->rc = bounds;
            return TRUE;
        }
    }
    return original(message, data);
}

bool Install()
{
    static std::mutex mutex;
    static bool installed = false;
    std::lock_guard lock(mutex);
    if (installed) return true;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* const entry = reinterpret_cast<void*>(&SHAppBarMessage);
    void* trampoline = nullptr;
    if (MH_CreateHook(entry, reinterpret_cast<void*>(&Query), &trampoline) != MH_OK) return false;
    original = reinterpret_cast<decltype(original)>(trampoline);
    if (MH_EnableHook(entry) == MH_OK)
    {
        installed = true;
        return true;
    }
    MH_RemoveHook(entry);
    return false;
}
}

bool Arm(const Request& request, DWORD host)
{
    current = {};
    const auto bar = reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.bar));
    const auto receiver = reinterpret_cast<HWND>(static_cast<UINT_PTR>(request.receiver));
    DWORD owner = 0;
    wchar_t name[64]{};
    if (!bar || !GetWindowThreadProcessId(bar, &owner) || owner != host ||
        !GetClassNameW(bar, name, static_cast<int>(std::size(name))) || wcscmp(name, L"SnowDesktop.StatusBar") != 0)
        return false;
    if (!Install()) return false;
    current = {bar, receiver, host, static_cast<UINT>(request.preferAbove ? ABE_BOTTOM : ABE_TOP),
        request.expires};
    return true;
}
}
