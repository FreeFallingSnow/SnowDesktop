#include "platform/auto_start_manager.h"
#include "platform/auto_start_launch.h"
#include "platform/deployment_context.h"
#include "diagnostics/diagnostic_log.h"

#include <windows.h>
#include <sddl.h>
#include <aclapi.h>
#include <tlhelp32.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <iostream>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

// Host identity/logging and the MSIX registry bridge are substituted. Scheduler
// operations and the portable registry adapter use real Windows APIs; only GUID
// namespaces are mutated, never the user's actual login registrations.
namespace snowdesktop::deployment
{
const RuntimeDeploymentContext& GetRuntimeDeploymentContext() noexcept
{
    static const RuntimeDeploymentContext context;
    return context;
}
bool CanOwnProductionAutoStart(RuntimeDeploymentKind kind) noexcept
{
    return kind == RuntimeDeploymentKind::Portable;
}
UnvirtualizedRegistryValue QueryUnvirtualizedCurrentUserValue(
    const wchar_t* key, const wchar_t* name) noexcept
{
    UnvirtualizedRegistryValue result;
    DWORD type = 0, size = static_cast<DWORD>(result.data.size());
    result.win32Result = RegGetValueW(HKEY_CURRENT_USER, key, name,
        RRF_RT_ANY | RRF_NOEXPAND, &type, result.data.data(), &size);
    result.type = type;
    result.size = size;
    return result;
}
std::uint32_t SetUnvirtualizedCurrentUserValue(const wchar_t* key,
    const wchar_t* name, std::uint32_t type, const void* data, std::uint32_t size) noexcept
{
    return RegSetKeyValueW(HKEY_CURRENT_USER, key, name, type, data, size);
}
std::uint32_t DeleteUnvirtualizedCurrentUserValue(const wchar_t* key, const wchar_t* name) noexcept
{
    return RegDeleteKeyValueW(HKEY_CURRENT_USER, key, name);
}
}

void WriteDiagnosticLogEntry(const wchar_t* message, DiagnosticLogLevel)
{
    // Keep system-localized error text observable in captured test logs.
    const int length = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8.data(), length, nullptr, nullptr);
    std::cerr << utf8.c_str() << '\n';
}

namespace
{
using Microsoft::WRL::ComPtr;
using namespace snowdesktop::auto_start;
using snowdesktop::UnifiedAutoStartTaskState;

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

HWND permissionTestWindow = nullptr;
HWND lowPermissionTestWindow = nullptr;
HANDLE permissionTestChild = nullptr;
std::wstring creationCommand, creationDirectory;
DWORD permissionTestParentFailure = 0;
int permissionTestParentCalls = 0;
bool permissionTestOmitParent = false;

HWND WINAPI PermissionTestShellWindow() { return permissionTestWindow; }
HWND WINAPI LowPermissionTestShellWindow() { return lowPermissionTestWindow; }

DWORD PermissionTestParentId()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD parent = 0;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == GetCurrentProcessId())
            {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent;
}

BOOL RetainPermissionTestProcess(LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES processSecurity, LPSECURITY_ATTRIBUTES threadSecurity,
    BOOL inheritHandles, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    const BOOL created = CreateProcessW(application, command, processSecurity, threadSecurity,
        inheritHandles, flags, environment, directory, startup, process);
    if (created)
    {
        if (permissionTestChild) CloseHandle(permissionTestChild);
        Require(DuplicateHandle(GetCurrentProcess(), process->hProcess, GetCurrentProcess(),
            &permissionTestChild, 0, FALSE, DUPLICATE_SAME_ACCESS) != FALSE,
            "retain isolated permission child until assertions finish");
    }
    return created;
}

BOOL WINAPI CreatePermissionTestParentProcess(LPCWSTR application, LPWSTR command,
    LPSECURITY_ATTRIBUTES processSecurity, LPSECURITY_ATTRIBUTES threadSecurity,
    BOOL inheritHandles, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION process)
{
    ++permissionTestParentCalls;
    Require((flags & (EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
        CREATE_UNICODE_ENVIRONMENT)) == (EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED |
        CREATE_UNICODE_ENVIRONMENT) && !inheritHandles &&
        startup->cb == sizeof(STARTUPINFOEXW), "single creation path supplies suspended desktop-parent startup attributes");
    creationCommand = command;
    creationDirectory = directory;
    if (permissionTestParentFailure)
    {
        SetLastError(permissionTestParentFailure);
        return FALSE;
    }
    if (permissionTestOmitParent)
    {
        STARTUPINFOW ordinary = *startup;
        ordinary.cb = sizeof(ordinary);
        return RetainPermissionTestProcess(application, command, processSecurity, threadSecurity,
            inheritHandles, flags & ~EXTENDED_STARTUPINFO_PRESENT, environment, directory, &ordinary, process);
    }
    // Observe the real Windows parent-process attributes and creation API.
    return RetainPermissionTestProcess(application, command, processSecurity, threadSecurity,
        inheritHandles, flags, environment, directory, startup, process);
}

