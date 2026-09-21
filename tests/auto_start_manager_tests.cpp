#include "auto_start_manager.h"
#include "deployment_context.h"
#include "diagnostic_log.h"

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <iostream>
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
            {
            DenyRegistryWrites deniedRun(fixture.registryRoot + L"\\Run");
            Require(!login.Configure(steam, true, &error) &&
                error.find(L"RegisterTaskDefinition") != std::wstring::npos &&
                error.find(L"0x80070005") != std::wstring::npos &&
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
        std::cout << "Auto-start scheduler integration checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
