#include "auto_start_manager.h"
#include "deployment_context.h"
#include "diagnostic_log.h"

#include <windows.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <iostream>
#include <stdexcept>
#include <thread>

// Only host identity and the log sink are substituted. Task creation, reads,
// replacement, verification and failures all go through the production COM path.
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
}

void WriteDiagnosticLogEntry(const wchar_t* message, DiagnosticLogLevel)
{
    std::wcerr << message << L'\n';
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
    ComPtr<ITaskService> service;
    Fixture()
    {
        GUID guid{};
        Require(SUCCEEDED(CoCreateGuid(&guid)), "create isolated task identity");
        wchar_t text[40]{};
        StringFromGUID2(guid, text, 40);
        path = L"\\SnowDesktopAutoStartTest-" + std::wstring(text);
        Require(SUCCEEDED(CoCreateInstance(CLSID_TaskScheduler, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service))), "activate scheduler");
        VARIANT empty{};
        Require(SUCCEEDED(service->Connect(empty, empty, empty, empty)), "connect scheduler");
    }
    void Cleanup()
    {
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
        std::cout << "Auto-start scheduler integration checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
