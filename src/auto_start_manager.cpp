#include "auto_start_manager.h"
#include "deployment_context.h"

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cwctype>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::UnifiedAutoStartOwner;
using snowdesktop::UnifiedAutoStartTaskState;
using snowdesktop::auto_start::State;
using snowdesktop::auto_start::Target;

constexpr wchar_t kTaskFolderPath[] = L"\\SnowDesktop";
constexpr wchar_t kTaskFolderName[] = L"SnowDesktop";
constexpr wchar_t kTaskName[] = L"Startup";
constexpr wchar_t kTaskUri[] = L"\\SnowDesktop\\Startup";
constexpr wchar_t kTaskAuthor[] = L"SnowDesktop";
constexpr wchar_t kTaskDescription[] =
    L"Starts the selected SnowDesktop deployment when this user signs in.";
constexpr wchar_t kMigrationEnableDescription[] =
    L"SnowDesktop auto-start migration pending; desired state: enabled.";
constexpr wchar_t kMigrationDisableDescription[] =
    L"SnowDesktop auto-start migration pending; desired state: disabled.";
constexpr wchar_t kTriggerId[] = L"SnowDesktopLogon";
constexpr wchar_t kPortableArgument[] =
    L"--snowdesktop-autostart-owner=portable";
constexpr wchar_t kPackagedArgument[] =
    L"--snowdesktop-autostart-owner=packaged";
constexpr wchar_t kPackagedExecutionAlias[] = L"SnowDesktopStore.exe";

class ScopedBstr final
{
public:
    explicit ScopedBstr(std::wstring_view value) noexcept
        : value_(SysAllocStringLen(
              value.data(), static_cast<UINT>(value.size())))
    {
    }

    ~ScopedBstr() noexcept { SysFreeString(value_); }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    [[nodiscard]] BSTR get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != nullptr; }

private:
    BSTR value_ = nullptr;
};

std::wstring TakeBstr(BSTR value) noexcept
{
    if (!value)
        return {};
    std::wstring result(value, SysStringLen(value));
    SysFreeString(value);
    return result;
}

bool MissingTaskObject(HRESULT result) noexcept
{
    return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

HRESULT ConnectTaskService(ComPtr<ITaskService>& service) noexcept
{
    HRESULT result = CoCreateInstance(CLSID_TaskScheduler, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(result))
        return result;
    VARIANT empty{};
    VariantInit(&empty);
    return service->Connect(empty, empty, empty, empty);
}

std::wstring CurrentUserSid() noexcept
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return {};
    struct TokenCloser final
    {
        HANDLE value = nullptr;
        ~TokenCloser() noexcept
        {
            if (value) CloseHandle(value);
        }
    } token{rawToken};

    DWORD required = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &required);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
        return {};
    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(rawToken, TokenUser, buffer.data(), required,
            &required))
    {
        return {};
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid))
        return {};
    std::wstring result(rawSid);
    LocalFree(rawSid);
    return result;
}

std::wstring CurrentExecutablePath() noexcept
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    return path;
}

std::wstring ExecutablePathFromCommand(std::wstring_view command) noexcept
{
    while (!command.empty() && iswspace(command.front()))
        command.remove_prefix(1);
    if (command.empty())
        return {};
    if (command.front() == L'"')
    {
        command.remove_prefix(1);
        const std::size_t end = command.find(L'"');
        if (end == std::wstring_view::npos)
            return {};
        return std::wstring(command.substr(0, end));
    }
    const std::size_t end = command.find_first_of(L" \t\r\n");
    return std::wstring(command.substr(0, end));
}

bool SameExecutablePath(
    const std::wstring& left, const std::wstring& right) noexcept
{
    if (left.empty() || right.empty())
        return false;
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error))
        return true;
    error.clear();
    const std::filesystem::path absoluteLeft =
        std::filesystem::absolute(left, error).lexically_normal();
    if (error)
        return false;
    const std::filesystem::path absoluteRight =
        std::filesystem::absolute(right, error).lexically_normal();
    if (error)
        return false;
    const std::wstring leftText = absoluteLeft.wstring();
    const std::wstring rightText = absoluteRight.wstring();
    return CompareStringOrdinal(leftText.c_str(),
        static_cast<int>(leftText.size()), rightText.c_str(),
        static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
}

UnifiedAutoStartOwner OwnerFromArguments(std::wstring_view arguments) noexcept
{
    if (arguments == kPortableArgument)
        return UnifiedAutoStartOwner::Portable;
    if (arguments == kPackagedArgument)
        return UnifiedAutoStartOwner::Packaged;
    return UnifiedAutoStartOwner::Unknown;
}