void CheckLowerIntegrityParent(const std::wstring& executable, const std::filesystem::path& directory)
{
    namespace launch = snowdesktop::auto_start_launch;
    // Only this GUID test directory becomes writable by the low-integrity
    // fixture. The user's shell, account, registrations and files are untouched.
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)",
        SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "create isolated low-integrity directory label");
    BOOL present = FALSE, defaulted = FALSE;
    PACL label = nullptr;
    const bool parsed = GetSecurityDescriptorSacl(descriptor, &present, &label, &defaulted) != FALSE;
    const DWORD applied = parsed && present ? SetNamedSecurityInfoW(
        const_cast<LPWSTR>(directory.c_str()), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, label) : ERROR_INVALID_DATA;
    LocalFree(descriptor);
    Require(applied == ERROR_SUCCESS, "label only the isolated test directory for lower-integrity reports");
    std::wstring fixtureCommand = L"\"" + executable + L"\" --autostart-low-shell \"" + directory.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION fixture{};
    Require(CreateProcessW(executable.c_str(), fixtureCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &fixture) != FALSE, "start isolated lower-integrity parent fixture");
    struct Cleanup
    {
        PROCESS_INFORMATION process;
        std::filesystem::path directory;
        ~Cleanup()
        {
            std::ofstream(directory / L"stop.txt").close();
            if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0)
                TerminateProcess(process.hProcess, ERROR_TIMEOUT);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            lowPermissionTestWindow = nullptr;
            permissionTestOmitParent = false;
            std::error_code ignored;
            for (const auto name : {L"shell.txt", L"stop.txt", L"child report.txt"})
                std::filesystem::remove(directory / name, ignored);
        }
    } cleanup{fixture, directory};
    const ULONGLONG deadline = GetTickCount64() + 10000;
    while (!lowPermissionTestWindow && GetTickCount64() < deadline)
    {
        std::ifstream report(directory / L"shell.txt");
        std::uintptr_t window = 0;
        if (report >> window) lowPermissionTestWindow = reinterpret_cast<HWND>(window);
        if (lowPermissionTestWindow || WaitForSingleObject(fixture.hProcess, 0) != WAIT_TIMEOUT) break;
        Sleep(20);
    }
    Require(lowPermissionTestWindow != nullptr, "lower-integrity fixture reports its hidden test window");
    DWORD windowProcess = 0;
    GetWindowThreadProcessId(lowPermissionTestWindow, &windowProcess);
    Require(windowProcess == fixture.dwProcessId, "desktop-parent fixture belongs to the isolated process");
    launch::NativePlatform native(&CreatePermissionTestParentProcess, &LowPermissionTestShellWindow);
    const auto desktop = native.DesktopIdentity();
    Require(desktop.integrity == SECURITY_MANDATORY_LOW_RID && !desktop.elevated,
        "isolated parent really runs below the caller's integrity");
    const auto reportPath = directory / L"child report.txt";
    const std::wstring command = L"\"" + executable + L"\" --autostart-permission-probe \"" +
        reportPath.wstring() + L"\" --snowdesktop-autostart-owner=steam \"保留 spaces\"";
    permissionTestOmitParent = true;
    bool denied = false;
    try { (void)native.Relaunch(desktop, command); }
    catch (const launch::Failure& failure) { denied = failure.code == ERROR_ACCESS_DENIED; }
    Require(denied && WaitForSingleObject(permissionTestChild, 10000) == WAIT_OBJECT_0 &&
        !std::filesystem::exists(reportPath), "omitting the desktop parent fails the real lower-integrity assertion before resume");
    permissionTestOmitParent = false;
    const DWORD processId = native.Relaunch(desktop, command);
    Require(processId != 0 && WaitForSingleObject(permissionTestChild, 10000) == WAIT_OBJECT_0,
        "actual lower-integrity desktop-parent child completes");
    DWORD exitCode = 1;
    const bool readExit = GetExitCodeProcess(permissionTestChild, &exitCode) != FALSE;
    if (!readExit || exitCode != 0) std::cerr << "Lower-integrity child exit code: " << exitCode << '\n';
    Require(readExit && exitCode == 0,
        "lower-integrity child can query its own token and write the isolated report");
    std::ifstream input(reportPath, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
    Require(!bytes.empty() && bytes.size() % sizeof(wchar_t) == 0, "lower-integrity child report is complete");
    std::wstring observed(bytes.size() / sizeof(wchar_t), L'\0');
    std::memcpy(observed.data(), bytes.data(), bytes.size());
    const std::wstring expected = command + L"\n" + creationDirectory + L"\n" +
        std::to_wstring(desktop.session) + L"," + std::to_wstring(desktop.integrity) + L",0\n" +
        std::to_wstring(fixture.dwProcessId);
    Require(observed == expected, "real child inherits the separate lower-integrity parent and unchanged startup inputs");
    std::cout << "Desktop-parent launch: actual lower-integrity child and missing-parent negative control passed\n";
}

