#include "app.h"

#include <tlhelp32.h>

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS*)
{
    // On crash, try to restart
    wchar_t selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    ShellExecuteW(nullptr, L"open", selfPath, nullptr, nullptr, SW_SHOWNORMAL);
    return EXCEPTION_EXECUTE_HANDLER;
}

void WaitForDesktopHostAfterExplorerRestart()
{
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        DesktopWindows windows = FindDesktopWindows();
        if (windows.host != nullptr && windows.defView != nullptr)
        {
            return;
        }
        Sleep(200);
    }
}

struct CloseWindowsContext
{
    DWORD processId = 0;
};

BOOL CALLBACK CloseWindowsForProcessProc(HWND hwnd, LPARAM lParam)
{
    auto* context = reinterpret_cast<CloseWindowsContext*>(lParam);
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(hwnd, &windowProcessId);
    if (windowProcessId == context->processId)
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

bool GetProcessImagePath(HANDLE process, std::wstring& path)
{
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!QueryFullProcessImageNameW(process, 0, buffer, &size))
    {
        return false;
    }
    path.assign(buffer, size);
    return true;
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

void CloseExistingInstances()
{
    wchar_t selfPath[MAX_PATH * 4]{};
    DWORD selfPathLength = GetModuleFileNameW(nullptr, selfPath, static_cast<DWORD>(std::size(selfPath)));
    if (selfPathLength == 0)
    {
        return;
    }

    const DWORD selfProcessId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot, &entry))
    {
        CloseHandle(snapshot);
        return;
    }

    do
    {
        if (entry.th32ProcessID == selfProcessId || _wcsicmp(entry.szExeFile, L"SnowDesktop.exe") != 0)
        {
            continue;
        }

        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE,
            entry.th32ProcessID);
        if (process == nullptr)
        {
            continue;
        }

        std::wstring imagePath;
        if (GetProcessImagePath(process, imagePath) && SamePath(imagePath, selfPath))
        {
            CloseWindowsContext context{ entry.th32ProcessID };
            EnumWindows(CloseWindowsForProcessProc, reinterpret_cast<LPARAM>(&context));
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

    if (commandLine != nullptr && wcsstr(commandLine, L"--wait-for-desktop-host") != nullptr)
    {
        WaitForDesktopHostAfterExplorerRestart();
    }

    SetUnhandledExceptionFilter(UnhandledFilter);
    RegisterApplicationRestart(nullptr, RESTART_NO_CRASH | RESTART_NO_HANG);

    UINT smokeTestMs = 0;
    if (commandLine != nullptr)
    {
        const wchar_t* smokeArg = wcsstr(commandLine, L"--smoke-test-ms=");
        if (smokeArg != nullptr)
        {
            smokeArg += wcslen(L"--smoke-test-ms=");
            smokeTestMs = static_cast<UINT>(std::max(0, _wtoi(smokeArg)));
        }
    }

    SnowDesktopApp app;
    return app.Run(instance, showCommand, smokeTestMs);
}