HRESULT OpenTaskFolder(
    ITaskService* service, ComPtr<ITaskFolder>& folder) noexcept
{
    const ScopedBstr path(kTaskFolderPath);
    if (!path.valid())
        return E_OUTOFMEMORY;
    return service->GetFolder(path.get(), &folder);
}

HRESULT EnsureTaskFolder(
    ITaskService* service, ComPtr<ITaskFolder>& folder) noexcept
{
    HRESULT result = OpenTaskFolder(service, folder);
    if (SUCCEEDED(result))
        return result;
    if (!MissingTaskObject(result))
        return result;

    const ScopedBstr rootPath(L"\\");
    const ScopedBstr folderName(kTaskFolderName);
    if (!rootPath.valid() || !folderName.valid())
        return E_OUTOFMEMORY;
    ComPtr<ITaskFolder> root;
    result = service->GetFolder(rootPath.get(), &root);
    if (FAILED(result))
        return result;
    VARIANT empty{};
    VariantInit(&empty);
    result = root->CreateFolder(folderName.get(), empty, &folder);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = OpenTaskFolder(service, folder);
    return result;
}

bool OwnedRegistration(ITaskDefinition* definition) noexcept
{
    ComPtr<IRegistrationInfo> registration;
    if (FAILED(definition->get_RegistrationInfo(&registration)))
        return false;
    BSTR rawUri = nullptr;
    if (FAILED(registration->get_URI(&rawUri)))
        return false;
    const std::wstring uri = TakeBstr(rawUri);
    return uri == kTaskUri;
}

State QueryRegisteredTask(IRegisteredTask* task) noexcept
{
    State state;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (FAILED(task->get_Enabled(&enabled)))
        return state;

    ComPtr<ITaskDefinition> definition;
    if (FAILED(task->get_Definition(&definition)))
        return state;
    if (!OwnedRegistration(definition.Get()))
    {
        state.status = UnifiedAutoStartTaskState::Foreign;
        return state;
    }

    ComPtr<IRegistrationInfo> registration;
    BSTR rawDescription = nullptr;
    if (FAILED(definition->get_RegistrationInfo(&registration)) ||
        FAILED(registration->get_Description(&rawDescription)))
    {
        return state;
    }
    const std::wstring description = TakeBstr(rawDescription);
    state.migrationPending = description == kMigrationEnableDescription ||
        description == kMigrationDisableDescription;
    state.enableAfterMigration =
        description == kMigrationEnableDescription;

    ComPtr<IActionCollection> actions;
    LONG count = 0;
    if (FAILED(definition->get_Actions(&actions)) ||
        FAILED(actions->get_Count(&count)) || count != 1)
    {
        return state;
    }
    ComPtr<IAction> action;
    if (FAILED(actions->get_Item(1, &action)))
        return state;
    TASK_ACTION_TYPE actionType = TASK_ACTION_EXEC;
    if (FAILED(action->get_Type(&actionType)) ||
        actionType != TASK_ACTION_EXEC)
    {
        return state;
    }
    ComPtr<IExecAction> execute;
    if (FAILED(action.As(&execute)))
        return state;

    BSTR rawPath = nullptr;
    BSTR rawArguments = nullptr;
    BSTR rawWorkingDirectory = nullptr;
    if (FAILED(execute->get_Path(&rawPath)) ||
        FAILED(execute->get_Arguments(&rawArguments)) ||
        FAILED(execute->get_WorkingDirectory(&rawWorkingDirectory)))
    {
        SysFreeString(rawPath);
        SysFreeString(rawArguments);
        SysFreeString(rawWorkingDirectory);
        return state;
    }
    state.target.executable = TakeBstr(rawPath);
    state.target.arguments = TakeBstr(rawArguments);
    state.target.workingDirectory = TakeBstr(rawWorkingDirectory);
    state.target.owner = OwnerFromArguments(state.target.arguments);
    if (state.target.owner == UnifiedAutoStartOwner::Unknown ||
        state.target.executable.empty())
    {
        return state;
    }
    state.status = enabled == VARIANT_FALSE
        ? UnifiedAutoStartTaskState::Disabled
        : UnifiedAutoStartTaskState::Enabled;
    return state;
}

HRESULT OpenRegisteredTask(
    ITaskService* service, ComPtr<IRegisteredTask>& task) noexcept
{
    ComPtr<ITaskFolder> folder;
    HRESULT result = OpenTaskFolder(service, folder);
    if (FAILED(result))
        return result;
    const ScopedBstr name(kTaskName);
    if (!name.valid())
        return E_OUTOFMEMORY;
    return folder->GetTask(name.get(), &task);
}