void CheckAutoStartNativeHandoff(const std::wstring& executable)
{
    namespace launch = snowdesktop::auto_start_launch;
    permissionTestWindow = CreateWindowExW(0, L"STATIC", L"SnowDesktop permission test shell",
        0, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Require(permissionTestWindow != nullptr, "create hidden isolated shell-token fixture");
    GUID guid{};
    Require(SUCCEEDED(CoCreateGuid(&guid)), "create permission report identity");
    wchar_t suffix[40]{};
    StringFromGUID2(guid, suffix, 40);
    const auto directory = std::filesystem::temp_directory_path() /
        (L"SnowDesktop permission 测试 " + std::wstring(suffix));
    Require(std::filesystem::create_directory(directory), "create isolated permission report directory");
    struct Cleanup
    {
        std::filesystem::path directory;
        ~Cleanup()
        {
            if (permissionTestChild) CloseHandle(permissionTestChild);
            permissionTestChild = nullptr;
            DestroyWindow(permissionTestWindow);
            permissionTestWindow = nullptr;
            permissionTestParentFailure = 0;
            permissionTestParentCalls = 0;
            permissionTestOmitParent = false;
            std::error_code ignored;
            std::filesystem::remove(directory / L"child report.txt", ignored);
            std::filesystem::remove(directory, ignored);
        }
    } cleanup{directory};
    struct Platform
    {
        launch::NativePlatform native{&CreatePermissionTestParentProcess, &PermissionTestShellWindow};
        launch::Identity CurrentIdentity()
        {
            auto identity = native.CurrentIdentity();
            // Supply the reported elevated starting state without showing UAC.
            identity.elevated = true;
            identity.integrity = SECURITY_MANDATORY_HIGH_RID;
            return identity;
        }
        launch::Identity DesktopIdentity() { return native.DesktopIdentity(); }
        DWORD Relaunch(const launch::Identity& desktop, std::wstring_view command)
        {
            return native.Relaunch(desktop, command);
        }
    } platform;
    const auto desktop = platform.native.CurrentIdentity();
    const auto report = directory / L"child report.txt";
    const std::wstring command = L"\"" + executable + L"\" --autostart-permission-probe \"" +
        report.wstring() + L"\" --snowdesktop-autostart-owner=steam \"保留 spaces\"";
    if (desktop.elevated || desktop.integrity >= SECURITY_MANDATORY_HIGH_RID)
    {
        const auto result = launch::Normalize(command, platform);
        Require(!result.handled && !permissionTestChild,
            "an elevated isolated shell fixture is not mistaken for a normal desktop");
        std::cout << "Native lower-token handoff unavailable in elevated test context; policy checks remain active\n";
        return;
    }
    {
        permissionTestParentCalls = 0;
        Platform attempt;
        const auto result = launch::Normalize(command, attempt);
        Require(result.handled && result.error == ERROR_SUCCESS && result.childProcessId != 0,
            "reported elevated startup reaches the real token handoff and child resume");
        Require(permissionTestParentCalls == 1,
            "elevated login uses exactly one desktop-parent process creation");
        Require(creationCommand == command && creationDirectory == std::filesystem::current_path().wstring(),
            "launch keeps the complete Unicode command and current directory");
        Require(WaitForSingleObject(permissionTestChild, 10000) == WAIT_OBJECT_0,
            "resumed isolated child completes");
        DWORD exitCode = 1;
        Require(GetExitCodeProcess(permissionTestChild, &exitCode) && exitCode == 0,
            "resumed child writes its own observed startup inputs");
        std::ifstream input(report, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        input.close();
        Require(!bytes.empty() && bytes.size() % sizeof(wchar_t) == 0,
            "child observation is present and complete");
        std::wstring observed(bytes.size() / sizeof(wchar_t), L'\0');
        std::memcpy(observed.data(), bytes.data(), bytes.size());
        const std::wstring expected = command + L"\n" + creationDirectory + L"\n" +
            std::to_wstring(desktop.session) + L"," + std::to_wstring(desktop.integrity) + L",0\n" +
            std::to_wstring(GetCurrentProcessId());
        Require(observed == expected,
            "child independently observes unchanged arguments/directory and normal desktop identity");
        Require(std::filesystem::remove(report), "remove completed child report before next attempt");
    }

    // Creation failures remain visible and never retry under elevated identity.
    for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_PRIVILEGE_NOT_HELD, ERROR_BAD_EXE_FORMAT})
    {
        permissionTestParentFailure = error;
        permissionTestParentCalls = 0;
        Platform failed;
        const auto result = launch::Normalize(command, failed);
        Require(result.handled && result.error == error && result.childProcessId == 0 &&
            permissionTestParentCalls == 1 && !std::filesystem::exists(report) &&
            std::wstring_view(result.operation) == L"CreateProcessW(autostart desktop parent)",
            "a creation failure exits once with its original operation/code and no elevated fallback");
    }
    permissionTestParentFailure = 0;

    // An unexpected child token must be rejected while its primary thread is
    // still suspended, before it can write the supplied report or start a host.
    launch::NativePlatform rejected(&CreatePermissionTestParentProcess, &PermissionTestShellWindow);
    auto mismatch = rejected.DesktopIdentity();
    mismatch.integrity = SECURITY_MANDATORY_HIGH_RID;
    bool denied = false;
    try { (void)rejected.Relaunch(mismatch, command); }
    catch (const launch::Failure& failure) { denied = failure.code == ERROR_ACCESS_DENIED; }
    Require(denied && WaitForSingleObject(permissionTestChild, 10000) == WAIT_OBJECT_0 &&
        !std::filesystem::exists(report), "wrong-token child is never resumed and cannot commit side effects");
    CheckLowerIntegrityParent(executable, directory);
}

