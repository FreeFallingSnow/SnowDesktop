#include "dock_minimize_hook_threads.h"
#include "dock_minimize_protocol.h"
#include "../taskbar_hook/tray_menu_first_show.h"

#include <shellapi.h>
#include <cstdlib>
#include <vector>

namespace
{
int ArmTrayPopup(wchar_t** args)
{
    namespace popup = snowdesktop::tray::first_show;
    DWORD values[4]{};
    for (int i = 0; i < 4; ++i)
    {
        wchar_t* end = nullptr;
        values[i] = wcstoul(args[i + 2], &end, 10);
        if (!values[i] || !end || *end) return 1;
    }
    const auto receiver = reinterpret_cast<HWND>(ULongToHandle(values[2]));
    const auto mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, popup::RequestName(values[0], values[1]).c_str());
    if (!mapping) return 1;
    const auto view = static_cast<const popup::Request*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(popup::Request)));
    if (view && view->process == values[3] && view->receiver == values[2] &&
        view->surface == popup::Surface::WpsHover && GetTickCount64() < view->expires)
    {
        if (const auto module = LoadLibraryExW(args[6], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
        {
            popup::SendArm(module, receiver, values[3], values[0], values[1]);
            FreeLibrary(module);
        }
    }
    if (view) UnmapViewOfFile(view);
    CloseHandle(mapping);
    return 0;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
    if (count == 7 && wcscmp(args[1], snowdesktop::tray::first_show::kArmHelperSwitch) == 0)
    {
        const int result = ArmTrayPopup(args);
        LocalFree(args);
        return result;
    }
    if (count != 4) { LocalFree(args); return 1; }
    wchar_t* end = nullptr;
    const DWORD ownerId = wcstoul(args[1], &end, 10);
    const bool validOwner = ownerId && end && !*end;
    const unsigned long windowValue = wcstoul(args[2], &end, 10);
    const HWND receiver = reinterpret_cast<HWND>(ULongToHandle(windowValue));
    const bool validWindow = windowValue && end && !*end;
    const std::wstring dll = args[3];
    LocalFree(args);
    DWORD receiverProcess = 0;
    GetWindowThreadProcessId(receiver, &receiverProcess);
    if (!validOwner || !validWindow || receiverProcess != ownerId) return 1;
    const HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, ownerId);
    if (!owner) return 1;
    snowdesktop::dock_minimize::ThreadHooks hooks;
    if (!hooks.Start(dll)) { CloseHandle(owner); return 1; }
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    bool quit = false;
    DWORD lastRevision = MAXDWORD;
    std::vector<HWND> readyWindows;
    while (!quit && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT &&
        GetPropW(receiver, snowdesktop::dock_minimize::kOwnerProperty) == ULongToHandle(ownerId))
    {
        const DWORD revision = HandleToULong(GetPropW(receiver, snowdesktop::dock_minimize::kRevisionProperty));
        if (revision != lastRevision || hooks.HasPendingRetries())
        {
            lastRevision = revision;
            for (HWND window : readyWindows)
            {
                if (GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == receiver)
                    RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
                if (GetPropW(window, snowdesktop::dock_minimize::kCloakDispatcherProperty) == hooks.CloakWindow())
                    RemovePropW(window, snowdesktop::dock_minimize::kCloakDispatcherProperty);
            }
            readyWindows.clear();
            struct Context { HWND receiver; std::vector<DWORD> threads; std::vector<HWND> windows; } context{receiver, {}, {}};
            EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
                auto& state = *reinterpret_cast<Context*>(parameter);
                if (GetPropW(window, snowdesktop::dock_minimize::kTargetProperty) != state.receiver)
                    return TRUE;
                DWORD process = 0;
                const DWORD thread = GetWindowThreadProcessId(window, &process);
                if (thread && snowdesktop::dock_minimize::Is32BitProcess(process))
                {
                    state.threads.push_back(thread);
                    state.windows.push_back(window);
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&context));
            hooks.Sync(context.threads);
            for (HWND window : context.windows)
            {
                if (hooks.HasThread(GetWindowThreadProcessId(window, nullptr)) &&
                    GetPropW(window, snowdesktop::dock_minimize::kTargetProperty) == receiver &&
                    SetPropW(window, snowdesktop::dock_minimize::kCloakDispatcherProperty, hooks.CloakWindow()) &&
                    SetPropW(window, snowdesktop::dock_minimize::kReadyProperty, receiver))
                    readyWindows.push_back(window);
            }
        }
        MsgWaitForMultipleObjects(1, &owner, FALSE, 250, QS_ALLINPUT);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    for (HWND window : readyWindows)
    {
        if (GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == receiver)
            RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
        if (GetPropW(window, snowdesktop::dock_minimize::kCloakDispatcherProperty) == hooks.CloakWindow())
            RemovePropW(window, snowdesktop::dock_minimize::kCloakDispatcherProperty);
    }
    hooks.Stop();
    CloseHandle(owner);
    return 0;
}
