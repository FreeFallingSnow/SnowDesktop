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

#include "app/app.h"
#include "platform/auto_start_elevation.h"
#include "platform/auto_start_launch.h"
#include "settings/settings_process.h"
#include "shell/shell_launch_process.h"
#include "shell/shell_extension_menu.h"
#include "shell/shell_start_pin.h"
#include "shell/start_menu_launcher.h"
#include "diagnostics/crashlog.h"
#include "platform/application_crash_watchdog.h"
#include "platform/application_restart_policy.h"
#include "platform/crash_restart_history.h"
#include "data/data_paths.h"
#include "diagnostics/debug_profile.h"
#include "platform/deployment_context.h"
#include "settings/general_settings.h"
#include "common/l10n.h"
#include "platform/single_instance.h"
#include "widget/authoring/widget_author_preview.h"
#include "ui/preview/native_component_preview_export.h"
#include "winui/settings_visual_preview.h"
#include "steam/steam_runtime_startup.h"
#include "taskbar_hook/taskbar_symbol_resolver.h"

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

using ExistingInstanceResolution = snowdesktop::single_instance::StartupResolution;

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

bool ConfirmStartupRecovery(const char* key, std::string_view detail = {}, DWORD error = 0) noexcept
{
    const auto& fallback = snowdesktop::auto_start_launch::StartupMessages();
    try
    {
        InitializeStartupLocale();
        std::wstring message = Locale::Instance().TrW(key);
        if (message == Utf8ToWide(key)) message = fallback.warning;
        if (!detail.empty()) message += L"\n\n" + Utf8ToWide(std::string(detail));
        if (error) message += L"\n\n" + std::to_wstring(error);
        const wchar_t* start = _LW("app.startup.startAnyway");
        const wchar_t* cancel = _LW("settings.dialog.cancel");
        if (wcscmp(start, L"app.startup.startAnyway") == 0) start = fallback.startAnyway;
        if (wcscmp(cancel, L"settings.dialog.cancel") == 0) cancel = fallback.cancel;
        return snowdesktop::auto_start_launch::ConfirmStartupContinuation(message.c_str(), {start, cancel});
    }
    catch (...)
    {
        return snowdesktop::auto_start_launch::ConfirmStartupContinuation(fallback.warning,
            {fallback.startAnyway, fallback.cancel});
    }
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
    snowdesktop::single_instance::LaunchIntent intent,
    snowdesktop::single_instance::InstanceInfo* switchTarget)
{
    using snowdesktop::single_instance::ExistingLaunchAction;
    const auto action = snowdesktop::single_instance::
        HandleExistingInstanceLaunch(running, requested, intent);
    if (action == ExistingLaunchAction::ExitNewInstance)
        return ExistingInstanceResolution::ExitNewInstance;
    if (action == ExistingLaunchAction::ActivationFailed)
        return ExistingInstanceResolution::RetryLaunch;
    if (action == ExistingLaunchAction::ReplaceManagedRuntime)
    {
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
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS)
    {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0,
                KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
    }

    const std::vector<DWORD> ticks = snowdesktop::ReadCrashRestartHistory(key);

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
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return false;
    }
    const HRESULT initialized = CoInitializeEx(nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        SetLastError(static_cast<DWORD>(initialized));
        return false;
    }
    struct ComScope
    {
        bool uninitialize;
        ~ComScope()
        {
            const DWORD error = GetLastError();
            if (uninitialize) CoUninitialize();
            SetLastError(error);
        }
    } com{SUCCEEDED(initialized)};
    wchar_t parameters[64]{};
    swprintf_s(parameters, L"--wait-for-pid=%lu", processId);
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    launch.lpVerb = L"open";
    launch.lpFile = selfPath.c_str();
    launch.lpParameters = parameters;
    launch.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&launch) != FALSE;
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
namespace
{
const ULONGLONG startupFeedbackBegan = GetTickCount64();
void ShowStartupFailure(const snowdesktop::operation_feedback::Failure& failure)
{
    using namespace snowdesktop::steam_runtime::startup;
    const auto phase = HostChannel().CurrentPhase();
    // A supervising launcher reports exits during its 60-second wait. Once
    // that wait expires (or startup was already confirmed), this host owns UI.
    if (phase == Phase::AwaitingHost || phase == Phase::Ready ||
        GetTickCount64() - startupFeedbackBegan >= 60000)
        snowdesktop::operation_feedback::Show(failure);
    else
        WriteDiagnosticLogEntry((failure.detail + L" (error " +
            std::to_wstring(failure.error) + L")").c_str(), DiagnosticLogLevel::Error);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int showCommand)
{
    if (const auto result = snowdesktop::taskbar_hook::TryRunTaskbarSymbolHelper()) return *result;
    if (const auto result = snowdesktop::start_menu::TryRunLauncherHelper()) return *result;
    if (const auto result = snowdesktop::winui::TryRunSettingsVisualPreview()) return *result;
    if (const auto result = snowdesktop::auto_start::TryRunElevationCommand()) return *result;
    const bool inheritedHelper =
        snowdesktop::settings_ipc::IsSettingsProcessCommand() ||
        snowdesktop::settings_ipc::IsSettingsProcessCommand(L"--shell-menu-helper") ||
        snowdesktop::settings_ipc::IsSettingsProcessCommand(L"--shell-open-helper") ||
        snowdesktop::auto_start_launch::IsPreviewHostCommand(GetCommandLineW()) ||
        snowdesktop::application_restart_policy::ParseWatchProcessHandle(
            commandLine ? commandLine : L"") != 0;
    // Normalize every desktop launch before the Steam startup channel, data
    // discovery and the single-instance mutex acquire any process identity.
    const auto autoStartLaunch = snowdesktop::auto_start_launch::Normalize(!inheritedHelper,
        !inheritedHelper && snowdesktop::steam_runtime::startup::CurrentPrivilegesAcceptedFromEnvironment());
    if (autoStartLaunch.handled || autoStartLaunch.error)
    {
        const std::wstring message = std::wstring(autoStartLaunch.operation) +
            L" error=" + std::to_wstring(autoStartLaunch.error) +
            L" childPid=" + std::to_wstring(autoStartLaunch.childProcessId) +
            (autoStartLaunch.currentPrivilegesAccepted ? L"; continuing with current privileges" : L"");
        WriteDiagnosticLogEntry(message.c_str(), autoStartLaunch.currentPrivilegesAccepted
            ? DiagnosticLogLevel::Warning : autoStartLaunch.error
                ? DiagnosticLogLevel::Error : DiagnosticLogLevel::Info);
        if (autoStartLaunch.handled) return static_cast<int>(autoStartLaunch.error);
    }
    snowdesktop::steam_runtime::startup::Begin();
    // Helpers and deployment discovery may access settings before Run.
    // Conservatively prohibit automatic downgrade from this boundary.
    snowdesktop::steam_runtime::startup::BeginDataAccess();
    // Read once before startup probes and helper branches; the active profile's
    // normal settings path also keeps portable/installed data isolated.
    GeneralSettings diagnosticSettings;
    if (LoadGeneralSettings(GetGeneralSettingsPath().c_str(), diagnosticSettings))
        snowdesktop::diagnostics::SetLogModules(diagnosticSettings.diagnosticLogModules);
    if (const auto result = snowdesktop::shell_extensions::TryRunHelper({}, {},
            [](snowdesktop::shell_start_pin::Action action, const std::wstring &path, HWND owner, POINT point) {
                return snowdesktop::shell_start_pin::Invoke(
                    snowdesktop::deployment::GetTaskbarHookPath(), action, path, owner, point);
            })) return *result;
    if (const auto result = snowdesktop::shell_launch_process::TryRunCommand())
        return *result;

    if (snowdesktop::settings_ipc::IsSettingsProcessCommand())
    {
        // The settings child bypasses the desktop's later initialization. It
        // still needs crash evidence, without starting a second desktop host.
        SetUnhandledExceptionFilter(CrashHandler);
        InstallCrashHandler();
        return snowdesktop::settings_ipc::RunSettingsProcess(instance);
    }

    if (snowdesktop::deployment::TryHandlePackagedAutoStartQueryCommand())
        return 0;

    /* 处理特殊命令行开关：仅恢复资源管理器图标后立即退出 */
    if (commandLine != nullptr && wcsstr(commandLine, L"--restore-explorer-icons") != nullptr)
    {
        RestoreExplorerIconLayerNow();
        return 0;
    }

    if (snowdesktop::deployment::HasInvalidRuntimeDeploymentContext())
    {
        const auto& context =
            snowdesktop::deployment::GetRuntimeDeploymentContext();
        Locale::Instance().Init((std::filesystem::path(GetExecutableDirectoryPath()) / L"lang").c_str());
        ShowStartupFailure({"app.operation.startFailed", Utf8ToWide(context.error), ERROR_INVALID_DATA});
        return ERROR_INVALID_DATA;
    }

    bool previewCommandHandled = false;
    const int previewCommandResult = snowdesktop::widget_authoring::
        TryRunWidgetAuthorPreviewHostCommand(previewCommandHandled);
    if (previewCommandHandled)
        return previewCommandResult;

    bool nativePreviewCommandHandled = false;
    const int nativePreviewCommandResult =
        snowdesktop::native_component_preview::TryRunHostCommand(
            instance, nativePreviewCommandHandled);
    if (nativePreviewCommandHandled)
        return nativePreviewCommandResult;

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
                LaunchRestartAfterProcess,
                [](const snowdesktop::application_crash_watchdog::WatchResult& failed) {
                    const char* key = failed.restartAttempted ?
                        "app.startup.recoveryFailed" : "app.startup.recoveryStopped";
                    return ConfirmStartupRecovery(key, {}, failed.error);
                });
        return static_cast<int>(result.error);
    }

    const DWORD predecessor =
        snowdesktop::single_instance::
            ParseRestartPredecessorProcessId(
                commandLine ? commandLine : L"");
    if (predecessor &&
        !snowdesktop::single_instance::WaitForRestartPredecessor(
            predecessor, 30000))
    {
        InitializeStartupLocale();
        ShowStartupFailure({"app.operation.startFailed", L"Previous process did not exit", ERROR_TIMEOUT});
        return ERROR_TIMEOUT;
    }

    const auto launchIntent = snowdesktop::single_instance::
        ParseLaunchIntent(GetCommandLineW());
    snowdesktop::single_instance::Guard singleInstance;
    const auto requestedInstance =
        snowdesktop::single_instance::DescribeCurrentInstance(
            kCurrentVersion);
    snowdesktop::single_instance::InstanceInfo switchTarget;
    bool switchHandoff = false;
    const auto acquisition = snowdesktop::single_instance::AcquireForStartup(singleInstance,
        [](DWORD timeout) { return snowdesktop::single_instance::FindExistingInstance(timeout); },
        [&](const snowdesktop::single_instance::InstanceInfo& running) {
            // Older builds do not own the mutex but do expose the stable control
            // window. Resolve it before acquiring the stable mutex.
            if (switchHandoff && SameDeployment(running, switchTarget))
            {
                if (snowdesktop::single_instance::RequestExistingInstanceExit(
                        running, 30000))
                    return ExistingInstanceResolution::RetryLaunch;

                const std::wstring message = _LFW(
                    "app.run.other_version_switch_failed",
                    running.version,
                    DeploymentSuffix(running));
                MessageBoxW(nullptr, message.c_str(),
                    _LW("app.run.other_version_title"),
                    MB_OK | MB_ICONERROR);
                return ExistingInstanceResolution::ExitNewInstance;
            }

            const auto resolution = ResolveExistingInstance(
                running, requestedInstance, launchIntent, &switchTarget);
            if (resolution == ExistingInstanceResolution::ExitNewInstance)
            {
                return resolution;
            }
            switchHandoff =
                resolution == ExistingInstanceResolution::RetryLaunch;
            // The user chose to close the running version. Retry the
            // stable lock after its process has fully exited.
            return resolution;
        },
        [](DWORD error) {
            InitializeStartupLocale();
            const char* key = error == ERROR_BUSY ? "app.startup.instanceUnavailable" : "app.operation.startFailed";
            if (Locale::Instance().TrW(key) == Utf8ToWide(key)) key = "app.operation.startFailed";
            ShowStartupFailure({key, L"Single-instance startup coordination failed", error});
        });
    if (!acquisition.primary) return static_cast<int>(acquisition.error);

    std::string profileError;
    if (!InitializeDebugProfile(profileError))
    {
        if (!ConfirmStartupRecovery("settings.debug.profile.recover", profileError))
            return ERROR_CANCELLED;
        const auto paths = snowdesktop::debug_profile::ResolvePaths(GetDataDirectoryPath());
        snowdesktop::debug_profile::Configuration config;
        std::string recoveryError;
        if (!snowdesktop::debug_profile::Read(paths, config, recoveryError))
            config.desktop = paths.defaultDesktop;
        config.enabled = false;
        config.pendingReset = false;
        if (!snowdesktop::debug_profile::Write(paths, config, recoveryError) ||
            !InitializeDebugProfile(recoveryError))
        {
            if (!ConfirmStartupRecovery("settings.debug.profile.recoveryFailed", recoveryError))
                return ERROR_CANCELLED;
            // A failed persistent recovery must not force the optional debug
            // environment on this run. Preserve its files and retry next time.
            auto& profile = snowdesktop::debug_profile::runtimeSession;
            profile.configuration.enabled = false;
            profile.initialized = false;
            WriteDiagnosticLogEntry(L"Debug-profile recovery was not saved; continuing in normal mode for this process",
                DiagnosticLogLevel::Warning);
        }
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
    snowdesktop::application_restart_policy::RegisterForCurrentProcess();

    /* 创建主应用实例并进入消息循环 */
    int result = 0;
    std::unique_ptr<snowdesktop::single_instance::PreparedRestart> restart;
    {
        DesktopApp app;
        result = app.Run(instance, showCommand);
        restart = app.TakeRestart();
        WriteDiagnosticLogEntry(L"Application run returned; releasing host resources");
    }
    WriteDiagnosticLogEntry(L"Application host resources released");
    if (result != 0 && result != ERROR_CANCELLED)
        ShowStartupFailure({"app.operation.startFailed",
            GetDataFilePath(L"SnowDesktop.log"), static_cast<DWORD>(result)});


    /* 正常退出时清除崩溃计数器，避免残留记录影响后续启动 */
    if (result == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegSubKey, kRegValueName);

    if (restart)
    {
        const DWORD error = result == 0 ? restart->Resume() : ERROR_CANCELLED;
        WriteDiagnosticLogEntry((L"Application restart handoff: pid=" +
            std::to_wstring(GetCurrentProcessId()) + L" child=" +
            std::to_wstring(restart->ProcessId()) + L" error=" +
            std::to_wstring(error)).c_str(), error == ERROR_SUCCESS
                ? DiagnosticLogLevel::Info : DiagnosticLogLevel::Error);
        if (error != ERROR_SUCCESS)
        {
            restart.reset();
            const std::wstring message =
                _LFW("app.run.restart_error", std::to_wstring(error));
            MessageBoxW(nullptr, message.c_str(), _LW("app.run.restart_failed"),
                MB_OK | MB_ICONERROR);
            return static_cast<int>(error);
        }
    }

    return result;
}
