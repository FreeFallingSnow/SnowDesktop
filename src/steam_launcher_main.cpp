#include "steam_runtime_environment.h"
#include "steam_runtime_manager.h"
#include "language_fallback.h"
#include "launcher_messages.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <exception>

namespace
{
std::filesystem::path CurrentExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::wstring QuoteArgument(std::wstring_view argument)
{
    if (!argument.empty() &&
        argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring result(L"\"");
    std::size_t slashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++slashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
        }
        else
        {
            result.append(slashes, L'\\');
            result.push_back(character);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

void AppendLauncherLog(const std::filesystem::path& installRoot,
    std::string_view message)
{
    std::ofstream stream(installRoot / L".snowdesktop" /
        L"launcher.log", std::ios::binary | std::ios::app);
    if (!stream)
        return;
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char prefix[64]{};
    sprintf_s(prefix, "%04u-%02u-%02uT%02u:%02u:%02uZ ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond);
    stream << prefix << message << '\n';
}

void ShowLaunchFailure(const std::filesystem::path& installRoot,
    std::string_view detail)
{
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH);
    char localeUtf8[LOCALE_NAME_MAX_LENGTH * 4]{};
    WideCharToMultiByte(CP_UTF8, 0, localeName, -1, localeUtf8,
        static_cast<int>(sizeof(localeUtf8)), nullptr, nullptr);
    std::vector<std::string> languages;
    for (const auto& entry : kLauncherMessages)
        languages.emplace_back(entry.language);
    std::string language = snowdesktop::localization::ResolveBestLanguage(
        languages, localeUtf8);
    if (language.empty())
        language = "en-US";
    std::wstring message;
    for (const auto& entry : kLauncherMessages)
        if (language == entry.language)
            message = entry.message;
    const int length = MultiByteToWideChar(CP_UTF8, 0, detail.data(),
        static_cast<int>(detail.size()), nullptr, 0);
    std::wstring wideDetail(static_cast<std::size_t>(length), L'\0');
    if (length > 0)
        MultiByteToWideChar(CP_UTF8, 0, detail.data(),
            static_cast<int>(detail.size()), wideDetail.data(), length);
    message += L"\n\n" + wideDetail;
    if (!installRoot.empty())
        message += L"\n\n" + (installRoot / L".snowdesktop" /
            L"launcher.log").wstring();
    MessageBoxW(nullptr, message.c_str(), L"SnowDesktop",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

bool LaunchRuntime(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, unsigned protocol,
    DWORD& error, bool& ready, bool& canRecover, bool& pending)
{
    canRecover = true;
    snowdesktop::steam_runtime::startup::Channel channel;
    if (protocol != 0 && !channel.Create())
    {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    std::wstring command = QuoteArgument(executable.wstring());
    for (const std::wstring& argument : arguments)
    {
        command.push_back(L' ');
        command += QuoteArgument(argument);
    }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> environment =
        snowdesktop::BuildSnowDesktopDetachedRuntimeEnvironment(channel.Name());
    if (environment.empty())
    {
        error = ERROR_BAD_ENVIRONMENT;
        return false;
    }
    const std::wstring workingDirectory =
        executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
            nullptr, FALSE, CREATE_NEW_PROCESS_GROUP |
                CREATE_UNICODE_ENVIRONMENT |
                CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED,
            environment.data(), workingDirectory.c_str(), &startup,
            &process))
    {
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED)
        {
            return false;
        }
        mutableCommand.assign(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        if (!CreateProcessW(executable.c_str(), mutableCommand.data(),
                nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP |
                    CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                environment.data(), workingDirectory.c_str(), &startup,
                &process))
        {
            error = GetLastError();
            return false;
        }
    }
    if (protocol != 0) channel.SetChild(process.dwProcessId);
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
    {
        error = GetLastError();
        // This child has never run. Do not leave a newly created suspended
        // process behind when its initial resume fails.
        TerminateProcess(process.hProcess, ERROR_PROCESS_ABORTED);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }
    CloseHandle(process.hThread);
    if (protocol == 0)
    {
        // Old hosts do not implement readiness. Keep them launchable, but
        // never treat process creation as confirmation or authorize pruning.
        CloseHandle(process.hProcess);
        return true;
    }
    const HANDLE waits[] = {channel.ReadyEvent(), process.hProcess};
    const DWORD waited = WaitForMultipleObjects(2, waits, FALSE, 60000);
    const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    const auto phase = channel.CurrentPhase();
    ready = phase == snowdesktop::steam_runtime::startup::Phase::Ready;
    canRecover = phase < snowdesktop::steam_runtime::startup::Phase::DataAccess;
    DWORD childExit = 0;
    const bool readExit = waited == WAIT_OBJECT_0 + 1 &&
        GetExitCodeProcess(process.hProcess, &childExit) != FALSE;
    CloseHandle(process.hProcess);
    if (ready) return true;
    if (waited == WAIT_TIMEOUT)
    {
        // Leave a slow host alive. Neither downgrade nor clean up while it
        // may still be initializing and accessing shared user data.
        pending = true;
        canRecover = false;
        error = ERROR_TIMEOUT;
        return false;
    }
    if (readExit)
    {
        if (childExit == ERROR_SUCCESS || childExit == ERROR_CANCELLED)
            return true; // activation of an existing instance or user cancellation
        error = childExit;
        return false;
    }
    canRecover = false;
    error = waitError != ERROR_SUCCESS ? waitError : ERROR_PROCESS_ABORTED;
    return false;
}
}

int RunLauncher(bool& maintenance)
{
    const std::filesystem::path installRoot = CurrentExecutableDirectory();
    if (installRoot.empty())
    {
        ShowLaunchFailure(installRoot, "cannot locate the launcher directory");
        return ERROR_PATH_NOT_FOUND;
    }

    int argumentCount = 0;
    wchar_t** rawArguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!rawArguments)
    {
        const DWORD error = GetLastError();
        ShowLaunchFailure(installRoot, "cannot read launch arguments (Win32 error " +
            std::to_string(error) + ")");
        return static_cast<int>(error);
    }
    bool applyOnly = false;
    bool pruneOnly = false;
    bool noUi = false;
    std::vector<std::wstring> forwarded;
    for (int index = 1; index < argumentCount; ++index)
    {
        if (wcscmp(rawArguments[index],
                L"--snowdesktop-launcher-apply-only") == 0)
        {
            applyOnly = true;
        }
        else if (wcscmp(rawArguments[index],
                L"--snowdesktop-launcher-prune-only") == 0)
        {
            pruneOnly = true;
        }
        else if (wcscmp(rawArguments[index], L"--snowdesktop-launcher-no-ui") == 0)
        {
            noUi = true;
        }
        else
        {
            forwarded.emplace_back(rawArguments[index]);
        }
    }
    LocalFree(rawArguments);
    maintenance = applyOnly || pruneOnly || noUi;

    // Legacy hosts provide no runtime identity or readiness acknowledgement.
    // Keep their maintenance invocation compatible without activating or
    // deleting a runtime on behalf of an unknown caller.
    if (pruneOnly)
        return 0;

    auto applied =
        snowdesktop::steam_runtime::ApplyDistribution(installRoot, applyOnly);
    if (!applied.error.empty())
        AppendLauncherLog(installRoot, applied.error);
    if (!applied.ok)
    {
        if (!maintenance)
            ShowLaunchFailure(installRoot, applied.error);
        return ERROR_INSTALL_FAILURE;
    }
    if (applyOnly)
        return 0;

    for (unsigned attempt = 0; attempt < 2; ++attempt)
    {
        DWORD launchError = ERROR_SUCCESS;
        bool ready = false;
        bool canRecover = false;
        bool pending = false;
        if (LaunchRuntime(applied.executable, forwarded, applied.launcherProtocol,
                launchError, ready, canRecover, pending))
        {
            if (ready)
            {
                std::string confirmationError;
                if (snowdesktop::steam_runtime::ConfirmRuntimeStarted(
                        installRoot, applied.executable, confirmationError))
                {
                    const auto pruned = snowdesktop::steam_runtime::PruneInactiveRuntimes(
                        installRoot, applied.executable);
                    if (!pruned.error.empty()) AppendLauncherLog(installRoot, pruned.error);
                    if (pruned.retained != 0)
                        AppendLauncherLog(installRoot, "occupied inactive runtimes retained until a later launch");
                }
                else
                    AppendLauncherLog(installRoot, confirmationError);
            }
            return 0;
        }
        std::string detail = pending
            ? "startup confirmation timed out; the host was left running and all runtimes were preserved"
            : "runtime startup failed (Win32/exit code " + std::to_string(launchError) + "); " +
                (canRecover ? "before data access" : "after data access; automatic downgrade disabled");
        AppendLauncherLog(installRoot, detail);
        if (canRecover && attempt == 0)
        {
            auto recovered = snowdesktop::steam_runtime::RecoverAfterLaunchFailure(
                installRoot, applied.executable);
            AppendLauncherLog(installRoot, recovered.error);
            if (recovered.ok)
            {
                applied = std::move(recovered);
                continue;
            }
            detail += "; " + recovered.error;
        }
        if (!maintenance && !pending) ShowLaunchFailure(installRoot, detail);
        return static_cast<int>(launchError);
    }
    return ERROR_INSTALL_FAILURE;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Loader/critical-error dialogs can block CreateProcess itself, before
    // our exit monitoring starts. Report those failures through our log/UI.
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS |
        SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    bool maintenance = false;
    try
    {
        return RunLauncher(maintenance);
    }
    catch (const std::exception& error)
    {
        const auto root = CurrentExecutableDirectory();
        AppendLauncherLog(root, error.what());
        if (!maintenance)
            ShowLaunchFailure(root, error.what());
        return ERROR_INSTALL_FAILURE;
    }
}