struct Bstr
{
    BSTR value;
    explicit Bstr(const std::wstring& text) : value(SysAllocString(text.c_str())) {}
    ~Bstr() { SysFreeString(value); }
};

// A unique folder is the entire mutation boundary; never use the product folder.
struct Fixture
{
    std::wstring path;
    std::wstring registryRoot;
    ComPtr<ITaskService> service;
    Fixture()
    {
        GUID guid{};
        Require(SUCCEEDED(CoCreateGuid(&guid)), "create isolated task identity");
        wchar_t text[40]{};
        StringFromGUID2(guid, text, 40);
        path = L"\\SnowDesktopAutoStartTest-" + std::wstring(text);
        registryRoot = L"Software\\SnowDesktopAutoStartTest-" + std::wstring(text);
        Require(SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))), "activate scheduler");
        VARIANT empty{};
        Require(SUCCEEDED(service->Connect(empty, empty, empty, empty)), "connect scheduler");
    }
    void Cleanup()
    {
        const auto registryDeleted = RegDeleteTreeW(HKEY_CURRENT_USER, registryRoot.c_str());
        Require(registryDeleted == ERROR_SUCCESS || registryDeleted == ERROR_FILE_NOT_FOUND,
            "clean isolated registry subtree");
        ComPtr<ITaskFolder> folder;
        if (SUCCEEDED(service->GetFolder(Bstr(path).value, &folder)))
        {
            const HRESULT deleted = folder->DeleteTask(Bstr(L"Startup").value, 0);
            Require(SUCCEEDED(deleted) || deleted == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "clean isolated task");
            ComPtr<ITaskFolder> root;
            Require(SUCCEEDED(service->GetFolder(Bstr(L"\\").value, &root)), "open cleanup root");
            Require(SUCCEEDED(root->DeleteFolder(Bstr(path).value, 0)), "clean isolated folder");
        }
    }
    ~Fixture()
    {
        try { Cleanup(); }
        catch (const std::exception& error)
        {
            std::cerr << "Cleanup failed: " << error.what() << '\n';
            std::wcerr << path << L'\n';
            std::terminate();
        }
    }
    void ReplaceArguments(const wchar_t* arguments)
    {
        ComPtr<ITaskFolder> folder;
        ComPtr<IRegisteredTask> task;
        ComPtr<ITaskDefinition> definition;
        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        ComPtr<IExecAction> execute;
        Require(SUCCEEDED(service->GetFolder(Bstr(path).value, &folder)), "open fixture folder");
        Require(SUCCEEDED(folder->GetTask(Bstr(L"Startup").value, &task)), "open fixture task");
        Require(SUCCEEDED(task->get_Definition(&definition)), "read fixture definition");
        Require(SUCCEEDED(definition->get_Actions(&actions)), "read fixture actions");
        Require(SUCCEEDED(actions->get_Item(1, &action)), "read fixture action");
        Require(SUCCEEDED(action.As(&execute)), "read fixture exec action");
        Require(SUCCEEDED(execute->put_Arguments(Bstr(arguments).value)), "change fixture arguments");
        VARIANT empty{};
        ComPtr<IRegisteredTask> updated;
        Require(SUCCEEDED(folder->RegisterTaskDefinition(Bstr(L"Startup").value,
            definition.Get(), TASK_UPDATE, empty, empty,
            TASK_LOGON_INTERACTIVE_TOKEN, empty, &updated)), "save stale fixture");
    }
};

