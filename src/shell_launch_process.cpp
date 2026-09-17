#include "shell_launch_process.h"

#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>

namespace snowdesktop::shell_launch_process
{
namespace
{
constexpr wchar_t kCommand[] = L"--shell-open-helper";
constexpr std::uint32_t kMagic = 0x534C504E;
constexpr std::size_t kMaxPathChars = 32767;
constexpr std::size_t kMaxPidlBytes = 65536;
constexpr std::size_t kHeaderBytes = 40;
constexpr std::size_t kMaxPayload =
    kHeaderBytes + kMaxPathChars * sizeof(wchar_t) + kMaxPidlBytes;
constexpr unsigned kMaxHelpers = 16;
std::atomic<unsigned> activeHelpers{0};

struct Handle
{
    HANDLE value = nullptr;
    explicit Handle(HANDLE handle = nullptr) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    ~Handle()
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};

struct View
{
    void* value;
    ~View() { if (value) UnmapViewOfFile(value); }
};

struct HelperSlot
{
    ~HelperSlot() { activeHelpers.fetch_sub(1); }
};

std::wstring ExecutablePath(HANDLE process = nullptr)
{
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (process)
    {
        if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    }
    else
    {
        size = GetModuleFileNameW(nullptr, path.data(), size);
        if (!size || size >= path.size()) return {};
    }
    path.resize(size);
    return path;
}

template<typename T>
void Put(std::vector<unsigned char>& bytes, std::size_t offset, T value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template<typename T>
T Get(std::span<const unsigned char> bytes, std::size_t offset)
{
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

bool ValidPidl(std::span<const unsigned char> bytes)
{
    if (bytes.empty()) return true;
    if (bytes.size() > kMaxPidlBytes) return false;
    std::size_t offset = 0;
    while (bytes.size() - offset >= sizeof(USHORT))
    {
        const USHORT size = Get<USHORT>(bytes, offset);
        if (size == 0) return offset + sizeof(USHORT) == bytes.size();
        if (size < sizeof(USHORT) || size > bytes.size() - offset) return false;
        offset += size;
    }
    return false;
}

HANDLE ParseHandle(std::wstring_view text)
{
    if (text.empty()) return nullptr;
    std::uintptr_t value = 0;
    for (wchar_t digit : text)
    {
        if (digit < L'0' || digit > L'9' ||
            value > (UINTPTR_MAX - (digit - L'0')) / 10) return nullptr;
        value = value * 10 + (digit - L'0');
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(value);
    DWORD flags = 0;
    return handle && handle != INVALID_HANDLE_VALUE &&
        GetHandleInformation(handle, &flags) && (flags & HANDLE_FLAG_INHERIT)
        ? handle : nullptr;
}

void Monitor(Handle process, Handle job, DWORD timeoutMs,
    std::shared_ptr<HelperSlot> slot)
{
    const DWORD wait = WaitForSingleObject(process.value, timeoutMs);
    DWORD result = ERROR_PROCESS_ABORTED;
    if (wait == WAIT_TIMEOUT)
    {
        result = ERROR_TIMEOUT;
        // Never terminate the launched application or a whole process tree.
        TerminateProcess(process.value, result);
    }
    else if (wait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(process.value, &result);
    }
    if (result != ERROR_SUCCESS)
    {
        wchar_t message[160]{};
        swprintf_s(message,
            L"SnowDesktop: Shell helper %lu ended with error %lu.\n",
            GetProcessId(process.value), result);
        OutputDebugStringW(message);
    }
    // Closing the private kill-on-close job also covers an unexpected wait
    // failure. SILENT_BREAKAWAY_OK keeps successfully opened apps independent.
    (void)job;
    (void)slot;
}
} // namespace

std::vector<unsigned char> Encode(const Request& request)
{
    if ((request.path.empty() && request.absolutePidl.empty()) || request.path.size() > kMaxPathChars ||
        request.path.find(L'\0') != std::wstring::npos ||
        request.action > Action::RunAs || request.showCommand < SW_HIDE ||
        request.showCommand > SW_MAX || !ValidPidl(request.absolutePidl)) return {};
    const std::size_t pathBytes = request.path.size() * sizeof(wchar_t);
    std::vector<unsigned char> bytes(kHeaderBytes + pathBytes + request.absolutePidl.size());
    Put(bytes, 0, kMagic);
    Put(bytes, 4, std::uint32_t{1});
    Put(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    Put(bytes, 12, static_cast<std::uint32_t>(request.path.size()));
    Put(bytes, 16, static_cast<std::uint32_t>(request.absolutePidl.size()));
    Put(bytes, 20, static_cast<std::uint32_t>(request.action));
    Put(bytes, 24, static_cast<std::int32_t>(request.showCommand));
    Put(bytes, 28, GetCurrentProcessId());
    Put(bytes, 32, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(request.owner)));
    std::memcpy(bytes.data() + kHeaderBytes, request.path.data(), pathBytes);
    if (!request.absolutePidl.empty())
        std::memcpy(bytes.data() + kHeaderBytes + pathBytes,
            request.absolutePidl.data(), request.absolutePidl.size());
    return bytes;
}

std::optional<Request> Decode(std::span<const unsigned char> bytes)
{
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxPayload ||
        Get<std::uint32_t>(bytes, 0) != kMagic ||
        Get<std::uint32_t>(bytes, 4) != 1 ||
        Get<std::uint32_t>(bytes, 8) != bytes.size()) return std::nullopt;
    const auto pathChars = Get<std::uint32_t>(bytes, 12);
    const auto pidlBytes = Get<std::uint32_t>(bytes, 16);
    const auto action = Get<std::uint32_t>(bytes, 20);
    const auto show = Get<std::int32_t>(bytes, 24);
    const auto owner = Get<std::uint64_t>(bytes, 32);
    if ((!pathChars && !pidlBytes) || pathChars > kMaxPathChars || pidlBytes > kMaxPidlBytes ||
        action > static_cast<std::uint32_t>(Action::RunAs) ||
        show < SW_HIDE || show > SW_MAX || owner > UINTPTR_MAX ||
        kHeaderBytes + pathChars * sizeof(wchar_t) + pidlBytes != bytes.size())
        return std::nullopt;
    const auto pidl = bytes.subspan(kHeaderBytes + pathChars * sizeof(wchar_t));
    if (!ValidPidl(pidl)) return std::nullopt;
    Request request;
    request.path.resize(pathChars);
    std::memcpy(request.path.data(), bytes.data() + kHeaderBytes, pathChars * sizeof(wchar_t));
    if (request.path.find(L'\0') != std::wstring::npos) return std::nullopt;
    request.absolutePidl.assign(pidl.begin(), pidl.end());
    request.owner = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(owner));
    request.action = static_cast<Action>(action);
    request.showCommand = show;
    return request;
}

StartedProcess Start(const Request& request, DWORD timeoutMs)
{
    if (!timeoutMs || timeoutMs == INFINITE) return {};
    try
    {
        const auto bytes = Encode(request);
        if (bytes.empty()) return {};
        if (activeHelpers.fetch_add(1) >= kMaxHelpers)
        {
            activeHelpers.fetch_sub(1);
            return {};
        }
        // Keep the slot reserved through helper startup and monitor cleanup.
        std::shared_ptr<HelperSlot> slot;
        try { slot = std::make_shared<HelperSlot>(); }
        catch (...) { activeHelpers.fetch_sub(1); return {}; }

        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, static_cast<DWORD>(bytes.size()), nullptr));
        if (!mapping.value) return {};
        {
            View view{MapViewOfFile(mapping.value, FILE_MAP_WRITE, 0, 0, bytes.size())};
            if (!view.value) return {};
            std::memcpy(view.value, bytes.data(), bytes.size());
        }
        Handle childMapping, parent;
        if (!DuplicateHandle(GetCurrentProcess(), mapping.value, GetCurrentProcess(),
                &childMapping.value, FILE_MAP_READ, TRUE, 0) ||
            !DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                &parent.value, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) return {};

