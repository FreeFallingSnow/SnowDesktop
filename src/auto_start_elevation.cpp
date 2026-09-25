#include "auto_start_elevation.h"
#include "diagnostic_log.h"

#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace snowdesktop::auto_start
{
namespace
{
constexpr wchar_t kArgument[] = L"--snowdesktop-startup-elevated=";
constexpr wchar_t kMappingPrefix[] = L"Local\\SnowDesktop.StartupElevation.v1.";
constexpr DWORD kMagic = 0x53444145;
struct Request
{
    DWORD magic = kMagic;
    DWORD size = sizeof(Request);
    DWORD processId = 0;
    LONG completed = 0;
    DWORD success = 0;
    DWORD enabled = 0;
    UnifiedAutoStartOwner owner = UnifiedAutoStartOwner::Unknown;
    wchar_t executable[32768]{};
    wchar_t arguments[1024]{};
    wchar_t directory[32768]{};
    wchar_t error[4096]{};
};
struct Failure { std::wstring message; };
void Check(bool success, std::wstring_view operation, DWORD code = ERROR_SUCCESS)
{
    if (success) return;
    if (!code) code = GetLastError();
    throw Failure{DescribeError(operation, HRESULT_FROM_WIN32(code ? code : ERROR_GEN_FAILURE))};
}
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
};
struct View
{
    Request* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
};
struct LocalMemory
{
    void* value = nullptr;
    ~LocalMemory() { LocalFree(value); }
};
std::wstring ProcessPath(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    Check(QueryFullProcessImageNameW(process, 0, path.data(), &size), L"QueryFullProcessImageNameW(startup)");
    path.resize(size);
    return path;
}
std::wstring ProcessSid(HANDLE process)
{
    Handle token;
    Check(OpenProcessToken(process, TOKEN_QUERY, &token.value), L"OpenProcessToken(startup owner)");
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    Check(size != 0, L"GetTokenInformation(startup owner size)");
    std::vector<BYTE> data(size);
    Check(GetTokenInformation(token.value, TokenUser, data.data(), size, &size), L"GetTokenInformation(startup owner)");
    LPWSTR sid = nullptr;
    Check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid), L"ConvertSidToStringSidW(startup owner)");
    LocalMemory cleanup{sid};
    return sid;
}
template<std::size_t N> void Copy(wchar_t (&destination)[N], std::wstring_view source)
{
    Check(source.size() < N, L"AutoStart.ElevationPayloadSize", ERROR_INSUFFICIENT_BUFFER);
    std::copy(source.begin(), source.end(), destination);
    destination[source.size()] = L'\0';
}
template<std::size_t N> bool Terminated(const wchar_t (&value)[N])
{
    return std::find(std::begin(value), std::end(value), L'\0') != std::end(value);
}
std::wstring ExceptionError()
{
    try { throw; }
    catch (const Failure& failure) { return failure.message; }
    catch (const std::bad_alloc&) { return DescribeError(L"AutoStart.ElevationAllocation", E_OUTOFMEMORY); }
    catch (...) { return DescribeError(L"AutoStart.ElevationException", E_UNEXPECTED); }
}
}

bool ConfigureElevated(const Target& target, bool enabled, std::wstring* error) noexcept
{
    if (error) error->clear();
    try
    {
        GUID guid{};
        const HRESULT created = CoCreateGuid(&guid);
        if (FAILED(created)) throw Failure{DescribeError(L"CoCreateGuid(startup elevation)", created)};
        wchar_t text[40]{};
        Check(StringFromGUID2(guid, text, 40) != 0, L"StringFromGUID2(startup elevation)", ERROR_INVALID_DATA);
        const std::wstring token = std::to_wstring(GetCurrentProcessId()) + L"." + text;
        const std::wstring name = std::wstring(kMappingPrefix) + token;
        // The original user and an alternate administrator can both access the
        // response. No request is stored in a writable file or command string.
        const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" +
            ProcessSid(GetCurrentProcess()) + L")";
        LocalMemory descriptor;
        Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
            &descriptor.value, nullptr), L"ConvertStringSecurityDescriptorToSecurityDescriptorW(startup IPC)");
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor.value, FALSE};
        Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE,
            0, sizeof(Request), name.c_str())};
        const DWORD mappingError = GetLastError();
        Check(mapping.value != nullptr, L"CreateFileMappingW(startup elevation)", mappingError);
        Check(mappingError != ERROR_ALREADY_EXISTS, L"CreateFileMappingW(startup elevation identity)", ERROR_ALREADY_EXISTS);
        View view{static_cast<Request*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)))};
        Check(view.value != nullptr, L"MapViewOfFile(startup elevation)");
        *view.value = Request{};
        view.value->processId = GetCurrentProcessId();
        view.value->enabled = enabled ? 1 : 0;
        view.value->owner = target.owner;
        Copy(view.value->executable, target.executable);
        Copy(view.value->arguments, target.arguments);
        Copy(view.value->directory, target.workingDirectory);
        const std::wstring executable = ProcessPath(GetCurrentProcess());
        const std::wstring arguments = std::wstring(kArgument) + token;
        SHELLEXECUTEINFOW launch{};
        launch.cbSize = sizeof(launch);
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        launch.lpVerb = L"runas";
        launch.lpFile = executable.c_str();
        launch.lpParameters = arguments.c_str();
        launch.nShow = SW_HIDE;
        Check(ShellExecuteExW(&launch), L"ShellExecuteExW(runas startup)");
        Handle child{launch.hProcess};
        Check(child.value != nullptr, L"ShellExecuteExW(startup process)", ERROR_INVALID_HANDLE);
        // UAC is a user decision, not a background timeout. Do not return and
        // start a competing fallback while the elevated writer is still alive.
        Check(WaitForSingleObject(child.value, INFINITE) == WAIT_OBJECT_0, L"WaitForSingleObject(startup elevation)");
        DWORD exitCode = ERROR_GEN_FAILURE;
        Check(GetExitCodeProcess(child.value, &exitCode), L"GetExitCodeProcess(startup elevation)");
        Check(InterlockedCompareExchange(&view.value->completed, 0, 0) == 1 && Terminated(view.value->error),
            L"AutoStart.ElevationResponse", exitCode == ERROR_SUCCESS ? ERROR_INVALID_DATA : exitCode);
        if (!view.value->success)
            throw Failure{view.value->error[0] ? view.value->error : DescribeError(L"AutoStart.ElevatedRegistration", E_FAIL)};
        return true;
    }
    catch (...)
    {
        const auto message = ExceptionError();
        WriteDiagnosticLogEntry(message.c_str(), DiagnosticLogLevel::Error);
        if (error) *error = message;
        return false;
    }
}

