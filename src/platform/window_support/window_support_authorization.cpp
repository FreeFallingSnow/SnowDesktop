#include "window_support_authorization.h"
#include "window_support_protocol.h"
#include "window_support_security.h"

#include <taskschd.h>
#include <sddl.h>
#include <aclapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace snowdesktop::window_support
{
namespace
{
using Microsoft::WRL::ComPtr;
struct Failure { DWORD error; };
void Check(HRESULT result) { if (FAILED(result)) throw Failure{static_cast<DWORD>(result)}; }
void WinCheck(BOOL result) { if (!result) throw Failure{GetLastError()}; }
struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComScope() { if (result != RPC_E_CHANGED_MODE) Check(result); }
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
};
struct Text
{
    BSTR value = nullptr;
    Text() = default;
    explicit Text(const wchar_t* text) : value(SysAllocString(text)) { if (!value) throw Failure{ERROR_OUTOFMEMORY}; }
    explicit Text(const std::wstring& text) : Text(text.c_str()) {}
    ~Text() { SysFreeString(value); }
    Text(const Text&) = delete;
    Text& operator=(const Text&) = delete;
};
struct Variant
{
    VARIANT value{};
    Variant() { VariantInit(&value); }
    explicit Variant(const std::wstring& text) : Variant()
    { value.vt = VT_BSTR; value.bstrVal = SysAllocString(text.c_str()); if (!value.bstrVal) throw Failure{ERROR_OUTOFMEMORY}; }
    ~Variant() { VariantClear(&value); }
};
struct Handle { HANDLE value = nullptr; ~Handle() { if (value) CloseHandle(value); } };
Handle CreateProtectedFile(const std::filesystem::path& path)
{
    const auto text = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;" + CurrentUserSid() + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    WinCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(descriptor);
    if (error) throw Failure{error};
    return Handle{file};
}
bool TaskSecurityMatches(IRegisteredTask* task)
{
    Text descriptor;
    Check(task->GetSecurityDescriptor(OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &descriptor.value));
    return TrustedAuthorizationSecurity(descriptor.value);
}
struct Scheduler
{
    ComScope scope;
    ComPtr<ITaskService> service;
    Scheduler()
    {
        Check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)));
        Variant empty;
        Check(service->Connect(empty.value, empty.value, empty.value, empty.value));
    }
    HRESULT Folder(ComPtr<ITaskFolder>& folder)
    { const Text path(protocol::TaskFolder); return service->GetFolder(path.value, &folder); }
};
std::wstring SecurityDescriptor()
{
    const auto text = AuthorizationSecurityDescriptor();
    if (text.empty()) throw Failure{ERROR_INVALID_SID};
    return text;
}
bool Missing(HRESULT result)
{ return result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || result == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND); }
ComPtr<ITaskFolder> EnsureTaskFolder(Scheduler& scheduler)
{
    ComPtr<ITaskFolder> folder;
    ComPtr<ITaskFolder> root, product;
    const Text rootName(L"\\"), productName(L"SnowDesktop"), supportName(L"WindowSupport");
    Check(scheduler.service->GetFolder(rootName.value, &root));
    const auto sharedSecurity = AuthorizationSecurityDescriptor(true);
    if (sharedSecurity.empty()) throw Failure{ERROR_INVALID_SID};
    const Variant security(sharedSecurity);
    const Text descriptor(sharedSecurity);
    const Text absoluteProduct(L"\\SnowDesktop");
    HRESULT result = scheduler.service->GetFolder(absoluteProduct.value, &product);
    if (Missing(result)) result = root->CreateFolder(productName.value, security.value, &product);
    Check(result);
    Check(product->SetSecurityDescriptor(descriptor.value, 0));
    result = scheduler.Folder(folder);
    if (Missing(result)) result = product->CreateFolder(supportName.value, security.value, &folder);
    Check(result);
    Check(folder->SetSecurityDescriptor(descriptor.value, 0));
    return folder;
}
bool TaskMatches(ITaskDefinition* definition)
{
    ComPtr<ITriggerCollection> triggers;
    Check(definition->get_Triggers(&triggers));
    LONG triggerCount = 0;
    Check(triggers->get_Count(&triggerCount));
    if (triggerCount) return false;
    ComPtr<ITaskSettings> settings;
    Check(definition->get_Settings(&settings));
    VARIANT_BOOL demand = VARIANT_FALSE;
    Check(settings->get_AllowDemandStart(&demand));
    if (demand != VARIANT_TRUE) return false;
    ComPtr<IPrincipal> principal;
    Check(definition->get_Principal(&principal));
    TASK_RUNLEVEL_TYPE level{};
    TASK_LOGON_TYPE logon{};
    Text sid;
    Check(principal->get_RunLevel(&level));
    Check(principal->get_LogonType(&logon));
    Check(principal->get_UserId(&sid.value));
    if (level != TASK_RUNLEVEL_HIGHEST || logon != TASK_LOGON_INTERACTIVE_TOKEN ||
        !sid.value || CurrentUserSid() != sid.value) return false;
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> execute;
    LONG count = 0;
    Check(definition->get_Actions(&actions));
    Check(actions->get_Count(&count));
    if (count != 1) return false;
    Check(actions->get_Item(1, &action));
    Check(action.As(&execute));
    Text executable, arguments;
    Check(execute->get_Path(&executable.value));
    Check(execute->get_Arguments(&arguments.value));
    return executable.value && _wcsicmp(executable.value, HelperImage().c_str()) == 0 &&
        (!arguments.value || !*arguments.value);
}
void RejectReparse(const std::filesystem::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw Failure{ERROR_ACCESS_DENIED};
}
void ProtectDirectory(const std::filesystem::path& path, bool shared = false)
{
    RejectReparse(path);
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw Failure{GetLastError()};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto sid = CurrentUserSid();
    const auto text = L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;" +
        (shared ? std::wstring(L"BU") : sid) + L")";
    WinCheck(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr));
    const BOOL result = SetFileSecurityW(path.c_str(), OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    const DWORD error = result ? ERROR_SUCCESS : GetLastError();
    LocalFree(descriptor);
    if (error) throw Failure{error};
}
void InstallFiles(const std::filesystem::path& source)
{
    const auto destination = HelperDirectory();
    if (destination.empty()) throw Failure{ERROR_PATH_NOT_FOUND};
    const auto support = destination.parent_path();
    const auto product = support.parent_path();
    RejectReparse(product.parent_path());
    ProtectDirectory(product, true);
    ProtectDirectory(support, true);
    ProtectDirectory(destination);
    // Fixed distribution-owned images only. The broker never accepts a DLL,
    // executable or command line from an ordinary IPC request.
    constexpr std::array images{protocol::HelperFilename, L"SnowDesktopDockMinimizeHook.dll",
        L"SnowDesktopDockMinimizeHook32.dll", L"SnowDesktopDockMinimizeHost32.exe"};
    for (const auto image : images)
    {
        const auto privateImage = source / L"SnowDesktop.Runtime" / image;
        const auto from = std::filesystem::is_regular_file(privateImage) ? privateImage : source / image;
        const auto to = destination / image;
        RejectReparse(from);
        RejectReparse(to);
        // Recreate fixed leaves under the now-protected parent. CopyFile can
        // copy a source DACL, and an old user-owned leaf can retain WRITE_DAC.
        // Neither may become a writable future elevated executable.
        if (!DeleteFileW(to.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) throw Failure{GetLastError()};
        std::ifstream input(from, std::ios::binary);
        if (!input) throw Failure{ERROR_FILE_NOT_FOUND};
        const auto output = CreateProtectedFile(to);
        std::array<char, 65536> buffer{};
        while (input)
        {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const DWORD bytes = static_cast<DWORD>(input.gcount());
            DWORD written = 0;
            if (bytes && (!WriteFile(output.value, buffer.data(), bytes, &written, nullptr) || written != bytes))
                throw Failure{ERROR_WRITE_FAULT};
        }
        if (!input.eof()) throw Failure{ERROR_READ_FAULT};
        WinCheck(FlushFileBuffers(output.value));
    }
}
DWORD Error() noexcept
{
    try { throw; }
    catch (const Failure& failure) { return failure.error; }
    catch (const std::bad_alloc&) { return ERROR_OUTOFMEMORY; }
    catch (...) { return ERROR_GEN_FAILURE; }
}
}
AuthorizationResult QueryAuthorization() noexcept
{
    try
    {
        Scheduler scheduler;
        ComPtr<ITaskFolder> folder;
        const HRESULT found = scheduler.Folder(folder);
        if (Missing(found)) return {};
        Check(found);
        ComPtr<IRegisteredTask> task;
        const Text name(HelperTaskName());
        const HRESULT result = folder->GetTask(name.value, &task);
        if (Missing(result)) return {};
        Check(result);
        VARIANT_BOOL enabled = VARIANT_FALSE;
        ComPtr<ITaskDefinition> definition;
        Check(task->get_Enabled(&enabled));
        Check(task->get_Definition(&definition));
        const auto image = HelperImage();
        const auto attributes = GetFileAttributesW(image.c_str());
        const bool valid = enabled == VARIANT_TRUE && TaskMatches(definition.Get()) && TaskSecurityMatches(task.Get()) &&
            attributes != INVALID_FILE_ATTRIBUTES && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
        return {valid ? AuthorizationState::Registered : AuthorizationState::Invalid,
            static_cast<DWORD>(valid ? ERROR_SUCCESS : ERROR_INVALID_DATA)};
    }
    catch (...) { return {AuthorizationState::Invalid, Error()}; }
}
DWORD StartAuthorizedHelper() noexcept
{
    try
    {
        Scheduler scheduler;
        ComPtr<ITaskFolder> folder;
        Check(scheduler.Folder(folder));
        ComPtr<IRegisteredTask> task;
        const Text name(HelperTaskName());
        Check(folder->GetTask(name.value, &task));
        ComPtr<ITaskDefinition> definition;
        Check(task->get_Definition(&definition));
        if (!TaskMatches(definition.Get()) || !TaskSecurityMatches(task.Get())) return ERROR_INVALID_DATA;
        Variant empty;
        ComPtr<IRunningTask> running;
        Check(task->Run(empty.value, &running));
        return ERROR_SUCCESS;
    }
    catch (...) { return Error(); }
}
DWORD ConfigureAuthorization(bool enabled, HWND owner) noexcept
{
    try
    {
        const auto ownImage = ProcessImage(GetCurrentProcess());
        if (ownImage.empty()) return ERROR_PATH_NOT_FOUND;
        const auto directory = std::filesystem::path(ownImage).parent_path();
        const auto privateImage = directory / L"SnowDesktop.Runtime" / protocol::HelperFilename;
        const auto executable = std::filesystem::is_regular_file(privateImage) ? privateImage : directory / protocol::HelperFilename;
        const std::wstring arguments = std::wstring(enabled ? L"--authorize " : L"--revoke ") + std::to_wstring(GetCurrentProcessId());
        SHELLEXECUTEINFOW info{sizeof(info)};
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.hwnd = owner;
        info.lpVerb = L"runas";
        info.lpFile = executable.c_str();
        info.lpParameters = arguments.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info)) return GetLastError();
        Handle process{info.hProcess};
        if (!process.value || WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) return ERROR_PROCESS_ABORTED;
        DWORD result = ERROR_GEN_FAILURE;
        return GetExitCodeProcess(process.value, &result) ? result : GetLastError();
    }
    catch (...) { return Error(); }
}
int RunAuthorizationCommand(bool enabled, DWORD requestingHost) noexcept
{
    try
    {
        Handle host{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, requestingHost)};
        TokenIdentity own;
        if (!host.value || !SameInteractiveUser(host.value) || !ReadTokenIdentity(GetCurrentProcess(), own) ||
            !own.elevated || own.integrity < SECURITY_MANDATORY_HIGH_RID ||
            WaitForSingleObject(host.value, 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
        const auto hostImage = std::filesystem::path(ProcessImage(host.value));
        if (_wcsicmp(hostImage.filename().c_str(), L"SnowDesktop.exe") != 0) return ERROR_ACCESS_DENIED;
        Scheduler scheduler;
        const auto folder = EnsureTaskFolder(scheduler);
        const Text name(HelperTaskName());
        if (!enabled)
        {
            const HRESULT removed = folder->DeleteTask(name.value, 0);
            Check(Missing(removed) ? S_OK : removed);
            return ERROR_SUCCESS; // Keep protected binaries; never recursively delete a computed path.
        }
        InstallFiles(hostImage.parent_path());
        {
            // Bind this narrowly scoped authorization to the actual host
            // selected by the user, rather than accepting a copied EXE name.
            const auto identityPath = HelperDirectory() / L"host-path.bin";
            RejectReparse(identityPath);
            if (!DeleteFileW(identityPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) throw Failure{GetLastError()};
            const auto identity = CreateProtectedFile(identityPath);
            const auto path = hostImage.wstring();
            const DWORD size = static_cast<DWORD>(path.size() * sizeof(wchar_t));
            DWORD written = 0;
            WinCheck(WriteFile(identity.value, path.data(), size, &written, nullptr));
            if (written != size) throw Failure{ERROR_WRITE_FAULT};
            WinCheck(FlushFileBuffers(identity.value));
        }
        ComPtr<ITaskDefinition> definition;
        Check(scheduler.service->NewTask(0, &definition));
        ComPtr<IPrincipal> principal;
        Check(definition->get_Principal(&principal));
        const Text sid(own.sid), principalName(L"SnowDesktopWindowSupport");
        Check(principal->put_Id(principalName.value));
        Check(principal->put_UserId(sid.value));
        Check(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
        Check(principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST));
        ComPtr<ITaskSettings> settings;
        Check(definition->get_Settings(&settings));
        Check(settings->put_Enabled(VARIANT_TRUE));
        Check(settings->put_AllowDemandStart(VARIANT_TRUE));
        Check(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
        Check(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
        Check(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
        const Text unlimited(L"PT0S");
        Check(settings->put_ExecutionTimeLimit(unlimited.value));
        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        ComPtr<IExecAction> execute;
        Check(definition->get_Actions(&actions));
        Check(actions->Create(TASK_ACTION_EXEC, &action));
        Check(action.As(&execute));
        const Text image(HelperImage().wstring()), directory(HelperDirectory().wstring());
        Check(execute->put_Path(image.value));
        Check(execute->put_WorkingDirectory(directory.value));
        // No login trigger: disabling auto-start never revokes this capability.
        Variant user(own.sid), empty, security(SecurityDescriptor());
        ComPtr<IRegisteredTask> task;
        Check(folder->RegisterTaskDefinition(name.value, definition.Get(), TASK_CREATE_OR_UPDATE | TASK_DONT_ADD_PRINCIPAL_ACE,
            user.value, empty.value, TASK_LOGON_INTERACTIVE_TOKEN, security.value, &task));
        return ERROR_SUCCESS;
    }
    catch (...) { return static_cast<int>(Error()); }
}
}
