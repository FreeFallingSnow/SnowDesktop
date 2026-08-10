/**
 * @file main.cpp
 * @brief 应用程序入口点
 *
 * 负责单实例管理、异常崩溃处理以及 DesktopApp 的启动。
 * 启动流程：
 *   1. 检测命令行特殊开关（例如 --restore-explorer-icons）
 *   2. 取得跨安装位置共享的单实例锁
 *   3. 注册全局未处理异常过滤器和崩溃日志处理器
 *   4. 注册应用程序重启回调
 *   5. 创建 DesktopApp 对象并进入消息循环
 */

#include "app.h"
#include "crashlog.h"
#include "application_crash_watchdog.h"
#include "application_restart_policy.h"
#include "data_paths.h"
#include "general_settings.h"
#include "l10n.h"
#include "single_instance.h"

#include <commctrl.h>

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#define SNOWDESKTOP_WIDEN_INNER(value) L##value
#define SNOWDESKTOP_WIDEN(value) SNOWDESKTOP_WIDEN_INNER(value)

namespace
{
constexpr wchar_t kCurrentVersion[] =
    SNOWDESKTOP_WIDEN(SNOWDESKTOP_VERSION);

enum class ExistingInstanceResolution
{
    ExitNewInstance,
    RetryLaunch
};

enum class VersionConflictChoice
{
    Switch,
    KeepRunning,
    Cancel
};

void InitializeStartupLocale()
{
    std::filesystem::path languageDirectory =
        GetExecutableDirectoryPath();
    languageDirectory /= L"lang";
    Locale::Instance().Init(languageDirectory.c_str());

    GeneralSettings settings;
    const std::filesystem::path settingsPath =
        std::filesystem::path(GetDataDirectoryPath()) /
        L"SnowDesktop.general.json";
    if (LoadGeneralSettings(settingsPath.c_str(), settings))
        Locale::Instance().SetLanguage(settings.language);
}

std::wstring DisplayDataDirectory(const std::wstring& path)
{
    return path.empty()
        ? std::wstring(_LW("app.run.other_version_unknown_path"))
        : path;
}

std::wstring DeploymentSuffix(
    const snowdesktop::single_instance::InstanceInfo& instance)
{
    if (!instance.packaged)
        return {};
    return _LFW(
        "app.run.other_version_deployment_suffix",
        _LW("app.run.other_version_installed"));
}

bool SameDeployment(
    const snowdesktop::single_instance::InstanceInfo& left,
    const snowdesktop::single_instance::InstanceInfo& right)
{
    if (left.processId != 0 && left.processId == right.processId)
        return true;
    return !left.dataDirectory.empty() && !right.dataDirectory.empty() &&
        snowdesktop::single_instance::DataDirectoriesMatch(
            left.dataDirectory, right.dataDirectory);
}

VersionConflictChoice ShowVersionConflictPrompt(
    const snowdesktop::single_instance::InstanceInfo& running,
    const snowdesktop::single_instance::InstanceInfo& requested)
{
    InitializeStartupLocale();

    const bool sharedData =
        snowdesktop::single_instance::DataDirectoriesMatch(
            running.dataDirectory, requested.dataDirectory);
    std::wstring content = _LFW(
        "app.run.other_version_details",
        running.version,
        DeploymentSuffix(running),
        DisplayDataDirectory(running.dataDirectory),
        requested.version,
        DeploymentSuffix(requested),
        DisplayDataDirectory(requested.dataDirectory));
    content += L"\n\n";
    content += sharedData
        ? _LW("app.run.other_version_shared_data")
        : _LW("app.run.other_version_separate_data");

    const std::wstring instruction = _LFW(
        "app.run.other_version_instruction",
        running.version,
        DeploymentSuffix(running));
    const std::wstring switchButton = _LFW(
        "app.run.other_version_switch",
        requested.version,
        DeploymentSuffix(requested));
    const std::wstring keepButton = _LFW(
        "app.run.other_version_keep",
        running.version,
        DeploymentSuffix(running));
    const TASKDIALOG_BUTTON buttons[] = {
        { 100, switchButton.c_str() },
        { 101, keepButton.c_str() },
    };

    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags =
        TDF_ALLOW_DIALOG_CANCELLATION |
        TDF_SIZE_TO_CONTENT |
        TDF_USE_COMMAND_LINKS;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle =
        _LW("app.run.other_version_title");
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = instruction.c_str();
    dialog.pszContent = content.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(buttons));
    dialog.pButtons = buttons;
    dialog.nDefaultButton = 101;

