#include "app_oo.h"

#include <tlhelp32.h>

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS*)
{
    wchar_t selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    ShellExecuteW(nullptr, L"open", selfPath, nullptr, nullptr, SW_SHOWNORMAL);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void CloseExistingInstances()
{
    wchar_t selfPath[MAX_PATH * 4]{};
    DWORD selfPathLength = GetModuleFileNameW(nullptr, selfPath, static_cast<DWORD>(std::size(selfPath)));
    if (selfPathLength == 0) return;

    const DWORD selfProcessId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry)) { CloseHandle(snapshot); return; }

    do
    {
        if (entry.th32ProcessID == selfProcessId || _wcsicmp(entry.szExeFile, L"SnowDesktopOO.exe") != 0)
            continue;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (process == nullptr) continue;

        std::wstring imagePath;
        wchar_t buffer[MAX_PATH * 4]{};
        DWORD size = static_cast<DWORD>(std::size(buffer));
        if (QueryFullProcessImageNameW(process, 0, buffer, &size))
            imagePath.assign(buffer, size);

        if (CompareStringOrdinal(imagePath.c_str(), -1, selfPath, -1, TRUE) == CSTR_EQUAL)
        {
            DWORD windowProcessId = entry.th32ProcessID;
            EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == static_cast<DWORD>(lp)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&windowProcessId));
            if (WaitForSingleObject(process, 1500) == WAIT_TIMEOUT)
            {
                TerminateProcess(process, 0);
                WaitForSingleObject(process, 3000);
            }
        }
        CloseHandle(process);
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    CloseExistingInstances();

    SetUnhandledExceptionFilter(UnhandledFilter);
    RegisterApplicationRestart(nullptr, RESTART_NO_CRASH | RESTART_NO_HANG);

    SnowDesktopAppOO app;
    return app.Run(instance, showCommand);
}