// Deny only task creation in our empty test folder. Preserve read, delete and
// WRITE_DAC, and restore the original DACL before the fixture removes the folder.
struct DenyTaskCreation
{
    ComPtr<ITaskFolder> folder;
    BSTR original = nullptr;
    explicit DenyTaskCreation(Fixture& fixture)
    {
        Require(SUCCEEDED(fixture.service->GetFolder(Bstr(fixture.path).value, &folder)), "open ACL fixture");
        Require(SUCCEEDED(folder->GetSecurityDescriptor(DACL_SECURITY_INFORMATION, &original)), "save folder DACL");
        std::wstring denied(original);
        const auto firstAce = denied.find(L'(');
        Require(firstAce != std::wstring::npos, "fixture DACL contains ACEs");
        denied.insert(firstAce, L"(D;;0x2;;;WD)");
        Require(SUCCEEDED(folder->SetSecurityDescriptor(Bstr(denied).value, 0)), "deny creation in isolated folder");
    }
    ~DenyTaskCreation()
    {
        const auto restored = folder->SetSecurityDescriptor(original, 0);
        SysFreeString(original);
        if (FAILED(restored)) std::terminate();
    }
    void Restore()
    {
        Require(SUCCEEDED(folder->SetSecurityDescriptor(original, 0)), "restore fixture write access");
    }
};

struct DenyRegistryWrites
{
    HKEY key = nullptr;
    std::vector<BYTE> original;
    explicit DenyRegistryWrites(const std::wstring& path)
    {
        Require(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS, "create isolated denied registry key");
        DWORD size = 0;
        Require(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, nullptr, &size) == ERROR_INSUFFICIENT_BUFFER,
            "size registry DACL");
        original.resize(size);
        Require(RegGetKeySecurity(key, DACL_SECURITY_INFORMATION, original.data(), &size) == ERROR_SUCCESS,
            "save registry DACL");
        LPWSTR sddl = nullptr;
        Require(ConvertSecurityDescriptorToStringSecurityDescriptorW(original.data(), SDDL_REVISION_1,
            DACL_SECURITY_INFORMATION, &sddl, nullptr), "convert registry DACL");
        std::wstring denied(sddl);
        LocalFree(sddl);
        const auto firstAce = denied.find(L'(');
        Require(firstAce != std::wstring::npos, "registry DACL has ACEs");
        denied.insert(firstAce, L"(D;;0x2;;;WD)");
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(denied.c_str(), SDDL_REVISION_1,
            &descriptor, nullptr), "parse denied registry DACL");
        const auto result = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, descriptor);
        LocalFree(descriptor);
        Require(result == ERROR_SUCCESS, "deny registry set-value access");
    }
    ~DenyRegistryWrites()
    {
        const auto result = RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, original.data());
        RegCloseKey(key);
        if (result != ERROR_SUCCESS) std::terminate();
    }
};
}

