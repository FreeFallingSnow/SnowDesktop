#include "dock_minimize_hook_threads.h"
#include "dock_minimize_protocol.h"

#include <shellapi.h>
#include <cstdlib>
#include <vector>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return 1;
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
        if (revision != lastRevision)
        {
            lastRevision = revision;
            for (HWND window : readyWindows)
                if (GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == receiver)
                    RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
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
    hooks.Stop();
    for (HWND window : readyWindows)
        if (GetPropW(window, snowdesktop::dock_minimize::kReadyProperty) == receiver)
            RemovePropW(window, snowdesktop::dock_minimize::kReadyProperty);
    CloseHandle(owner);
    return 0;
}