bool PutText(HRESULT (STDMETHODCALLTYPE IRegistrationInfo::*setter)(BSTR),
    IRegistrationInfo* target, std::wstring_view value) noexcept
{
    const ScopedBstr text(value);
    return text.valid() && SUCCEEDED((target->*setter)(text.get()));
}

bool ConfigureDefinition(
    ITaskDefinition* definition, const Target& target, bool enabled,
    std::wstring_view description) noexcept
{
    const std::wstring userSid = CurrentUserSid();
    if (userSid.empty())
        return false;

    ComPtr<IRegistrationInfo> registration;
    if (FAILED(definition->get_RegistrationInfo(&registration)) ||
        !PutText(&IRegistrationInfo::put_Author,
            registration.Get(), kTaskAuthor) ||
        !PutText(&IRegistrationInfo::put_Source,
            registration.Get(), kTaskAuthor) ||
        !PutText(&IRegistrationInfo::put_Description,
            registration.Get(), description) ||
        !PutText(&IRegistrationInfo::put_URI,
            registration.Get(), kTaskUri))
    {
        return false;
    }

    ComPtr<IPrincipal> principal;
    const ScopedBstr principalId(L"SnowDesktopCurrentUser");
    const ScopedBstr sid(userSid);
    if (!principalId.valid() || !sid.valid() ||
        FAILED(definition->get_Principal(&principal)) ||
        FAILED(principal->put_Id(principalId.get())) ||
        FAILED(principal->put_UserId(sid.get())) ||
        FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
        FAILED(principal->put_RunLevel(TASK_RUNLEVEL_LUA)))
    {
        return false;
    }

    ComPtr<ITaskSettings> settings;
    const ScopedBstr noLimit(L"PT0S");
    if (!noLimit.valid() || FAILED(definition->get_Settings(&settings)) ||
        FAILED(settings->put_Enabled(
            enabled ? VARIANT_TRUE : VARIANT_FALSE)) ||
        FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE)) ||
        FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
        FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
        FAILED(settings->put_ExecutionTimeLimit(noLimit.get())))
    {
        return false;
    }

    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logonTrigger;
    const ScopedBstr triggerId(kTriggerId);
    if (!triggerId.valid() ||
        FAILED(definition->get_Triggers(&triggers)) ||
        FAILED(triggers->Create(TASK_TRIGGER_LOGON, &trigger)) ||
        FAILED(trigger.As(&logonTrigger)) ||
        FAILED(logonTrigger->put_Id(triggerId.get())) ||
        FAILED(logonTrigger->put_UserId(sid.get())))
    {
        return false;
    }

    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> execute;
    const ScopedBstr path(target.executable);
    const ScopedBstr arguments(target.arguments);
    const ScopedBstr workingDirectory(target.workingDirectory);
    if (!path.valid() || !arguments.valid() || !workingDirectory.valid() ||
        FAILED(definition->get_Actions(&actions)) ||
        FAILED(actions->Create(TASK_ACTION_EXEC, &action)) ||
        FAILED(action.As(&execute)) ||
        FAILED(execute->put_Path(path.get())) ||
        FAILED(execute->put_Arguments(arguments.get())) ||
        FAILED(execute->put_WorkingDirectory(workingDirectory.get())))
    {
        return false;
    }
    return true;
}

bool SameTarget(const Target& left, const Target& right) noexcept
{
    return left.owner == right.owner &&
        left.arguments == right.arguments &&
        SameExecutablePath(left.executable, right.executable);
}

bool ConfigureTask(const Target& target, bool enabled,
    std::wstring_view description) noexcept
{
    if ((target.owner != UnifiedAutoStartOwner::Portable &&
            target.owner != UnifiedAutoStartOwner::Packaged) ||
        target.executable.empty() || target.arguments.empty())
    {
        return false;
    }

    const State before = snowdesktop::auto_start::Query();
    if (before.status == UnifiedAutoStartTaskState::Foreign ||
        before.status == UnifiedAutoStartTaskState::Unavailable)
    {
        return false;
    }

    ComPtr<ITaskService> service;
    if (FAILED(ConnectTaskService(service)))
        return false;
    ComPtr<ITaskFolder> folder;
    if (FAILED(EnsureTaskFolder(service.Get(), folder)))
        return false;
    ComPtr<ITaskDefinition> definition;
    if (FAILED(service->NewTask(0, &definition)) ||
        !ConfigureDefinition(
            definition.Get(), target, enabled, description))
    {
        return false;
    }

    const ScopedBstr name(kTaskName);
    if (!name.valid())
        return false;
    VARIANT empty{};
    VariantInit(&empty);
    ComPtr<IRegisteredTask> registered;
    if (FAILED(folder->RegisterTaskDefinition(name.get(), definition.Get(),
            TASK_CREATE_OR_UPDATE, empty, empty,
            TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered)))
    {
        return false;
    }

    const State after = QueryRegisteredTask(registered.Get());
    const UnifiedAutoStartTaskState expected = enabled
        ? UnifiedAutoStartTaskState::Enabled
        : UnifiedAutoStartTaskState::Disabled;
    return after.status == expected && SameTarget(after.target, target) &&
        after.migrationPending ==
            (description == kMigrationEnableDescription ||
                description == kMigrationDisableDescription) &&
        (!after.migrationPending || after.enableAfterMigration ==
            (description == kMigrationEnableDescription));
}
} // namespace