std::optional<int> TryRunElevationCommand() noexcept
{
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return std::nullopt;
    LocalMemory cleanup{arguments};
    std::wstring token;
    bool found = false;
    for (int index = 1; index < count; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument.starts_with(kArgument))
        {
            found = true;
            token = argument.substr(std::size(kArgument) - 1);
            break;
        }
    }
    if (!found) return std::nullopt;
    try
    {
        Check(!token.empty() && token.size() < 80 && token.find_first_not_of(L"0123456789abcdefABCDEF{}-.") == std::wstring::npos,
            L"AutoStart.ElevationToken", ERROR_INVALID_PARAMETER);
        const std::wstring name = std::wstring(kMappingPrefix) + token;
        Handle mapping{OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str())};
        Check(mapping.value != nullptr, L"OpenFileMappingW(startup elevation)");
        View view{static_cast<Request*>(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Request)))};
        Check(view.value != nullptr, L"MapViewOfFile(startup helper)");
        // Copy once before validation: a shared request must not change midway
        // through task registration. Responses contain only success/error text.
        const Request request = *view.value;
        std::wstring error;
        bool success = false;
        try
        {
            Check(request.magic == kMagic && request.size == sizeof(Request) && request.completed == 0 &&
                request.enabled <= 1 && token.starts_with(std::to_wstring(request.processId) + L".") &&
                (request.owner == UnifiedAutoStartOwner::Portable || request.owner == UnifiedAutoStartOwner::Steam ||
                    request.owner == UnifiedAutoStartOwner::Packaged) &&
                Terminated(request.executable) && Terminated(request.arguments) && Terminated(request.directory),
                L"AutoStart.ElevationRequest", ERROR_INVALID_DATA);
            Handle parent{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, request.processId)};
            Check(parent.value != nullptr, L"OpenProcess(startup owner)");
            Check(WaitForSingleObject(parent.value, 0) == WAIT_TIMEOUT, L"AutoStart.ElevationOwnerExited", ERROR_PROCESS_ABORTED);
            const auto parentPath = ProcessPath(parent.value);
            const auto currentPath = ProcessPath(GetCurrentProcess());
            Check(CompareStringOrdinal(parentPath.c_str(), -1, currentPath.c_str(), -1, TRUE) == CSTR_EQUAL,
                L"AutoStart.ElevationOwnerImage", ERROR_INVALID_DATA);
            const auto sid = ProcessSid(parent.value);
            const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(initialized)) throw Failure{DescribeError(L"CoInitializeEx(startup elevation)", initialized)};
            struct Apartment { ~Apartment() { CoUninitialize(); } } apartment;
            Target target{request.owner, request.executable, request.arguments, request.directory, {}};
            success = TaskStore(L"\\SnowDesktop", sid).Configure(target, request.enabled != 0,
                L"Starts the selected SnowDesktop deployment when this user signs in.", &error);
        }
        catch (...) { error = ExceptionError(); }
        // Bound the response even if an underlying Windows message is unusually long.
        Copy(view.value->error, std::wstring_view(error).substr(0, std::size(view.value->error) - 1));
        view.value->success = success ? 1 : 0;
        InterlockedExchange(&view.value->completed, 1);
        return success ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
    }
    catch (...)
    {
        const auto error = ExceptionError();
        WriteDiagnosticLogEntry(error.c_str(), DiagnosticLogLevel::Error);
        return ERROR_INVALID_DATA;
    }
}
}