        Handle job(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
            JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!job.value || !SetInformationJobObject(job.value,
                JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return {};

        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> storage(attributeBytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes)) return {};
        struct AttributeGuard
        {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
        } guard{attributes};
        HANDLE inherited[] = {childMapping.value, parent.value};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, sizeof(inherited), nullptr, nullptr)) return {};
        const auto executable = ExecutablePath();
        if (executable.empty()) return {};
        std::wstring command = L"\"" + executable + L"\" " + kCommand + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(childMapping.value)) + L" " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(parent.value));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION information{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                nullptr, nullptr, &startup.StartupInfo, &information)) return {};
        Handle process(information.hProcess), thread(information.hThread);
        if (!AssignProcessToJobObject(job.value, process.value))
        {
            TerminateProcess(process.value, ERROR_PROCESS_ABORTED);
            return {};
        }
        AllowSetForegroundWindow(information.dwProcessId);
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return {};
        std::thread(Monitor, std::move(process), std::move(job), timeoutMs,
            std::move(slot)).detach();
        return {information.dwProcessId};
    }
    catch (...)
    {
        return {};
    }
}

std::optional<int> TryRunCommand(Executor executor)
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return std::nullopt;
    struct ArgumentsGuard
    {
        wchar_t** values;
        ~ArgumentsGuard() { LocalFree(values); }
    } guard{arguments};
    bool helper = false;
    for (int i = 1; i < count; ++i)
        helper = helper || std::wstring_view(arguments[i]) == kCommand;
    if (!helper) return std::nullopt;
    if (count != 4 || std::wstring_view(arguments[1]) != kCommand || !executor)
        return ERROR_INVALID_PARAMETER;
    try
    {
        const HANDLE mappingRaw = ParseHandle(arguments[2]);
        const HANDLE parentRaw = ParseHandle(arguments[3]);
        if (!mappingRaw || !parentRaw || mappingRaw == parentRaw)
            return ERROR_INVALID_HANDLE;
        Handle mapping(mappingRaw), parent(parentRaw);
        // Shell handlers may create children with handle inheritance enabled.
        // The request channel belongs only to this helper, never to opened apps.
        if (!SetHandleInformation(mapping.value, HANDLE_FLAG_INHERIT, 0) ||
            !SetHandleInformation(parent.value, HANDLE_FLAG_INHERIT, 0))
            return ERROR_INVALID_HANDLE;
        const DWORD parentId = GetProcessId(parent.value);
        const auto parentPath = ExecutablePath(parent.value);
        const auto executable = ExecutablePath();
        if (!parentId || parentId == GetCurrentProcessId() || parentPath.empty() ||
            executable.empty() || _wcsicmp(parentPath.c_str(), executable.c_str()) != 0 ||
            WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT) return ERROR_ACCESS_DENIED;
        View view{MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0)};
        MEMORY_BASIC_INFORMATION region{};
        if (!view.value || !VirtualQuery(view.value, &region, sizeof(region)) ||
            region.RegionSize < kHeaderBytes) return ERROR_INVALID_DATA;
        const auto* data = static_cast<const unsigned char*>(view.value);
        const auto header = std::span(data, kHeaderBytes);
        const auto size = Get<std::uint32_t>(header, 8);
        if (size < kHeaderBytes || size > kMaxPayload || size > region.RegionSize ||
            Get<DWORD>(header, 28) != parentId) return ERROR_INVALID_DATA;
        auto request = Decode(std::span(data, size));
        if (!request) return ERROR_INVALID_DATA;
        DWORD ownerProcess = 0;
        if (request->owner) GetWindowThreadProcessId(request->owner, &ownerProcess);
        if (ownerProcess != parentId) request->owner = nullptr;
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        const HRESULT com = CoInitializeEx(nullptr,
            COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com)) return static_cast<int>(com);
        // The parent grants this helper foreground eligibility before resuming
        // it; pass that eligibility to the Shell execution/elevation delegate.
        AllowSetForegroundWindow(ASFW_ANY);
        const bool opened = executor(*request);
        CoUninitialize();
        return opened ? ERROR_SUCCESS : ERROR_OPEN_FAILED;
    }
    catch (...)
    {
        return ERROR_UNHANDLED_EXCEPTION;
    }
}
} // namespace snowdesktop::shell_launch_process