int RunAutoStartLowShellProbe()
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_INVALID_DATA;
    struct Arguments
    {
        LPWSTR* value;
        ~Arguments() { LocalFree(value); }
    } cleanup{arguments};
    if (count != 3) return ERROR_INVALID_DATA;
    try
    {
        const std::filesystem::path directory(arguments[2]);
        snowdesktop::auto_start_launch::Handle token;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &token.value))
            return static_cast<int>(GetLastError());
        BYTE sid[SECURITY_MAX_SID_SIZE]{};
        DWORD sidSize = sizeof(sid);
        if (!CreateWellKnownSid(WinLowLabelSid, nullptr, sid, &sidSize))
            return static_cast<int>(GetLastError());
        TOKEN_MANDATORY_LABEL label{{sid, SE_GROUP_INTEGRITY}};
        if (!SetTokenInformation(token.value, TokenIntegrityLevel, &label,
            static_cast<DWORD>(sizeof(label)) + sidSize))
            return static_cast<int>(GetLastError());
        const HWND window = CreateWindowExW(0, L"STATIC", L"SnowDesktop isolated low parent",
            0, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window) return static_cast<int>(GetLastError());
        {
            std::ofstream report(directory / L"shell.txt");
            report << reinterpret_cast<std::uintptr_t>(window);
            report.close();
            if (!report) { DestroyWindow(window); return ERROR_WRITE_FAULT; }
        }
        const ULONGLONG deadline = GetTickCount64() + 30000;
        while (!std::filesystem::exists(directory / L"stop.txt") && GetTickCount64() < deadline)
            Sleep(20);
        DestroyWindow(window);
        return ERROR_SUCCESS;
    }
    catch (...) { return ERROR_GEN_FAILURE; }
}

int RunAutoStartLaunchProbe()
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return ERROR_INVALID_DATA;
    struct Arguments
    {
        LPWSTR* value;
        ~Arguments() { LocalFree(value); }
    } cleanup{arguments};
    if (count != 5) return ERROR_INVALID_DATA;
    try
    {
        snowdesktop::auto_start_launch::NativePlatform platform;
        const auto identity = platform.CurrentIdentity();
        const std::wstring observed = std::wstring(GetCommandLineW()) + L"\n" +
            std::filesystem::current_path().wstring() + L"\n" +
            std::to_wstring(identity.session) + L"," + std::to_wstring(identity.integrity) +
            L"," + std::to_wstring(identity.elevated ? 1 : 0) + L"\n" +
            std::to_wstring(PermissionTestParentId());
        std::ofstream output(std::filesystem::path(arguments[2]), std::ios::binary);
        output.write(reinterpret_cast<const char*>(observed.data()),
            static_cast<std::streamsize>(observed.size() * sizeof(wchar_t)));
        output.close();
        return output ? 0 : ERROR_WRITE_FAULT;
    }
    catch (const snowdesktop::auto_start_launch::Failure& failure)
    {
        return static_cast<int>(failure.code);
    }
    catch (...) { return ERROR_GEN_FAILURE; }
}

