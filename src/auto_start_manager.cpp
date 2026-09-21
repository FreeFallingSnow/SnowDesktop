#include "auto_start_manager.h"
#include "deployment_context.h"
#include "diagnostic_log.h"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>

#include <cwctype>
#include <cstring>
#include <filesystem>
#include <new>
#include <string_view>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using snowdesktop::UnifiedAutoStartOwner;
using snowdesktop::UnifiedAutoStartTaskState;
using snowdesktop::auto_start::State;
using snowdesktop::auto_start::Target;

constexpr wchar_t kTaskName[] = L"Startup";
constexpr wchar_t kTaskAuthor[] = L"SnowDesktop";
constexpr wchar_t kTaskDescription[] =
    L"Starts the selected SnowDesktop deployment when this user signs in.";
constexpr wchar_t kMigrationEnableDescription[] =
    L"SnowDesktop auto-start migration pending; desired state: enabled.";
constexpr wchar_t kMigrationDisableDescription[] =
    L"SnowDesktop auto-start migration pending; desired state: disabled.";
constexpr wchar_t kTriggerId[] = L"SnowDesktopLogon";
constexpr wchar_t kPortableArgument[] = L"--snowdesktop-autostart-owner=portable";
constexpr wchar_t kPackagedArgument[] = L"--snowdesktop-autostart-owner=packaged";
constexpr wchar_t kSteamArgument[] = L"--snowdesktop-autostart-owner=steam";
constexpr wchar_t kPackagedExecutionAlias[] = L"SnowDesktopStore.exe";

std::wstring FormatError(std::wstring_view operation, HRESULT result)
{
    wchar_t code[24]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
    std::wstring message(operation);
    message += L" (";
    message += code;
    message += L")";
    wchar_t* systemMessage = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        static_cast<DWORD>(result), 0,
        reinterpret_cast<wchar_t*>(&systemMessage), 0, nullptr);
    if (length && systemMessage)
    {
        message += L": ";
        message.append(systemMessage, length);
        while (!message.empty() && iswspace(message.back())) message.pop_back();
    }
    LocalFree(systemMessage);
    return message;
}

struct TaskFailure { std::wstring message; };

void Check(HRESULT result, std::wstring_view operation)
{
    if (FAILED(result)) throw TaskFailure{FormatError(operation, result)};
}

std::wstring ExceptionMessage()
{
    try { throw; }
    catch (const TaskFailure& error) { return error.message; }
    catch (const std::filesystem::filesystem_error& error)
    {
        return FormatError(L"AutoStart.FileSystem",
            HRESULT_FROM_WIN32(error.code().value()));
    }
    catch (const std::bad_alloc&)
    {
        return FormatError(L"AutoStart.Allocation", E_OUTOFMEMORY);
    }
    catch (...) { return FormatError(L"AutoStart.Exception", E_UNEXPECTED); }
}

bool ReportFailure(std::wstring message, std::wstring* error)
{
    WriteDiagnosticLogEntry(message.c_str(), DiagnosticLogLevel::Error);
    if (error) *error = std::move(message);
    return false;
}

class ScopedBstr final
{
public:
    explicit ScopedBstr(std::wstring_view value)
        : value_(SysAllocStringLen(value.data(), static_cast<UINT>(value.size())))
    {
        Check(value_ ? S_OK : E_OUTOFMEMORY, L"SysAllocStringLen");
    }
    ~ScopedBstr() { SysFreeString(value_); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
    [[nodiscard]] BSTR get() const noexcept { return value_; }
private:
    BSTR value_ = nullptr;
};

template<typename Object, typename Getter>
std::wstring ReadText(Object* object, Getter getter, const wchar_t* operation)
{
    BSTR text = nullptr;
    const HRESULT result = (object->*getter)(&text);
    struct Cleanup { BSTR value; ~Cleanup() { SysFreeString(value); } } cleanup{text};
    Check(result, operation);
    return text ? std::wstring(text, SysStringLen(text)) : std::wstring{};
}

bool MissingTaskObject(HRESULT result) noexcept
{
    return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

ComPtr<ITaskService> ConnectTaskService()
{
    ComPtr<ITaskService> service;
    Check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&service)), L"CoCreateInstance(TaskScheduler)");
    VARIANT empty{};
    Check(service->Connect(empty, empty, empty, empty), L"ITaskService::Connect");
    return service;
}

