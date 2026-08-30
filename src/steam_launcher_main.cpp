#include "steam_runtime_environment.h"
#include "steam_runtime_manager.h"

#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
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

bool LaunchRuntime(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, DWORD& error)
{
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
        snowdesktop::BuildSnowDesktopDetachedRuntimeEnvironment();
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
                CREATE_BREAKAWAY_FROM_JOB,
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
                    CREATE_UNICODE_ENVIRONMENT,
                environment.data(), workingDirectory.c_str(), &startup,
                &process))
        {
            error = GetLastError();
            return false;
        }
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const std::filesystem::path installRoot = CurrentExecutableDirectory();
    if (installRoot.empty())
        return ERROR_PATH_NOT_FOUND;

    int argumentCount = 0;
    wchar_t** rawArguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!rawArguments)
        return static_cast<int>(GetLastError());
    bool applyOnly = false;
    bool pruneOnly = false;
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
        else
        {
            forwarded.emplace_back(rawArguments[index]);
        }
    }
    LocalFree(rawArguments);

    const auto applied =
        snowdesktop::steam_runtime::ApplyDistribution(installRoot);
    if (!applied.error.empty())
        AppendLauncherLog(installRoot, applied.error);
    if (!applied.ok)
        return ERROR_INSTALL_FAILURE;
    if (pruneOnly)
    {
        snowdesktop::steam_runtime::PruneResult pruned;
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            pruned = snowdesktop::steam_runtime::PruneInactiveRuntimes(
                installRoot, applied.executable);
            if (!pruned.ok)
            {
                AppendLauncherLog(installRoot, pruned.error);
                return ERROR_INSTALL_FAILURE;
            }
            if (pruned.retained == 0)
                return 0;
            Sleep(250);
        }
        AppendLauncherLog(installRoot,
            "inactive Steam runtime remains occupied after handoff");
        return 0;
    }
    if (applyOnly)
        return 0;

    DWORD launchError = ERROR_SUCCESS;
    if (!LaunchRuntime(applied.executable, forwarded, launchError))
    {
        AppendLauncherLog(installRoot,
            "cannot launch runtime (Win32 error " +
                std::to_string(launchError) + ")");
        return static_cast<int>(launchError);
    }
    return 0;
}
