#include "single_instance.h"
#include "steam_runtime_context.h"
#include "pending_window_message.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <shlobj.h>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

void CheckDataDirectory(std::wstring_view actual,
    const std::filesystem::path& expected, const char* message)
{
    std::error_code canonicalError;
    const auto canonicalExpected =
        std::filesystem::weakly_canonical(expected, canonicalError);
    const auto& comparableExpected = canonicalError
        ? expected
        : canonicalExpected;
    if (snowdesktop::single_instance::DataDirectoriesMatch(
            actual, comparableExpected.wstring()))
    {
        return;
    }
    ++failures;
    std::wcerr << L"FAIL: " << message << L"\n  actual: " << actual
               << L"\n  expected: " << comparableExpected.wstring() << L'\n';
}

void WriteText(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream)
        throw std::runtime_error("cannot write test file");
}

std::filesystem::path PackagedDataRoot(std::wstring_view familyName)
{
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
    {
        return {};
    }
    std::filesystem::path result(localAppData);
    CoTaskMemFree(localAppData);
    return result / L"Packages" / familyName / L"LocalState" / L"data";
}

void WriteManagedSidecar(const std::filesystem::path& runtime)
{
    WriteText(runtime /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-managed\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \"data\",\n"
        "  \"launcherRelative\": \"SnowDesktopLauncher.exe\"\n"
        "}\n");
}

