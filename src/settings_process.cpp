#include "settings_process.h"

#include <shellapi.h>
#include <array>
#include <charconv>
#include <filesystem>
#include <utility>

namespace snowdesktop::settings_ipc
{
namespace
{
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE Detach() { return std::exchange(value, nullptr); }
};
std::wstring ExecutablePath(HANDLE process = nullptr)
{
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (process)
    {
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &length))
            throw ProtocolError("cannot identify settings parent executable");
    }
    else
    {
        length = GetModuleFileNameW(nullptr, path.data(), length);
        if (!length || length >= path.size()) throw ProtocolError("cannot identify settings executable");
    }
    path.resize(length);
    return path;
}
std::string FileIdentity(const std::wstring& path)
{
    Handle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr)};
    BY_HANDLE_FILE_INFORMATION information{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value, &information))
        throw ProtocolError("cannot read settings executable identity");
    return "settings-ipc-1/" + std::to_string(sizeof(void*)) + "/" +
        std::to_string(information.dwVolumeSerialNumber) + "/" +
        std::to_string(information.nFileIndexHigh) + "/" + std::to_string(information.nFileIndexLow) + "/" +
        std::to_string(information.nFileSizeHigh) + "/" + std::to_string(information.nFileSizeLow) + "/" +
        std::to_string(information.ftLastWriteTime.dwHighDateTime) + "/" +
        std::to_string(information.ftLastWriteTime.dwLowDateTime);
}
struct Arguments
{
    int count = 0;
    wchar_t** values = CommandLineToArgvW(GetCommandLineW(), &count);
    ~Arguments() { if (values) LocalFree(values); }
};
HANDLE ParseHandle(const wchar_t* text)
{
    if (!text || !*text) throw ProtocolError("missing inherited settings handle");
    std::uintptr_t value = 0;
    for (const auto* digit = text; *digit; ++digit)
    {
        if (*digit < L'0' || *digit > L'9' ||
            value > (UINTPTR_MAX - (*digit - L'0')) / 10)
            throw ProtocolError("invalid inherited settings handle");
        value = value * 10 + (*digit - L'0');
    }
    const auto handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    if (!handle || handle == INVALID_HANDLE_VALUE ||
        !GetHandleInformation(handle, &flags) || !(flags & HANDLE_FLAG_INHERIT))
        throw ProtocolError("settings handle was not inherited");
    return handle;
}
}

struct SettingsProcess::Impl
{
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD id = 0;
    void Stop() noexcept
    {
        // Closing the private job also handles a child stuck during XAML
        // teardown. This is only called after a normal close acknowledgement,
        // on startup failure, or during final application shutdown.
        if (job) CloseHandle(std::exchange(job, nullptr));
        if (process) CloseHandle(std::exchange(process, nullptr));
        id = 0;
    }
    ~Impl() { Stop(); }
};
SettingsProcess::SettingsProcess() : impl_(std::make_unique<Impl>()) {}
SettingsProcess::~SettingsProcess() = default;
void SettingsProcess::Stop() noexcept { impl_->Stop(); }
bool SettingsProcess::Running() const noexcept
{
    return impl_->process && WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
}
DWORD SettingsProcess::ProcessId() const noexcept { return impl_->id; }
std::string ExecutableIdentity() { return FileIdentity(ExecutablePath()); }

void SettingsProcess::Start(Channel& channel)
{
    if (Running()) throw ProtocolError("settings process is already running");
    impl_->Stop();
    channel.Close();
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle parentRead, childWrite, childRead, parentWrite, inheritedParent;
    if (!CreatePipe(&parentRead.value, &childWrite.value, &security, 65536) ||
        !CreatePipe(&childRead.value, &parentWrite.value, &security, 65536) ||
        !SetHandleInformation(parentRead.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parentWrite.value, HANDLE_FLAG_INHERIT, 0) ||
        !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
            &inheritedParent.value, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
        throw ProtocolError("cannot create inherited settings channel");

    Handle job{CreateJobObjectW(nullptr, nullptr)};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
        &limits, sizeof(limits))) throw ProtocolError("cannot supervise settings process");

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> storage(attributeBytes);
    auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        throw ProtocolError("cannot initialize settings handle allowlist");
    struct AttributeGuard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{attributes};
    HANDLE inherited[] = {childRead.value, childWrite.value, inheritedParent.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr))
        throw ProtocolError("cannot set settings handle allowlist");
    const auto executable = ExecutablePath();
    std::wstring command = L"\"" + executable + L"\" --settings-ui " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(childRead.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(childWrite.value)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(inheritedParent.value));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
        nullptr, nullptr, &startup.StartupInfo, &process))
        throw ProtocolError("cannot launch settings process");
    Handle child{process.hProcess}, thread{process.hThread};
    if (!AssignProcessToJobObject(job.value, child.value))
    {
        TerminateProcess(child.value, ERROR_PROCESS_ABORTED);
        throw ProtocolError("cannot attach settings process to its lifetime job");
    }
    Handle peer;
    if (!DuplicateHandle(GetCurrentProcess(), child.value, GetCurrentProcess(),
        &peer.value, SYNCHRONIZE, FALSE, 0))
        throw ProtocolError("cannot retain settings process identity");
    channel.Open(parentRead.Detach(), parentWrite.Detach(), peer.Detach());
    impl_->process = child.Detach();
    impl_->job = job.Detach();
    impl_->id = process.dwProcessId;
    AllowSetForegroundWindow(process.dwProcessId);
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
    {
        channel.Close();
        impl_->Stop();
        throw ProtocolError("cannot resume settings process");
    }
}
bool IsSettingsProcessCommand()
{
    const Arguments args;
    for (int i = 1; args.values && i < args.count; ++i)
        if (std::wstring_view(args.values[i]) == L"--settings-ui") return true;
    return false;
}
void OpenInheritedSettingsChannel(Channel& channel)
{
    const Arguments args;
    if (!args.values || args.count != 5 || std::wstring_view(args.values[1]) != L"--settings-ui")
        throw ProtocolError("invalid settings process command");
    // Validate all handles before adopting any: repeated handles are rejected
    // so malformed input cannot close an unrelated object twice.
    HANDLE read = ParseHandle(args.values[2]);
    HANDLE write = ParseHandle(args.values[3]);
    HANDLE parent = ParseHandle(args.values[4]);
    if (read == write || read == parent || write == parent ||
        GetProcessId(parent) == GetCurrentProcessId() ||
        FileIdentity(ExecutablePath(parent)) != ExecutableIdentity())
        throw ProtocolError("settings parent executable does not match");
    channel.Open(read, write, parent);
}
} // namespace snowdesktop::settings_ipc