    int selectedButton = IDCANCEL;
    if (SUCCEEDED(TaskDialogIndirect(
            &dialog, &selectedButton, nullptr, nullptr)))
    {
        if (selectedButton == 100)
            return VersionConflictChoice::Switch;
        if (selectedButton == 101)
            return VersionConflictChoice::KeepRunning;
        return VersionConflictChoice::Cancel;
    }

    const int fallback = MessageBoxW(
        nullptr, content.c_str(), instruction.c_str(),
        MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (fallback == IDYES)
        return VersionConflictChoice::Switch;
    if (fallback == IDNO)
        return VersionConflictChoice::KeepRunning;
    return VersionConflictChoice::Cancel;
}

ExistingInstanceResolution ResolveExistingInstance(
    const snowdesktop::single_instance::InstanceInfo& running,
    const snowdesktop::single_instance::InstanceInfo& requested,
    snowdesktop::single_instance::InstanceInfo* switchTarget)
{
    const bool versionsMatch =
        running.version.empty() || requested.version.empty() ||
        snowdesktop::single_instance::VersionsMatch(
            running.version, requested.version);
    const bool knownDataDirectoriesDiffer =
        !running.dataDirectory.empty() &&
        !requested.dataDirectory.empty() &&
        !snowdesktop::single_instance::DataDirectoriesMatch(
            running.dataDirectory, requested.dataDirectory);
    if (versionsMatch && !knownDataDirectoriesDiffer)
    {
        snowdesktop::single_instance::NotifyExistingInstance(running);
        return ExistingInstanceResolution::ExitNewInstance;
    }

    const VersionConflictChoice choice =
        ShowVersionConflictPrompt(running, requested);
    if (choice == VersionConflictChoice::KeepRunning)
    {
        snowdesktop::single_instance::NotifyExistingInstance(running);
        return ExistingInstanceResolution::ExitNewInstance;
    }
    if (choice != VersionConflictChoice::Switch)
        return ExistingInstanceResolution::ExitNewInstance;

    if (snowdesktop::single_instance::RequestExistingInstanceExit(
            running, 30000))
    {
        if (switchTarget)
            *switchTarget = running;
        return ExistingInstanceResolution::RetryLaunch;
    }

    const std::wstring message = _LFW(
        "app.run.other_version_switch_failed",
        running.version,
        DeploymentSuffix(running));
    MessageBoxW(nullptr, message.c_str(),
        _LW("app.run.other_version_title"),
        MB_OK | MB_ICONERROR);
    return ExistingInstanceResolution::ExitNewInstance;
}
}

/*
 * 崩溃计数器：记录最近崩溃的时间戳（tick），存储在注册表中。
 * 若 60 秒内崩溃超过 3 次，则禁止自动重启，防止无限重启风暴。
 */
static constexpr DWORD kCrashWindowSeconds = 60;
static constexpr int kMaxCrashesInWindow = 3;
static constexpr wchar_t kRegSubKey[] = L"Software\\SnowDesktop";
static constexpr wchar_t kRegValueName[] = L"CrashTicks";
static std::atomic_bool g_crashWatchdogStarted = false;

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

static std::wstring GetCurrentExecutablePath()
{
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return {};
        if (length < buffer.size() - 1)
            return std::wstring(buffer.data(), length);
        if (buffer.size() >= 32768)
            return {};
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
    }
}