void WriteLocalDevelopmentSidecar(const std::filesystem::path& runtime,
    std::string_view buildId, std::string_view profileId)
{
    WriteText(runtime /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{\n"
        "  \"schemaVersion\": 1,\n"
        "  \"kind\": \"steam-local-dev\",\n"
        "  \"installRootRelative\": \"../../..\",\n"
        "  \"dataRootRelative\": \".snowdesktop/dev-data/" +
            std::string(profileId) + "\",\n"
        "  \"launcherRelative\": \".snowdesktop/dev/" +
            std::string(buildId) + "/SnowDesktop.exe\",\n"
        "  \"profileId\": \"" + std::string(profileId) + "\"\n"
        "}\n");
}

void TestDeploymentDataResolution(const std::filesystem::path& root)
{
    using snowdesktop::single_instance::DataDirectoriesMatch;
    using snowdesktop::single_instance::ResolveInstanceDataDirectory;

    const auto portable = root / L"portable" / L"SnowDesktop.exe";
    WriteText(portable, "portable");
    Check(DataDirectoriesMatch(
            ResolveInstanceDataDirectory(portable.wstring()),
            (portable.parent_path() / L"data").wstring()),
        "a sidecar-free portable instance retains executable-relative data");

    WriteText(portable.parent_path() /
            snowdesktop::deployment::kSteamRuntimeContextFilename,
        "{not-json");
    Check(ResolveInstanceDataDirectory(portable.wstring()).empty(),
        "an invalid explicit Steam sidecar does not fall back to portable data");

    constexpr std::wstring_view packageFamily =
        L"FreeFallingSnow.SnowDesktop.Test_123456789abcd";
    const auto packagedExpected = PackagedDataRoot(packageFamily);
    Check(!packagedExpected.empty() && DataDirectoriesMatch(
            ResolveInstanceDataDirectory(
                portable.wstring(), packageFamily),
            packagedExpected.wstring()),
        "MSIX package identity retains LocalState data beside a sidecar");

    const auto managedInstall = root / L"managed";
    const auto managedRuntime = managedInstall / L".snowdesktop" /
        L"runtime" / L"build-1";
    const auto managedExecutable = managedRuntime / L"SnowDesktop.exe";
    WriteText(managedInstall /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    WriteText(managedExecutable, "managed host");
    WriteManagedSidecar(managedRuntime);
    CheckDataDirectory(
        ResolveInstanceDataDirectory(managedExecutable.wstring()),
        managedInstall / L"data",
        "a managed Steam instance reports the install-root data directory");

    constexpr std::string_view buildId = "build-local";
    constexpr std::string_view profileId = "profile-local";
    const auto localInstall = root / L"local";
    const auto localRuntime = localInstall / L".snowdesktop" / L"dev" /
        std::filesystem::path(buildId);
    const auto localExecutable = localRuntime / L"SnowDesktop.exe";
    WriteText(localExecutable, "local host");
    WriteLocalDevelopmentSidecar(localRuntime, buildId, profileId);
    CheckDataDirectory(
        ResolveInstanceDataDirectory(localExecutable.wstring()),
        localInstall / L".snowdesktop" / L"dev-data" /
            std::filesystem::path(profileId),
        "a local Steam development instance reports its isolated profile data");
}

void TestManagedSteamRuntimeReplacement(const std::filesystem::path& root)
{
    using snowdesktop::single_instance::InstanceInfo;
    using snowdesktop::single_instance::IsManagedSteamRuntimeReplacement;

    const auto install = root / L"managed-replacement";
    WriteText(install /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    const auto oldRuntime = install / L".snowdesktop" /
        L"runtime" / L"build-old";
    const auto newRuntime = install / L".snowdesktop" /
        L"runtime" / L"build-new";
    const auto oldExecutable = oldRuntime / L"SnowDesktop.exe";
    const auto newExecutable = newRuntime / L"SnowDesktop.exe";
    WriteText(oldExecutable, "old host");
    WriteText(newExecutable, "new host");
    WriteManagedSidecar(oldRuntime);
    WriteManagedSidecar(newRuntime);

    InstanceInfo running;
    running.executablePath = oldExecutable.wstring();
    InstanceInfo requested;
    requested.executablePath = newExecutable.wstring();
    Check(IsManagedSteamRuntimeReplacement(running, requested),
        "different immutable runtimes in one managed Steam install require an automatic handoff");

    requested.executablePath = oldExecutable.wstring();
    Check(!IsManagedSteamRuntimeReplacement(running, requested),
        "the same managed Steam runtime remains an ordinary same-instance activation");

    const auto otherInstall = root / L"other-managed-replacement";
    WriteText(otherInstall /
        snowdesktop::deployment::kSteamLauncherFilename, "launcher");
    const auto otherRuntime = otherInstall / L".snowdesktop" /
        L"runtime" / L"build-new";
    const auto otherExecutable = otherRuntime / L"SnowDesktop.exe";
    WriteText(otherExecutable, "other host");
    WriteManagedSidecar(otherRuntime);
    requested.executablePath = otherExecutable.wstring();
    Check(!IsManagedSteamRuntimeReplacement(running, requested),
        "managed Steam runtimes from different installs retain the explicit version-conflict flow");
}

struct Handle
{
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::wstring SelfPath()
{
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (!size || size >= path.size()) throw std::runtime_error("test executable path");
    path.resize(size);
    return path;
}

HANDLE Event(const std::wstring& prefix, const wchar_t* suffix)
{
    return CreateEventW(nullptr, TRUE, FALSE, (prefix + suffix).c_str());
}

constexpr wchar_t kRestartTestEnvironment[] = L"SNOWDESKTOP_RESTART_TEST_PREFIX";

bool RestartDescendantsAreIndependent()
{
    const std::wstring executable = SelfPath();
    std::wstring command = L"\"" + executable + L"\" --restart-leaf";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return false;
    CloseHandle(child.hThread);
    Handle process(child.hProcess);
    const DWORD waited = WaitForSingleObject(process.value, 5000);
    if (waited != WAIT_OBJECT_0)
    {
        TerminateProcess(process.value, ERROR_CANCELLED);
        WaitForSingleObject(process.value, 5000);
        return false;
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    // Query the real immediate job after spawning another process. The private
    // handoff job must have contained only this replacement, not descendants.
    return QueryInformationJobObject(nullptr, JobObjectBasicAccountingInformation,
        &accounting, sizeof(accounting), nullptr) && accounting.TotalProcesses == 1;
}

// These modes run only this test executable: no desktop host or real data.
int RunRestartChild(DWORD predecessor)
{
    wchar_t prefix[256]{};
    if (!GetEnvironmentVariableW(kRestartTestEnvironment, prefix, 256)) return 10;
    Handle started(Event(prefix, L"-started"));
    Handle primary(Event(prefix, L"-primary"));
    if (!started.value || !primary.value || !SetEvent(started.value)) return 11;
    if (!snowdesktop::single_instance::WaitForRestartPredecessor(predecessor, 10000))
        return 12;
    snowdesktop::single_instance::Guard instance;
    if (instance.Acquire((std::wstring(prefix) + L"-mutex").c_str()) !=
        snowdesktop::single_instance::AcquireResult::Primary) return 13;
    const bool independent = RestartDescendantsAreIndependent();
    if (!SetEvent(primary.value)) return 14;
    return independent ? 0 : 15;
}

int RunRestartFixture(const std::wstring& prefix)
{
    if (!SetEnvironmentVariableW(kRestartTestEnvironment, prefix.c_str())) return 20;
    Handle prepared(Event(prefix, L"-prepared"));
    Handle release(Event(prefix, L"-release"));
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(DWORD), (prefix + L"-pid").c_str()));
    if (!prepared.value || !release.value || !mapping.value) return 21;
    auto* childId = static_cast<DWORD*>(MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, sizeof(DWORD)));
    if (!childId) return 22;
    snowdesktop::single_instance::Guard instance;
    if (instance.Acquire((prefix + L"-mutex").c_str()) !=
        snowdesktop::single_instance::AcquireResult::Primary)
    {
        UnmapViewOfFile(childId);
        return 23;
    }
    snowdesktop::single_instance::PreparedRestart restart;
    const DWORD error = restart.Prepare(SelfPath());
    *childId = restart.ProcessId();
    UnmapViewOfFile(childId);
    if (error != ERROR_SUCCESS || !SetEvent(prepared.value)) return 24;
    // Represents old-host teardown, controlled by the test rather than sleep.
    if (WaitForSingleObject(release.value, 10000) != WAIT_OBJECT_0) return 25;
    return static_cast<int>(restart.Resume());
}

void TestRestartHandoff()
{
    using snowdesktop::single_instance::PreparedRestart;
    const std::wstring prefix = L"Local\\SnowDesktopRestartTests-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    Handle prepared(Event(prefix, L"-prepared"));
    Handle release(Event(prefix, L"-release"));
    Handle started(Event(prefix, L"-started"));
    Handle primary(Event(prefix, L"-primary"));
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(DWORD), (prefix + L"-pid").c_str()));
    Check(prepared.value && release.value && started.value && primary.value && mapping.value,
        "restart fixture synchronization objects are available");
    if (!prepared.value || !release.value || !started.value || !primary.value || !mapping.value) return;
    const auto* childId = static_cast<const DWORD*>(MapViewOfFile(mapping.value,
        FILE_MAP_READ, 0, 0, sizeof(DWORD)));
    Check(childId != nullptr, "restart fixture PID mapping is available");
    if (!childId) return;

    // Two handoffs protect repeat restart; a third kills only the isolated
    // preparing fixture, checking that its uncommitted child cannot be orphaned.
    for (int round = 0; round < 3; ++round)
    {
        ResetEvent(prepared.value); ResetEvent(release.value);
        ResetEvent(started.value); ResetEvent(primary.value);
        const std::wstring executable = SelfPath();
        std::wstring command = L"\"" + executable + L"\" --restart-fixture " + prefix;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool launched = CreateProcessW(executable.c_str(), command.data(), nullptr,
            nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE;
        Check(launched, "isolated preparing process starts");
        if (!launched) break;
        CloseHandle(process.hThread);
        Handle fixture(process.hProcess);
        const bool ready = WaitForSingleObject(prepared.value, 10000) == WAIT_OBJECT_0;
        Check(ready, "restart preparation succeeds before teardown");
        if (!ready)
        {
            TerminateProcess(fixture.value, ERROR_CANCELLED);
            WaitForSingleObject(fixture.value, 5000);
            break;
        }
        Handle child(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *childId));
        Check(child.value != nullptr, "prepared child has a stable process handle");
        Check(WaitForSingleObject(started.value, 250) == WAIT_TIMEOUT,
            "replacement must not begin its predecessor timeout while host cleanup is blocked");
        if (round == 2)
        {
            Check(TerminateProcess(fixture.value, ERROR_CANCELLED) != FALSE,
                "isolated preparing fixture can simulate an abrupt exit");
        }
        else SetEvent(release.value);
        Check(WaitForSingleObject(fixture.value, 5000) == WAIT_OBJECT_0,
            "preparing fixture exits after cleanup is released");
        if (round != 2)
        {
            DWORD code = STILL_ACTIVE;
            GetExitCodeProcess(fixture.value, &code);
            Check(code == 0, "prepared restart resumes successfully");
            Check(WaitForSingleObject(primary.value, 10000) == WAIT_OBJECT_0,
                "replacement waits for predecessor then acquires the production single-instance guard");
        }
        if (child.value)
        {
            Check(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0,
                "replacement completes or uncommitted child is cancelled with its parent");
            if (round != 2)
            {
                DWORD code = STILL_ACTIVE;
                GetExitCodeProcess(child.value, &code);
                Check(code != 15, "replacement descendants must not inherit the private restart job");
                Check(code == 0, "replacement exits normally after claiming its isolated guard");
            }
        }
        if (round == 2)
            Check(WaitForSingleObject(started.value, 0) == WAIT_TIMEOUT,
                "an uncommitted restart never runs when its preparing parent dies");
    }
    UnmapViewOfFile(childId);

    HANDLE cancelled = nullptr;
    {
        PreparedRestart restart;
        Check(restart.Prepare(SelfPath() + L".missing") != ERROR_SUCCESS && restart.ProcessId() == 0,
            "launch failure leaves no pending restart and the caller can keep its host running");
        Check(restart.Prepare(SelfPath()) == ERROR_SUCCESS, "failed preparation can be retried");
        const DWORD child = restart.ProcessId();
        Check(restart.Prepare(SelfPath()) == ERROR_ALREADY_EXISTS && restart.ProcessId() == child,
            "a pending request cannot create duplicate restart children");
        cancelled = OpenProcess(SYNCHRONIZE, FALSE, child);
    }
    Handle cancelledChild(cancelled);
    Check(cancelled && WaitForSingleObject(cancelled, 5000) == WAIT_OBJECT_0,
        "discarding a pending request cancels its suspended child");
}

