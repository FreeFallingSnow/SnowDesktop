/**
 * @file main.cpp
 * @brief 应用程序入口点
 *
 * 负责单实例管理、异常崩溃处理以及 DesktopApp 的启动。
 * 启动流程：
 *   1. 检测命令行特殊开关（例如 --restore-explorer-icons）
 *   2. 关闭已存在的同名进程实例（单实例保证）
 *   3. 注册全局未处理异常过滤器和崩溃日志处理器
 *   4. 注册应用程序重启回调
 *   5. 创建 DesktopApp 对象并进入消息循环
 */

#include "app.h"
#include "crashlog.h"

#include <tlhelp32.h>

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* info)
{
    CrashHandler(info); // write stack trace to log

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
        if (entry.th32ProcessID == selfProcessId || _wcsicmp(entry.szExeFile, L"SnowDesktop.exe") != 0)
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

/**
 * @brief Windows GUI 应用程序入口
 * @param instance 当前应用程序实例句柄
 * @param commandLine 命令行参数（Unicode）
 * @param showCommand 窗口显示方式（SW_SHOWNORMAL 等）
 * @return 应用程序退出码
 */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    /* 处理特殊命令行开关：仅恢复资源管理器图标后立即退出 */
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    /* 关闭已存在的同名实例，确保单实例运行 */
    CloseExistingInstances();

    /* 注册全局未处理异常过滤器与崩溃日志处理器 */
    SetUnhandledExceptionFilter(UnhandledFilter);
    InstallCrashHandler();

    /* 注册应用程序自动重启，遇到某些崩溃后可自动重启 */
    RegisterApplicationRestart(nullptr, RESTART_NO_CRASH | RESTART_NO_HANG);

    /* 创建主应用实例并进入消息循环 */
    DesktopApp app;
    return app.Run(instance, showCommand);
}