static bool LaunchRestartAfterProcess(DWORD processId)
{
    const std::wstring selfPath = GetCurrentExecutablePath();
    if (selfPath.empty())
        return false;
    wchar_t parameters[64]{};
    swprintf_s(parameters, L"--wait-for-pid=%lu", processId);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(
        nullptr, L"open", selfPath.c_str(), parameters,
        nullptr, SW_SHOWNORMAL)) > 32;
}

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* info)
{
    CrashHandler(info); // write stack trace to log

    if (!g_crashWatchdogStarted.load(std::memory_order_acquire) &&
        !ShouldPreventAutoRestart())
    {
        LaunchRestartAfterProcess(GetCurrentProcessId());
    }
    return EXCEPTION_EXECUTE_HANDLER;
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
    const std::uintptr_t watchedProcessHandle =
        snowdesktop::application_restart_policy::
            ParseWatchProcessHandle(
                commandLine ? commandLine : L"");
    if (watchedProcessHandle != 0)
    {
        const auto result =
            snowdesktop::application_crash_watchdog::WatchProcess(
                reinterpret_cast<HANDLE>(watchedProcessHandle),
                ShouldPreventAutoRestart,
                LaunchRestartAfterProcess);
        return static_cast<int>(result.error);
    }

    /* 处理特殊命令行开关：仅恢复资源管理器图标后立即退出 */
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    const DWORD predecessor =
        snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                commandLine ? commandLine : L"");
    if (predecessor &&
        !snowdesktop::single_instance::WaitForRestartPredecessor(
            predecessor, 30000))
    {
        return ERROR_TIMEOUT;
    }

    snowdesktop::single_instance::Guard singleInstance;
    const auto requestedInstance =
        snowdesktop::single_instance::DescribeCurrentInstance(
            kCurrentVersion);
    auto acquisition =
        snowdesktop::single_instance::AcquireResult::Existing;
    snowdesktop::single_instance::InstanceInfo switchTarget;
    bool switchHandoff = false;
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        // Older builds do not own the mutex but do expose the stable control
        // window. Inspect that window first so cross-version launches can
        // explain and offer a controlled switch.
        if (const auto running =
                snowdesktop::single_instance::
                    FindExistingInstance(0))
        {
            if (switchHandoff && SameDeployment(*running, switchTarget))
            {
                if (snowdesktop::single_instance::RequestExistingInstanceExit(
                        *running, 30000))
                    continue;

                const std::wstring message = _LFW(
                    "app.run.other_version_switch_failed",
                    running->version,
                    DeploymentSuffix(*running));
                MessageBoxW(nullptr, message.c_str(),
                    _LW("app.run.other_version_title"),
                    MB_OK | MB_ICONERROR);
                return 0;
            }

            const auto resolution = ResolveExistingInstance(
                *running, requestedInstance, &switchTarget);
            if (resolution == ExistingInstanceResolution::ExitNewInstance)
            {
                return 0;
            }
            switchHandoff =
                resolution == ExistingInstanceResolution::RetryLaunch;
            // The user chose to close the running version. Retry the
            // stable lock after its process has fully exited.
            continue;
        }

        acquisition = singleInstance.Acquire();
        if (acquisition ==
            snowdesktop::single_instance::AcquireResult::Primary)
        {
            break;
        }
        if (acquisition ==
            snowdesktop::single_instance::AcquireResult::Error)
        {
            break;
        }

        // The owner can acquire the mutex before its control window exists.
        // Give startup time to publish the window, then resolve it on the
        // next pass with full version information.
        if (!snowdesktop::single_instance::
                FindExistingInstance(5000))
        {
            continue;
        }
    }
    if (acquisition ==
        snowdesktop::single_instance::AcquireResult::Error)
    {
        wchar_t diagnostic[160]{};
        swprintf_s(diagnostic,
            L"SnowDesktop: single-instance lock failed (error %lu).\n",
            singleInstance.LastError());
        OutputDebugStringW(diagnostic);
        return static_cast<int>(singleInstance.LastError());
    }
    if (acquisition !=
        snowdesktop::single_instance::AcquireResult::Primary)
    {
        return 0;
    }

    /* 注册全局未处理异常过滤器与崩溃日志处理器 */
    SetUnhandledExceptionFilter(UnhandledFilter);
    InstallCrashHandler();

    // This out-of-process watcher observes fail-fast exits that bypass every
    // in-process exception filter. UnhandledFilter remains as startup fallback.
    const bool watchdogStarted =
        snowdesktop::application_crash_watchdog::
            StartForCurrentProcess(GetCurrentExecutablePath());
    g_crashWatchdogStarted.store(
        watchdogStarted, std::memory_order_release);
    if (!watchdogStarted)
        OutputDebugStringW(
            L"SnowDesktop: failed to start crash watchdog.\n");

    /* 注册系统恢复作为补充；排除 HANG，避免无响应时系统反复拉起 */
    RegisterApplicationRestart(
        nullptr, snowdesktop::application_restart_policy::kFlags);

    /* 创建主应用实例并进入消息循环 */
    DesktopApp app;
    int result = app.Run(instance, showCommand);

    /* 正常退出时清除崩溃计数器，避免残留记录影响后续启动 */
    if (result == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegSubKey, kRegValueName);

    return result;
}