void TestCleanupPreservesQuit()
{
    constexpr UINT completion = WM_APP + 37;
    const HWND window = CreateWindowExW(0, L"STATIC", L"Restart cleanup test", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "isolated completion window is available");
    if (!window) return;
    PostMessageW(window, completion, 0, 123);
    PostQuitMessage(73);
    MSG message{};
    int completions = 0;
    while (snowdesktop::TakePendingWindowMessage(message, window, completion) ==
        snowdesktop::PendingWindowMessage::Ready)
    {
        Check(message.message == completion && message.lParam == 123,
            "cleanup receives only its owned completion, never a quit payload");
        ++completions;
    }
    Check(completions == 1, "pending completion is drained exactly once");
    // Several cleanup paths may drain on the same UI thread before returning
    // to the outer loop; each must leave the same exit request available.
    Check(snowdesktop::TakePendingWindowMessage(message, window, completion) ==
            snowdesktop::PendingWindowMessage::Quit,
        "a second cleanup preserves the pending quit request");
    Check(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) &&
        message.message == WM_QUIT && message.wParam == 73,
        "outer message loop still receives the original exit code after cleanup");
    DestroyWindow(window);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && std::wstring_view(argv[1]) == L"--restart-leaf") return 0;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--restart-fixture")
        return RunRestartFixture(argv[2]);
    if (const DWORD predecessor = snowdesktop::single_instance::
        ParseRestartPredecessorProcessId(GetCommandLineW()))
        return RunRestartChild(predecessor);
    const auto root = std::filesystem::temp_directory_path() /
        (L"SnowDesktopSingleInstanceTests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::error_code cleanupError;
    try
    {
        TestDeploymentDataResolution(root);
        TestManagedSteamRuntimeReplacement(root);
        TestRestartHandoff();
        TestCleanupPreservesQuit();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::remove_all(root, cleanupError);
    if (cleanupError)
    {
        std::cerr << "FAIL: temporary test cleanup failed: "
                  << cleanupError.message() << '\n';
        ++failures;
    }
    if (failures == 0)
        std::cout << "single_instance_tests: passed\n";
    return failures == 0 ? 0 : 1;
}
