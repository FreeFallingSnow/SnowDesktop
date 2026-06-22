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

/*
 * 崩溃计数器：记录最近崩溃的时间戳（tick），存储在注册表中。
 * 若 60 秒内崩溃超过 3 次，则禁止自动重启，防止无限重启风暴。
 */
static constexpr DWORD kCrashWindowSeconds = 60;
static constexpr int kMaxCrashesInWindow = 3;
static constexpr wchar_t kRegSubKey[] = L"Software\\SnowDesktop";
static constexpr wchar_t kRegValueName[] = L"CrashTicks";

static bool ShouldPreventAutoRestart()
{
    std::vector<DWORD> ticks;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
    {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0,
                KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
    }

    DWORD type = REG_BINARY;
    DWORD dataSize = 0;
    if (RegQueryValueExW(key, kRegValueName, nullptr, &type, nullptr, &dataSize) == ERROR_SUCCESS &&
        type == REG_BINARY && dataSize >= sizeof(DWORD))
    {
        ticks.resize(dataSize / sizeof(DWORD));
        RegQueryValueExW(key, kRegValueName, nullptr, nullptr,
            reinterpret_cast<BYTE*>(ticks.data()), &dataSize);
    }

    DWORD now = GetTickCount();
    std::vector<DWORD> recent;
    for (DWORD t : ticks)
    {
        if (now - t <= kCrashWindowSeconds * 1000)
            recent.push_back(t);
    }

    if (static_cast<int>(recent.size()) >= kMaxCrashesInWindow)
    {
        RegCloseKey(key);
        return true;
    }

    recent.push_back(now);
    RegSetValueExW(key, kRegValueName, 0, REG_BINARY,
        reinterpret_cast<const BYTE*>(recent.data()),
        static_cast<DWORD>(recent.size() * sizeof(DWORD)));
    RegCloseKey(key);
    return false;
}

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* info)
{
    CrashHandler(info); // write stack trace to log

    if (!ShouldPreventAutoRestart())
    {
        wchar_t selfPath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
        ShellExecuteW(nullptr, L"open", selfPath, nullptr, nullptr, SW_SHOWNORMAL);
    }
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

    /* 注册应用程序崩溃后自动重启（不含 HANG，避免无响应时系统反复拉起） */
    RegisterApplicationRestart(nullptr, RESTART_NO_CRASH);

    /* 创建主应用实例并进入消息循环 */
    DesktopApp app;
    int result = app.Run(instance, showCommand);

    /* 正常退出时清除崩溃计数器，避免残留记录影响后续启动 */
    if (result == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegSubKey, kRegValueName);

    return result;
}