std::wstring CurrentUserSid()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        Check(HRESULT_FROM_WIN32(GetLastError()), L"OpenProcessToken");
    struct TokenCloser { HANDLE value; ~TokenCloser() { CloseHandle(value); } } token{rawToken};
    DWORD required = 0;
    GetTokenInformation(rawToken, TokenUser, nullptr, 0, &required);
    const DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || required == 0)
        Check(error ? HRESULT_FROM_WIN32(error) : E_UNEXPECTED, L"GetTokenInformation(size)");
    std::vector<std::byte> buffer(required);
    if (!GetTokenInformation(rawToken, TokenUser, buffer.data(), required, &required))
        Check(HRESULT_FROM_WIN32(GetLastError()), L"GetTokenInformation(TokenUser)");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &rawSid))
        Check(HRESULT_FROM_WIN32(GetLastError()), L"ConvertSidToStringSidW");
    std::wstring result(rawSid);
    LocalFree(rawSid);
    return result;
}

std::wstring CurrentExecutablePath()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0) Check(HRESULT_FROM_WIN32(GetLastError()), L"GetModuleFileNameW");
    if (length >= path.size()) Check(HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER), L"GetModuleFileNameW");
    path.resize(length);
    return path;
}

std::wstring ExecutablePathFromCommand(std::wstring_view command)
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
    const std::wstring& left, const std::wstring& right)
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

UnifiedAutoStartOwner OwnerFromArguments(std::wstring_view arguments)
{
    // Ownership is a command-line token, not the byte-for-byte entire command.
    const std::wstring command = L"SnowDesktop.exe " + std::wstring(arguments);
    int count = 0;
    LPWSTR* values = CommandLineToArgvW(command.c_str(), &count);
    if (!values) Check(HRESULT_FROM_WIN32(GetLastError()), L"CommandLineToArgvW");
    UnifiedAutoStartOwner owner = UnifiedAutoStartOwner::Unknown;
    for (int index = 1; index < count; ++index)
    {
        const std::wstring_view value(values[index]);
        if (value == kPortableArgument) owner = UnifiedAutoStartOwner::Portable;
        else if (value == kPackagedArgument) owner = UnifiedAutoStartOwner::Packaged;
        else if (value == kSteamArgument) owner = UnifiedAutoStartOwner::Steam;
    }
    LocalFree(values);
    return owner;
}

ComPtr<ITaskFolder> EnsureTaskFolder(ITaskService* service, const std::wstring& path)
{
    ComPtr<ITaskFolder> folder;
    const ScopedBstr folderPath(path);
    HRESULT result = service->GetFolder(folderPath.get(), &folder);
    if (SUCCEEDED(result)) return folder;
    if (!MissingTaskObject(result)) Check(result, L"ITaskService::GetFolder(" + path + L")");
    ComPtr<ITaskFolder> root;
    const ScopedBstr rootPath(L"\\");
    Check(service->GetFolder(rootPath.get(), &root), L"ITaskService::GetFolder(root)");
    VARIANT empty{};
    result = root->CreateFolder(folderPath.get(), empty, &folder);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = service->GetFolder(folderPath.get(), &folder);
    Check(result, L"ITaskFolder::CreateFolder(" + path + L")");
    return folder;
}

ComPtr<IRegisteredTask> OpenRegisteredTask(ITaskService* service, const std::wstring& path)
{
    ComPtr<ITaskFolder> folder;
    const ScopedBstr folderPath(path);
    HRESULT result = service->GetFolder(folderPath.get(), &folder);
    if (MissingTaskObject(result)) return {};
    Check(result, L"ITaskService::GetFolder(" + path + L")");
    ComPtr<IRegisteredTask> task;
    const ScopedBstr name(kTaskName);
    result = folder->GetTask(name.get(), &task);
    if (MissingTaskObject(result)) return {};
    Check(result, L"ITaskFolder::GetTask(" + path + L"\\Startup)");
    return task;
}