int RunAutoStartManagerTests()
{
    try
    {
        Target target;
        target.owner = snowdesktop::UnifiedAutoStartOwner::Portable;
        wchar_t executable[32768]{};
        Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "get test executable");
        target.executable = executable;
        target.arguments = L"--snowdesktop-autostart-owner=portable";

        CheckAutoStartNativeHandoff(target.executable);

        bool failedWithDetails = false;
        std::thread uninitialized([&] {
            std::wstring error;
            const TaskStore store(L"\\SnowDesktopAutoStartTest-NoCom");
            const bool applied = store.Configure(target, true, L"test", &error);
            failedWithDetails = !applied &&
                error.find(L"CoCreateInstance(TaskScheduler)") != std::wstring::npos &&
                error.find(L"0x800401F0") != std::wstring::npos;
            const State state = store.Query();
            failedWithDetails = failedWithDetails && !state.error.empty() &&
                state.status == UnifiedAutoStartTaskState::Unavailable;
        });
        uninitialized.join();
        Require(failedWithDetails, "COM failures preserve the failed operation and exact HRESULT");

        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize COM");
        struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
        Fixture fixture;
        TaskStore store(fixture.path);
        std::wstring error;
        Require(store.Query().status == UnifiedAutoStartTaskState::Missing, "isolated task starts missing");
        Require(store.Configure(target, false, L"test", &error), "create disabled task");
        Require(error.empty() && store.Query().status == UnifiedAutoStartTaskState::Disabled, "disabled state verified");

        // Regression: a real readable task with stale parameters formerly made
        // Configure return before RegisterTaskDefinition could repair it.
        fixture.ReplaceArguments(L"--legacy-startup");
        Require(store.Query().status == UnifiedAutoStartTaskState::Unavailable, "stale task reproduces unknown classification");
        Require(store.Configure(target, true, L"test", &error), "explicit enable repairs stale task");
        Require(store.Query().status == UnifiedAutoStartTaskState::Enabled, "repaired task is enabled");
        fixture.ReplaceArguments(L"  --snowdesktop-autostart-owner=portable  --extra");
        const auto spaced = store.Query();
        Require(spaced.status == UnifiedAutoStartTaskState::Enabled &&
            spaced.target.owner == snowdesktop::UnifiedAutoStartOwner::Portable,
            "whitespace and unrelated arguments do not hide an active task");
        fixture.ReplaceArguments(L"--legacy-startup");
        Require(store.Configure(target, false, L"test", &error), "explicit disable repairs stale task");
        Require(store.Query().status == UnifiedAutoStartTaskState::Disabled, "stale task is disabled");

        Require(store.Configure(target, false,
            L"SnowDesktop auto-start migration pending; desired state: enabled.", &error), "seed interrupted migration");
        Require(store.Query().migrationPending, "fixture is migration pending");
        Require(store.Configure(target, false, L"test", &error), "explicit choice replaces migration intent");
        const auto after = store.Query();
        Require(!after.migrationPending && after.status == UnifiedAutoStartTaskState::Disabled,
            "old migration cannot re-enable the user's disabled task");

        Target invalid;
        Require(!store.Configure(invalid, true, L"test", &error) &&
            error.find(L"AutoStart.Target") != std::wstring::npos &&
            error.find(L"0x80070057") != std::wstring::npos,
            "invalid target returns a stage and HRESULT");
        Require(store.Query().status == UnifiedAutoStartTaskState::Disabled, "failed request preserves actual task state");
        Require(store.Delete(&error), "delete isolated task");
        Require(store.SetEnabled(false, &error) && error.empty(), "disabling a missing task is successful");
        Require(!store.SetEnabled(true, &error) && error.find(L"0x80070002") != std::wstring::npos,
            "missing task enable returns the concrete Windows error");

        const RunStore run(fixture.registryRoot + L"\\Run", fixture.registryRoot + L"\\Approval");
        const LoginStore login(store, run);
        int elevationRequests = 0;
        const LoginStore cancelledElevation(store, run, [&](const Target&, bool, std::wstring* detail) {
            ++elevationRequests;
            *detail = DescribeError(L"ShellExecuteExW(runas startup)", HRESULT_FROM_WIN32(ERROR_CANCELLED));
            return false;
        });
        Target steam = target;
        steam.owner = snowdesktop::UnifiedAutoStartOwner::Steam;
        steam.arguments = L"--snowdesktop-autostart-owner=steam";
        {
            DenyTaskCreation denied(fixture);
            Require(!store.Configure(steam, true, L"test", &error) &&
                error.find(L"RegisterTaskDefinition") != std::wstring::npos &&
                error.find(L"0x80070005") != std::wstring::npos,
                "reproduce Steam registration access denied through real scheduler ACL");
            Require(login.Configure(steam, true, &error) && error.empty(),
                "explicit enable falls back after real scheduler access denial");
            const auto restarted = LoginStore(store, run).Query();
            Require(restarted.status == UnifiedAutoStartTaskState::Enabled &&
                restarted.target.executable == steam.executable && restarted.target.arguments == steam.arguments &&
                restarted.target.owner == steam.owner && !restarted.migrationPending,
                "new query reconstructs active Steam fallback without migration");
            const auto disabled = snowdesktop::BuildPortableAutoStartApprovalPayload(false, 1);
            Require(RegSetKeyValueW(HKEY_CURRENT_USER, (fixture.registryRoot + L"\\Approval").c_str(),
                L"SnowDesktopFallback", REG_BINARY, disabled.data(), static_cast<DWORD>(disabled.size())) == ERROR_SUCCESS,
                "simulate Windows disabling fallback");
            Require(login.Query().status == UnifiedAutoStartTaskState::Disabled, "read Windows disabled state");
            Require(login.Configure(steam, true, &error) && login.Query().status == UnifiedAutoStartTaskState::Enabled,
                "explicit re-enable repairs Windows disabled approval");
            Require(login.Configure(steam, false, &error) &&
                LoginStore(store, run).Query().status == UnifiedAutoStartTaskState::Disabled,
                "disable succeeds while scheduler registration remains denied");
            Require(cancelledElevation.Configure(steam, true, &error) && elevationRequests == 1 &&
                run.Query().status == UnifiedAutoStartTaskState::Enabled,
                "permission denial asks for elevation once, then falls back after cancellation");
            Require(login.Configure(steam, false, &error), "restore disabled fallback for failed write check");
            {
            DenyRegistryWrites deniedRun(fixture.registryRoot + L"\\Run");
            Require(!cancelledElevation.Configure(steam, true, &error) && elevationRequests == 2 &&
                error.find(L"RegisterTaskDefinition") != std::wstring::npos &&
                error.find(L"0x80070005") != std::wstring::npos &&
                error.find(L"0x800704C7") != std::wstring::npos &&
                error.find(L"RegSetValueExW(HKCU\\") != std::wstring::npos,
                "both failed mechanisms retain their concrete operation and error");
            Require(run.Query().status == UnifiedAutoStartTaskState::Disabled,
                "failed command write must not activate a retained disabled command");
            }
            {
            DenyRegistryWrites deniedApproval(fixture.registryRoot + L"\\Approval");
            Require(!login.Configure(steam, false, &error) &&
                run.Query().status == UnifiedAutoStartTaskState::Missing,
                "failed approval disable still removes Run instead of leaving it active");
            }
            Require(login.Configure(steam, true, &error), "restore enabled fallback before task recovery");
        }
        Require(login.Configure(steam, true, &error) && store.Query().status == UnifiedAutoStartTaskState::Enabled &&
            run.Query().status == UnifiedAutoStartTaskState::Missing, "recovered scheduler removes duplicate fallback");
        Require(run.Configure(steam, true, &error), "seed both mechanisms active");
        Require(login.Configure(steam, false, &error) && store.Query().status == UnifiedAutoStartTaskState::Disabled &&
            run.Query().status == UnifiedAutoStartTaskState::Missing, "disable clears both active mechanisms");
        Target tooLong = steam;
        tooLong.executable = L"C:\\" + std::wstring(260, L'x') + L".exe";
        Require(!run.Configure(tooLong, true, &error) && error.find(L"0x800700CE") != std::wstring::npos &&
            run.Query().status == UnifiedAutoStartTaskState::Missing, "long Run commands fail explicitly without truncation");

        Require(store.Delete(&error) && run.Configure(steam, false, &error), "prepare successful elevation boundary");
        {
            DenyTaskCreation denied(fixture);
            int requests = 0;
            const LoginStore elevated(store, run, [&](const Target& requested, bool enabled, std::wstring* detail) {
                ++requests;
                Require(run.Query().status == UnifiedAutoStartTaskState::Disabled,
                    "try elevation before writing the fallback");
                denied.Restore(); // Substitute only the UAC/privilege boundary.
                return store.Configure(requested, enabled, L"test", detail);
            });
            Require(elevated.Configure(steam, true, &error) && requests == 1 &&
                store.Query().status == UnifiedAutoStartTaskState::Enabled &&
                run.Query().status == UnifiedAutoStartTaskState::Missing,
                "successful elevated registration is verified and removes fallback");
            Require(elevated.Configure(steam, false, &error) && requests == 1,
                "normal successful writes never prompt for elevation");
            Require(!elevated.Configure(Target{}, true, &error) && requests == 1,
                "invalid targets do not prompt for elevation");
        }

        // Exercise the actual helper entry point without displaying UAC or
        // creating a production task: malformed/missing IPC is always terminal.
        for (const auto suffix : {L"", L"invalid/path", L"1.{00000000-0000-0000-0000-000000000000}"})
        {
            std::wstring command = L"\"" + target.executable + L"\" --snowdesktop-startup-elevated=" + suffix;
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION child{};
            Require(CreateProcessW(target.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child), "launch isolated malformed helper");
            CloseHandle(child.hThread);
            const auto waited = WaitForSingleObject(child.hProcess, 10000);
            DWORD exitCode = 0;
            GetExitCodeProcess(child.hProcess, &exitCode);
            CloseHandle(child.hProcess);
            Require(waited == WAIT_OBJECT_0 && exitCode == ERROR_INVALID_DATA,
                "malformed helper requests exit with error instead of starting the app");
        }
        std::cout << "Auto-start scheduler integration checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