namespace snowdesktop::auto_start
{
Target CurrentDeploymentTarget() noexcept
{
    if (deployment::IsPackaged())
        return PackagedDeploymentTarget();

    Target target;
    target.owner = UnifiedAutoStartOwner::Portable;
    target.executable = CurrentExecutablePath();
    target.arguments = kPortableArgument;
    if (!target.executable.empty())
        target.workingDirectory =
            std::filesystem::path(target.executable).parent_path().wstring();
    return target;
}

Target PackagedDeploymentTarget() noexcept
{
    Target target;
    target.owner = UnifiedAutoStartOwner::Packaged;
    target.arguments = kPackagedArgument;
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)))
    {
        return target;
    }
    target.executable = (std::filesystem::path(localAppData) /
        L"Microsoft" / L"WindowsApps" / kPackagedExecutionAlias).wstring();
    CoTaskMemFree(localAppData);
    return target;
}

Target PortableTargetFromLegacyCommand(std::wstring_view command) noexcept
{
    Target target;
    target.owner = UnifiedAutoStartOwner::Portable;
    target.executable = ExecutablePathFromCommand(command);
    target.arguments = kPortableArgument;
    if (!target.executable.empty())
        target.workingDirectory =
            std::filesystem::path(target.executable).parent_path().wstring();
    return target;
}

State Query() noexcept
{
    State state;
    ComPtr<ITaskService> service;
    const HRESULT connected = ConnectTaskService(service);
    if (FAILED(connected))
        return state;

    ComPtr<IRegisteredTask> task;
    const HRESULT opened = OpenRegisteredTask(service.Get(), task);
    if (MissingTaskObject(opened))
    {
        state.status = UnifiedAutoStartTaskState::Missing;
        return state;
    }
    if (FAILED(opened))
        return state;
    return QueryRegisteredTask(task.Get());
}

bool Configure(const Target& target, bool enabled) noexcept
{
    return ConfigureTask(target, enabled, kTaskDescription);
}

bool ConfigureMigration(
    const Target& target, bool enableAfterMigration) noexcept
{
    return ConfigureTask(target, false, enableAfterMigration
        ? kMigrationEnableDescription
        : kMigrationDisableDescription);
}

bool SetEnabled(bool enabled) noexcept
{
    const State before = Query();
    if (before.status == UnifiedAutoStartTaskState::Foreign ||
        before.status == UnifiedAutoStartTaskState::Unavailable ||
        before.status == UnifiedAutoStartTaskState::Missing)
    {
        return false;
    }

    ComPtr<ITaskService> service;
    if (FAILED(ConnectTaskService(service)))
        return false;
    ComPtr<IRegisteredTask> task;
    if (FAILED(OpenRegisteredTask(service.Get(), task)) ||
        FAILED(task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE)))
    {
        return false;
    }
    const State after = QueryRegisteredTask(task.Get());
    return after.status == (enabled
        ? UnifiedAutoStartTaskState::Enabled
        : UnifiedAutoStartTaskState::Disabled);
}

bool Delete() noexcept
{
    const State before = Query();
    if (before.status == UnifiedAutoStartTaskState::Missing)
        return true;
    if (before.status == UnifiedAutoStartTaskState::Foreign ||
        before.status == UnifiedAutoStartTaskState::Unavailable)
    {
        return false;
    }

    ComPtr<ITaskService> service;
    if (FAILED(ConnectTaskService(service)))
        return false;
    ComPtr<ITaskFolder> folder;
    if (FAILED(OpenTaskFolder(service.Get(), folder)))
        return false;
    const ScopedBstr name(kTaskName);
    if (!name.valid() || FAILED(folder->DeleteTask(name.get(), 0)))
        return false;
    return Query().status == UnifiedAutoStartTaskState::Missing;
}

bool IsCurrentDeploymentTarget(const Target& target) noexcept
{
    return SameTarget(target, CurrentDeploymentTarget());
}
} // namespace snowdesktop::auto_start