State QueryRegisteredTask(IRegisteredTask* task, const std::wstring& folder)
{
    State state;
    try
    {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        Check(task->get_Enabled(&enabled), L"IRegisteredTask::get_Enabled");
        state.enabledKnown = true;
        state.enabled = enabled != VARIANT_FALSE;
        ComPtr<ITaskDefinition> definition;
        Check(task->get_Definition(&definition), L"IRegisteredTask::get_Definition");
        ComPtr<IRegistrationInfo> registration;
        Check(definition->get_RegistrationInfo(&registration), L"ITaskDefinition::get_RegistrationInfo");
        const auto uri = ReadText(registration.Get(), &IRegistrationInfo::get_URI, L"IRegistrationInfo::get_URI");
        if (uri != folder + L"\\Startup")
        {
            state.status = UnifiedAutoStartTaskState::Foreign;
            throw TaskFailure{FormatError(L"AutoStart.TaskIdentity(" + uri + L")", HRESULT_FROM_WIN32(ERROR_INVALID_DATA))};
        }
        const auto description = ReadText(registration.Get(), &IRegistrationInfo::get_Description, L"IRegistrationInfo::get_Description");
        state.migrationPending = description == kMigrationEnableDescription || description == kMigrationDisableDescription;
        state.enableAfterMigration = description == kMigrationEnableDescription;
        ComPtr<IActionCollection> actions;
        Check(definition->get_Actions(&actions), L"ITaskDefinition::get_Actions");
        LONG count = 0;
        Check(actions->get_Count(&count), L"IActionCollection::get_Count");
        Check(count == 1 ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.ActionCount=" + std::to_wstring(count));
        ComPtr<IAction> action;
        Check(actions->get_Item(1, &action), L"IActionCollection::get_Item");
        ComPtr<IExecAction> execute;
        Check(action.As(&execute), L"QueryInterface(IExecAction)");
        state.target.executable = ReadText(execute.Get(), &IExecAction::get_Path, L"IExecAction::get_Path");
        state.target.arguments = ReadText(execute.Get(), &IExecAction::get_Arguments, L"IExecAction::get_Arguments");
        state.target.workingDirectory = ReadText(execute.Get(), &IExecAction::get_WorkingDirectory, L"IExecAction::get_WorkingDirectory");
        state.target.owner = OwnerFromArguments(state.target.arguments);
        Check(!state.target.executable.empty() && state.target.owner != UnifiedAutoStartOwner::Unknown
            ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.TaskTarget(" + state.target.arguments + L")");
        state.status = enabled == VARIANT_FALSE ? UnifiedAutoStartTaskState::Disabled : UnifiedAutoStartTaskState::Enabled;
    }
    catch (...) { state.error = ExceptionMessage(); }
    return state;
}

void ConfigureDefinition(ITaskDefinition* definition, const Target& target,
    bool enabled, std::wstring_view description, const std::wstring& folder)
{
    const ScopedBstr sid(CurrentUserSid());
    const ScopedBstr author(kTaskAuthor);
    const ScopedBstr text(description);
    const ScopedBstr uri(folder + L"\\Startup");
    ComPtr<IRegistrationInfo> registration;
    Check(definition->get_RegistrationInfo(&registration), L"ITaskDefinition::get_RegistrationInfo");
    Check(registration->put_Author(author.get()), L"IRegistrationInfo::put_Author");
    Check(registration->put_Source(author.get()), L"IRegistrationInfo::put_Source");
    Check(registration->put_Description(text.get()), L"IRegistrationInfo::put_Description");
    Check(registration->put_URI(uri.get()), L"IRegistrationInfo::put_URI");
    ComPtr<IPrincipal> principal;
    const ScopedBstr principalId(L"SnowDesktopCurrentUser");
    Check(definition->get_Principal(&principal), L"ITaskDefinition::get_Principal");
    Check(principal->put_Id(principalId.get()), L"IPrincipal::put_Id");
    Check(principal->put_UserId(sid.get()), L"IPrincipal::put_UserId");
    Check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN), L"IPrincipal::put_LogonType");
    Check(principal->put_RunLevel(TASK_RUNLEVEL_LUA), L"IPrincipal::put_RunLevel");
    ComPtr<ITaskSettings> settings;
    const ScopedBstr noLimit(L"PT0S");
    Check(definition->get_Settings(&settings), L"ITaskDefinition::get_Settings");
    Check(settings->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE), L"ITaskSettings::put_Enabled");
    Check(settings->put_StartWhenAvailable(VARIANT_TRUE), L"ITaskSettings::put_StartWhenAvailable");
    Check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE), L"ITaskSettings::put_DisallowStartIfOnBatteries");
    Check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE), L"ITaskSettings::put_StopIfGoingOnBatteries");
    Check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW), L"ITaskSettings::put_MultipleInstances");
    Check(settings->put_ExecutionTimeLimit(noLimit.get()), L"ITaskSettings::put_ExecutionTimeLimit");
    ComPtr<ITriggerCollection> triggers;
    ComPtr<ITrigger> trigger;
    ComPtr<ILogonTrigger> logonTrigger;
    const ScopedBstr triggerId(kTriggerId);
    Check(definition->get_Triggers(&triggers), L"ITaskDefinition::get_Triggers");
    Check(triggers->Create(TASK_TRIGGER_LOGON, &trigger), L"ITriggerCollection::Create(LOGON)");
    Check(trigger.As(&logonTrigger), L"QueryInterface(ILogonTrigger)");
    Check(logonTrigger->put_Id(triggerId.get()), L"ILogonTrigger::put_Id");
    Check(logonTrigger->put_UserId(sid.get()), L"ILogonTrigger::put_UserId");
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> execute;
    const ScopedBstr path(target.executable);
    const ScopedBstr arguments(target.arguments);
    const ScopedBstr workingDirectory(target.workingDirectory);
    Check(definition->get_Actions(&actions), L"ITaskDefinition::get_Actions");
    Check(actions->Create(TASK_ACTION_EXEC, &action), L"IActionCollection::Create(EXEC)");
    Check(action.As(&execute), L"QueryInterface(IExecAction)");
    Check(execute->put_Path(path.get()), L"IExecAction::put_Path");
    Check(execute->put_Arguments(arguments.get()), L"IExecAction::put_Arguments");
    Check(execute->put_WorkingDirectory(workingDirectory.get()), L"IExecAction::put_WorkingDirectory");
}

bool SameTarget(const Target& left, const Target& right)
{
    return left.owner == right.owner && SameExecutablePath(left.executable, right.executable);
}

void AppendError(std::wstring& errors, const std::wstring& next)
{
    if (next.empty()) return;
    if (!errors.empty()) errors += L"\n";
    errors += next;
}

bool MissingValue(DWORD result)
{
    return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

std::wstring RegistryOperation(const wchar_t* operation,
    const std::wstring& key, const std::wstring& name)
{
    return std::wstring(operation) + L"(HKCU\\" + key + L"\\" + name + L")";
}
} // namespace

namespace snowdesktop::auto_start
{
State RunStore::Query() const noexcept
{
    State state;
    try
    {
        const auto value = deployment::QueryUnvirtualizedCurrentUserValue(
            runKey_.c_str(), valueName_.c_str());
        if (MissingValue(value.win32Result))
        {
            state.status = UnifiedAutoStartTaskState::Missing;
            return state;
        }
        Check(HRESULT_FROM_WIN32(value.win32Result),
            RegistryOperation(L"RegQueryValueExW", runKey_, valueName_));
        Check(value.type == REG_SZ && value.size >= sizeof(wchar_t) &&
            value.size <= value.data.size() && value.size % sizeof(wchar_t) == 0
            ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.RunValue");
        std::wstring command(value.size / sizeof(wchar_t), L'\0');
        std::memcpy(command.data(), value.data.data(), value.size);
        Check(command.back() == L'\0' ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"AutoStart.RunTerminator");
        command.pop_back();
        state.target.executable = ExecutablePathFromCommand(command);
        const auto end = !command.empty() && command.front() == L'"'
            ? command.find(L'"', 1) : command.find_first_of(L" \t");
        if (end != std::wstring::npos) state.target.arguments = command.substr(end + 1);
        while (!state.target.arguments.empty() && iswspace(state.target.arguments.front()))
            state.target.arguments.erase(0, 1);
        state.target.owner = OwnerFromArguments(state.target.arguments);
        Check(!state.target.executable.empty() && state.target.owner != UnifiedAutoStartOwner::Unknown
            ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.RunTarget");
        state.target.workingDirectory =
            std::filesystem::path(state.target.executable).parent_path().wstring();
        const auto approval = deployment::QueryUnvirtualizedCurrentUserValue(
            approvalKey_.c_str(), valueName_.c_str());
        auto approved = PortableAutoStartApprovalState::Missing;
        if (!MissingValue(approval.win32Result))
        {
            Check(HRESULT_FROM_WIN32(approval.win32Result),
                RegistryOperation(L"RegQueryValueExW", approvalKey_, valueName_));
            Check(approval.type == REG_BINARY && approval.size == kPortableAutoStartApprovalPayloadSize
                ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.RunApproval");
            approved = DecodePortableAutoStartApprovalState(approval.data[0]);
            Check(approved != PortableAutoStartApprovalState::Error
                ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.RunApprovalState");
        }
        state.status = IsPortableAutoStartApprovalActive(approved)
            ? UnifiedAutoStartTaskState::Enabled : UnifiedAutoStartTaskState::Disabled;
        state.enabledKnown = true;
        state.enabled = state.status == UnifiedAutoStartTaskState::Enabled;
    }
    catch (...) { state.error = ExceptionMessage(); }
    return state;
}

bool RunStore::Configure(const Target& target, bool enabled, std::wstring* error) const noexcept
{
    if (error) error->clear();
    try
    {
        if (!target.error.empty()) throw TaskFailure{target.error};
        Check(!target.executable.empty() && !target.arguments.empty() &&
            target.owner != UnifiedAutoStartOwner::Unknown ? S_OK : E_INVALIDARG,
            L"AutoStart.RunTarget");
        const std::wstring command = L"\"" + target.executable + L"\" " + target.arguments;
        // Run has a documented 260-character limit; never silently truncate it.
        Check(command.size() <= 260 ? S_OK : HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE),
            L"AutoStart.RunCommandLength=" + std::to_wstring(command.size()));
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const auto approval = BuildPortableAutoStartApprovalPayload(enabled,
            (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime);
        std::wstring errors;
        const auto write = [&](const std::wstring& key, DWORD type, const void* data, DWORD size) {
            const auto result = deployment::SetUnvirtualizedCurrentUserValue(
                key.c_str(), valueName_.c_str(), type, data, size);
            if (result != ERROR_SUCCESS)
                AppendError(errors, FormatError(RegistryOperation(L"RegSetValueExW", key, valueName_),
                    HRESULT_FROM_WIN32(result)));
        };
        // Disable approval before replacing a retained command; enable approval
        // after writing the new command so Windows cannot launch an old target.
        if (!enabled) write(approvalKey_, REG_BINARY, approval.data(), static_cast<DWORD>(approval.size()));
        if (enabled || errors.empty())
            write(runKey_, REG_SZ, command.c_str(), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
        else
        {
            const auto removed = deployment::DeleteUnvirtualizedCurrentUserValue(runKey_.c_str(), valueName_.c_str());
            if (removed != ERROR_SUCCESS && !MissingValue(removed))
                AppendError(errors, FormatError(RegistryOperation(L"RegDeleteValueW", runKey_, valueName_),
                    HRESULT_FROM_WIN32(removed)));
        }
        if (enabled) write(approvalKey_, REG_BINARY, approval.data(), static_cast<DWORD>(approval.size()));
        const State actual = Query();
        AppendError(errors, actual.error);
        if (actual.status != (enabled ? UnifiedAutoStartTaskState::Enabled : UnifiedAutoStartTaskState::Disabled) ||
            !SameTarget(actual.target, target) || actual.target.arguments != target.arguments)
            AppendError(errors, FormatError(L"AutoStart.VerifyRunState", HRESULT_FROM_WIN32(ERROR_INVALID_DATA)));
        if (!errors.empty()) throw TaskFailure{errors};
        return true;
    }
    catch (...) { return ReportFailure(ExceptionMessage(), error); }
}

bool RunStore::Delete(std::wstring* error) const noexcept
{
    if (error) error->clear();
    try
    {
        // Keep a disabled approval marker if deleting Run fails: removing it
        // would otherwise re-enable a registration disabled in Task Manager.
        const auto result = deployment::DeleteUnvirtualizedCurrentUserValue(runKey_.c_str(), valueName_.c_str());
        Check(result == ERROR_SUCCESS || MissingValue(result) ? S_OK : HRESULT_FROM_WIN32(result),
            RegistryOperation(L"RegDeleteValueW", runKey_, valueName_));
        const auto approval = deployment::DeleteUnvirtualizedCurrentUserValue(approvalKey_.c_str(), valueName_.c_str());
        Check(approval == ERROR_SUCCESS || MissingValue(approval) ? S_OK : HRESULT_FROM_WIN32(approval),
            RegistryOperation(L"RegDeleteValueW", approvalKey_, valueName_));
        const auto actual = Query();
        if (!actual.error.empty()) throw TaskFailure{actual.error};
        Check(actual.status == UnifiedAutoStartTaskState::Missing ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            L"AutoStart.VerifyRunDeletion");
        return true;
    }
    catch (...) { return ReportFailure(ExceptionMessage(), error); }
}

State LoginStore::Query() const noexcept
{
    auto run = run_.Query();
    const auto task = task_.Query();
    if (run.status == UnifiedAutoStartTaskState::Missing) return task;
    if (run.status == UnifiedAutoStartTaskState::Enabled) return run;
    // An enabled or unreadable task cannot be reported as fully disabled merely
    // because the fallback is disabled. An established fallback also supersedes
    // stale migration intent in an already-disabled task.
    if (task.status != UnifiedAutoStartTaskState::Missing &&
        !(task.enabledKnown && !task.enabled)) return task;
    return run;
}

bool LoginStore::Configure(const Target& target, bool enabled, std::wstring* error) const noexcept
{
    if (error) error->clear();
    if (!target.error.empty()) return ReportFailure(target.error, error);
    if (target.executable.empty() || target.arguments.empty() ||
        target.owner == UnifiedAutoStartOwner::Unknown)
        return ReportFailure(FormatError(L"AutoStart.Target", E_INVALIDARG), error);
    std::wstring taskError;
    if (task_.Configure(target, enabled, kTaskDescription, &taskError))
        return run_.Delete(error);

    // Even a read/disable failure must not stop the attempt to write the fallback.
    // Report incomplete cleanup after trying both mechanisms.
    std::wstring disableError, runError;
    const bool disabled = task_.SetEnabled(false, &disableError);
    const bool configured = run_.Configure(target, enabled, &runError);
    const auto task = task_.Query();
    const bool inactive = disabled || task.status == UnifiedAutoStartTaskState::Missing ||
        (task.enabledKnown && !task.enabled);
    if (configured && (inactive || (enabled && !task.enabledKnown &&
        task.status == UnifiedAutoStartTaskState::Unavailable)))
        return true;
    AppendError(taskError, disableError);
    AppendError(taskError, runError);
    AppendError(taskError, task.error);
    return ReportFailure(std::move(taskError), error);
}

Target CurrentDeploymentTarget() noexcept
{
    const auto& context = deployment::GetRuntimeDeploymentContext();
    if (context.kind == deployment::RuntimeDeploymentKind::Packaged)
        return PackagedDeploymentTarget();

    if (context.kind == deployment::RuntimeDeploymentKind::SteamManaged)
    {
        Target target;
        target.owner = UnifiedAutoStartOwner::Steam;
        target.executable = context.launcher.wstring();
        target.arguments = kSteamArgument;
        target.workingDirectory = context.installRoot.wstring();
        return target;
    }

    // Local Steam development profiles are intentionally unable to replace
    // the production login task.
    if (!deployment::CanOwnProductionAutoStart(context.kind))
    {
        Target target;
        target.error = FormatError(L"AutoStart.CurrentDeploymentTarget", E_NOTIMPL);
        return target;
    }

    Target target;
    target.owner = UnifiedAutoStartOwner::Portable;
    try { target.executable = CurrentExecutablePath(); }
    catch (...) { target.error = ExceptionMessage(); }
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
    const HRESULT result = SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData);
    if (FAILED(result))
    {
        target.error = FormatError(L"SHGetKnownFolderPath(LocalAppData)", result);
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

State TaskStore::Query() const noexcept
{
    State state;
    try
    {
        const auto service = ConnectTaskService();
        const auto task = OpenRegisteredTask(service.Get(), folder_);
        if (!task) state.status = UnifiedAutoStartTaskState::Missing;
        else state = QueryRegisteredTask(task.Get(), folder_);
    }
    catch (...) { state.error = ExceptionMessage(); }
    if (!state.error.empty()) ReportFailure(state.error, nullptr);
    return state;
}

bool TaskStore::Configure(const Target& target, bool enabled,
    std::wstring_view description, std::wstring* error) const noexcept
{
    if (error) error->clear();
    try
    {
        if (!target.error.empty()) throw TaskFailure{target.error};
        Check(!target.executable.empty() && !target.arguments.empty()
            ? S_OK : E_INVALIDARG, L"AutoStart.Target");
        // An explicit write repairs stale definitions. Reading/classifying the
        // old task must not veto an operation Windows would allow.
        const auto service = ConnectTaskService();
        const auto folder = EnsureTaskFolder(service.Get(), folder_);
        ComPtr<ITaskDefinition> definition;
        Check(service->NewTask(0, &definition), L"ITaskService::NewTask");
        ConfigureDefinition(definition.Get(), target, enabled, description, folder_);
        const ScopedBstr name(kTaskName);
        VARIANT empty{};
        ComPtr<IRegisteredTask> registered;
        Check(folder->RegisterTaskDefinition(name.get(), definition.Get(),
            TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN,
            empty, &registered), L"ITaskFolder::RegisterTaskDefinition(" + folder_ + L"\\Startup)");
        const State after = QueryRegisteredTask(registered.Get(), folder_);
        if (!after.error.empty()) throw TaskFailure{after.error};
        const auto expected = enabled ? UnifiedAutoStartTaskState::Enabled : UnifiedAutoStartTaskState::Disabled;
        const bool pending = description == kMigrationEnableDescription || description == kMigrationDisableDescription;
        Check(after.status == expected && SameTarget(after.target, target) &&
            after.target.arguments == target.arguments &&
            after.migrationPending == pending && (!pending ||
                after.enableAfterMigration == (description == kMigrationEnableDescription))
            ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.VerifyRegisteredTask");
        return true;
    }
    catch (...) { return ReportFailure(ExceptionMessage(), error); }
}

bool TaskStore::SetEnabled(bool enabled, std::wstring* error) const noexcept
{
    if (error) error->clear();
    try
    {
        const auto service = ConnectTaskService();
        const auto task = OpenRegisteredTask(service.Get(), folder_);
        if (!task && !enabled) return true;
        Check(task ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"ITaskFolder::GetTask(Startup)");
        Check(task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE), L"IRegisteredTask::put_Enabled");
        VARIANT_BOOL actual = VARIANT_FALSE;
        Check(task->get_Enabled(&actual), L"IRegisteredTask::get_Enabled");
        Check((actual != VARIANT_FALSE) == enabled ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA), L"AutoStart.VerifyEnabled");
        return true;
    }
    catch (...) { return ReportFailure(ExceptionMessage(), error); }
}

bool TaskStore::Delete(std::wstring* error) const noexcept
{
    if (error) error->clear();
    try
    {
        const auto service = ConnectTaskService();
        const auto task = OpenRegisteredTask(service.Get(), folder_);
        if (!task) return true;
        ComPtr<ITaskFolder> folder;
        const ScopedBstr path(folder_);
        const ScopedBstr name(kTaskName);
        Check(service->GetFolder(path.get(), &folder), L"ITaskService::GetFolder");
        Check(folder->DeleteTask(name.get(), 0), L"ITaskFolder::DeleteTask");
        Check(!OpenRegisteredTask(service.Get(), folder_) ? S_OK : HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), L"AutoStart.VerifyDeleted");
        return true;
    }
    catch (...) { return ReportFailure(ExceptionMessage(), error); }
}

State Query() noexcept { return LoginStore{}.Query(); }
bool Apply(const Target& target, bool enabled, std::wstring* error) noexcept
{
    return LoginStore{}.Configure(target, enabled, error);
}
bool Configure(const Target& target, bool enabled, std::wstring* error) noexcept
{
    return TaskStore{}.Configure(target, enabled, kTaskDescription, error);
}
bool ConfigureMigration(const Target& target, bool enabled, std::wstring* error) noexcept
{
    return TaskStore{}.Configure(target, false,
        enabled ? kMigrationEnableDescription : kMigrationDisableDescription, error);
}
bool SetEnabled(bool enabled, std::wstring* error) noexcept
{
    return TaskStore{}.SetEnabled(enabled, error);
}
bool Delete(std::wstring* error) noexcept { return TaskStore{}.Delete(error); }
bool IsCurrentDeploymentTarget(const Target& target) noexcept
{
    try { return SameTarget(target, CurrentDeploymentTarget()); }
    catch (...) { return false; }
}
} // namespace snowdesktop::auto_start
