#pragma once

#include "auto_start_rules.h"
#include "startup_privileges_dialog.h"

#include <windows.h>
#include <shellapi.h>
#pragma push_macro("ShellExecute")
#undef ShellExecute
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <wrl/client.h>
#include <sddl.h>

#include <new>
#include <string>
#include <string_view>

namespace snowdesktop::auto_start_launch
{
// Internal handoff marker; desktop privileges are still checked after handoff.
inline constexpr wchar_t kExplorerRelaunchArgument[] = L"--snowdesktop-explorer-relaunch";
inline constexpr wchar_t kExplorerAckPrefix[] = L"--snowdesktop-explorer-ack=";
inline constexpr wchar_t kExplorerAckEventPrefix[] = L"Local\\SnowDesktop.ExplorerLaunch.";

struct Failure
{
    DWORD code;
    const wchar_t* operation;
};

inline void Require(bool success, const wchar_t* operation)
{
    if (!success)
    {
        const DWORD code = GetLastError();
        throw Failure{code ? code : ERROR_GEN_FAILURE, operation};
    }
}

struct Arguments
{
    LPWSTR* value = nullptr;
    int count = 0;
    explicit Arguments(std::wstring_view command)
    {
        const std::wstring terminated(command);
        value = CommandLineToArgvW(terminated.c_str(), &count);
        Require(value != nullptr, L"CommandLineToArgvW(autostart)");
    }
    Arguments(const Arguments&) = delete;
    Arguments& operator=(const Arguments&) = delete;
    ~Arguments() { LocalFree(value); }
};

inline bool IsExplorerAckArgument(std::wstring_view value)
{
    return value.starts_with(kExplorerAckPrefix);
}

inline void AcknowledgeExplorerLaunch(std::wstring_view command)
{
    const Arguments arguments(command);
    for (int index = 1; index < arguments.count; ++index)
    {
        const std::wstring_view argument(arguments.value[index]);
        if (!IsExplorerAckArgument(argument)) continue;
        const auto token = argument.substr(std::size(kExplorerAckPrefix) - 1);
        if (token.size() != 38 || token.find_first_not_of(L"{}-0123456789abcdefABCDEF") != std::wstring_view::npos)
            continue;
        const std::wstring name = std::wstring(kExplorerAckEventPrefix) + std::wstring(token);
        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str());
        if (event) { SetEvent(event); CloseHandle(event); }
    }
}

inline bool IsOwnerArgument(std::wstring_view value)
{
    return value == L"--snowdesktop-autostart-owner=portable" ||
        value == L"--snowdesktop-autostart-owner=packaged" ||
        value == L"--snowdesktop-autostart-owner=steam";
}

inline bool IsAutoStartCommand(std::wstring_view command)
{
    const Arguments arguments(command);
    for (int index = 1; index < arguments.count; ++index)
        if (IsOwnerArgument(arguments.value[index])) return true;
    return false;
}

inline bool IsPreviewHostCommand(std::wstring_view command) noexcept
{
    try
    {
        const Arguments arguments(command);
        return arguments.count > 1 &&
            (std::wstring_view(arguments.value[1]) == L"--widget-author-preview" ||
                std::wstring_view(arguments.value[1]) == L"--native-component-preview");
    }
    catch (...) { return false; } // Normalize will report an unreadable command.
}

struct Result
{
    bool handled = false;
    DWORD error = ERROR_SUCCESS;
    DWORD childProcessId = 0; // Shell automation does not return a process ID.
    const wchar_t* operation = L"Standard-user Explorer launch requested";
    bool currentPrivilegesAccepted = false;
};
inline std::wstring QuoteArgument(std::wstring_view argument)
{
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t value : argument)
    {
        if (value == L'\\') { ++slashes; continue; }
        result.append(value == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result.push_back(value);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

inline void CheckHr(HRESULT result, const wchar_t* operation)
{
    if (FAILED(result)) throw Failure{static_cast<DWORD>(result), operation};
}

inline void ExecuteThroughExplorerImpl(const std::wstring& executable,
    const std::wstring& parameters, const std::wstring& directory, int show)
{
    using Microsoft::WRL::ComPtr;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct ComScope
    {
        HRESULT result;
        ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com{initialized};
    if (initialized != RPC_E_CHANGED_MODE) CheckHr(initialized, L"CoInitializeEx(autostart shell)");
    ComPtr<IShellWindows> windows;
    CheckHr(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&windows)), L"CoCreateInstance(ShellWindows)");
    VARIANT location{}, root{};
    location.vt = VT_I4;
    location.lVal = CSIDL_DESKTOP;
    long window = 0;
    ComPtr<IDispatch> dispatch;
    const ULONGLONG deadline = GetTickCount64() + 30000;
    HRESULT found = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window,
        SWFO_NEEDDISPATCH, &dispatch);
    while ((FAILED(found) || !dispatch) && GetTickCount64() < deadline)
    {
        Sleep(100);
        dispatch.Reset();
        found = windows->FindWindowSW(&location, &root, SWC_DESKTOP, &window,
            SWFO_NEEDDISPATCH, &dispatch);
    }
    CheckHr(found, L"IShellWindows::FindWindowSW(desktop)");
    if (!dispatch) throw Failure{ERROR_TIMEOUT, L"FindWindowSW(autostart desktop)"};
    ComPtr<IServiceProvider> provider;
    CheckHr(dispatch.As(&provider), L"QueryInterface(desktop service provider)");
    ComPtr<IShellBrowser> browser;
    CheckHr(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser)),
        L"QueryService(desktop browser)");
    ComPtr<IShellView> view;
    CheckHr(browser->QueryActiveShellView(&view), L"QueryActiveShellView(desktop)");
    dispatch.Reset();
    CheckHr(view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&dispatch)),
        L"GetItemObject(desktop automation)");
    ComPtr<IShellFolderViewDual> folder;
    CheckHr(dispatch.As(&folder), L"QueryInterface(desktop folder view)");
    dispatch.Reset();
    CheckHr(folder->get_Application(&dispatch), L"IShellFolderViewDual::get_Application");
    ComPtr<IShellDispatch2> shell;
    CheckHr(dispatch.As(&shell), L"QueryInterface(ShellDispatch2)");
    struct Bstr
    {
        BSTR value;
        explicit Bstr(const std::wstring& text)
            : value(SysAllocStringLen(text.data(), static_cast<UINT>(text.size())))
        {
            if (!value) throw Failure{ERROR_OUTOFMEMORY, L"SysAllocString(autostart shell)"};
        }
        ~Bstr() { SysFreeString(value); }
    };
    const Bstr file(executable), args(parameters), cwd(directory), operation(L"open");
    // These variants borrow the BSTRs above for the synchronous COM call.
    VARIANT arg{}, dir{}, verb{}, display{};
    arg.vt = dir.vt = verb.vt = VT_BSTR;
    arg.bstrVal = args.value;
    dir.bstrVal = cwd.value;
    verb.bstrVal = operation.value;
    display.vt = VT_I4;
    display.lVal = show;
    CheckHr(shell->ShellExecute(file.value, arg, dir, verb, display),
        L"IShellDispatch2::ShellExecute(autostart)");
}

inline void ExecuteThroughExplorer(const std::wstring& executable,
    const std::wstring& parameters, const std::wstring& directory)
{
    ExecuteThroughExplorerImpl(executable, parameters, directory, SW_SHOWNORMAL);
}

using ShellExecutor = decltype(&ExecuteThroughExplorer);

class NativePlatform final
{
public:
    explicit NativePlatform(ShellExecutor execute = &ExecuteThroughExplorer, DWORD handoffTimeout = 15000)
        : execute_(execute), handoffTimeout_(handoffTimeout) {}

    bool IsElevated()
    {
        return ProcessIsElevated(GetCurrentProcess());
    }

    bool IsExplorerElevated()
    {
        // At logon the interactive shell may not have created its window yet.
        const ULONGLONG deadline = GetTickCount64() + 30000;
        HWND shell = GetShellWindow();
        while (!shell && GetTickCount64() < deadline)
        {
            Sleep(100);
            shell = GetShellWindow();
        }
        if (!shell) throw Failure{ERROR_TIMEOUT, L"GetShellWindow(startup privileges)"};
        DWORD processId = 0;
        Require(GetWindowThreadProcessId(shell, &processId) != 0 && processId != 0,
            L"GetWindowThreadProcessId(Explorer startup privileges)");
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId)};
        Require(process.value != nullptr, L"OpenProcess(Explorer startup privileges)");
        return ProcessIsElevated(process.value);
    }

    bool ConfirmContinue(const Failure& failure)
    {
        return ConfirmCurrentStartupPrivileges(failure.operation, failure.code);
    }

private:
    struct Handle
    {
        HANDLE value = nullptr;
        ~Handle() { if (value) CloseHandle(value); }
    };

    static bool ProcessIsElevated(HANDLE process)
    {
        Handle token;
        Require(OpenProcessToken(process, TOKEN_QUERY, &token.value) != FALSE,
            L"OpenProcessToken(startup privileges)");
        TOKEN_ELEVATION elevation{};
        DWORD size = 0;
        Require(GetTokenInformation(token.value, TokenElevation, &elevation,
            sizeof(elevation), &size) != FALSE, L"GetTokenInformation(startup elevation)");
        alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE]{};
        Require(GetTokenInformation(token.value, TokenIntegrityLevel, buffer,
            sizeof(buffer), &size) != FALSE, L"GetTokenInformation(startup integrity)");
        const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer);
        if (!IsValidSid(label->Label.Sid) || *GetSidSubAuthorityCount(label->Label.Sid) == 0)
            throw Failure{ERROR_INVALID_SID, L"Startup integrity SID"};
        const DWORD integrity = *GetSidSubAuthority(label->Label.Sid,
            static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
        return elevation.TokenIsElevated != 0 || integrity > SECURITY_MANDATORY_MEDIUM_RID;
    }

public:
    DWORD Relaunch(std::wstring_view command)
    {
        GUID token{};
        CheckHr(CoCreateGuid(&token), L"CoCreateGuid(Explorer launch acknowledgement)");
        wchar_t tokenText[40]{};
        Require(StringFromGUID2(token, tokenText, static_cast<int>(std::size(tokenText))) != 0,
            L"StringFromGUID2(Explorer launch acknowledgement)");
        struct SecurityDescriptor
        {
            PSECURITY_DESCRIPTOR value = nullptr;
            ~SecurityDescriptor() { if (value) LocalFree(value); }
        } descriptor;
        // The ordinary Explorer child must be able to signal an elevated
        // parent's event. This acknowledgement never authorizes privileges.
        Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;IU)S:(ML;;NW;;;ME)", SDDL_REVISION_1,
            &descriptor.value, nullptr) != FALSE, L"Explorer launch acknowledgement security");
        SECURITY_ATTRIBUTES security{sizeof(security), descriptor.value, FALSE};
        const std::wstring eventName = std::wstring(kExplorerAckEventPrefix) + tokenText;
        Handle acknowledgement{CreateEventW(&security, TRUE, FALSE, eventName.c_str())};
        const DWORD eventError = GetLastError();
        Require(acknowledgement.value != nullptr, L"CreateEvent(Explorer launch acknowledgement)");
        if (eventError == ERROR_ALREADY_EXISTS)
            throw Failure{ERROR_ALREADY_EXISTS, L"Explorer launch acknowledgement identity"};
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        Require(length != 0 && length < executable.size(), L"GetModuleFileNameW(autostart)");
        executable.resize(length);
        const DWORD directorySize = GetCurrentDirectoryW(0, nullptr);
        Require(directorySize != 0, L"GetCurrentDirectoryW(size)");
        std::wstring directory(directorySize, L'\0');
        const DWORD copied = GetCurrentDirectoryW(directorySize, directory.data());
        Require(copied != 0 && copied < directorySize, L"GetCurrentDirectoryW");
        directory.resize(copied);
        const Arguments arguments(command);
        std::wstring parameters;
        for (int index = 1; index < arguments.count; ++index)
        {
            if (!parameters.empty()) parameters.push_back(L' ');
            // Retain background intent, but do not delegate the child again.
            parameters += QuoteArgument(IsOwnerArgument(arguments.value[index]) ?
                kAutoStartBackgroundArgument : arguments.value[index]);
        }
        if (!parameters.empty()) parameters.push_back(L' ');
        parameters += QuoteArgument(std::wstring(kExplorerAckPrefix) + tokenText);
        parameters.push_back(L' ');
        parameters += QuoteArgument(kExplorerRelaunchArgument);
        execute_(executable, parameters, directory);
        const DWORD waited = WaitForSingleObject(acknowledgement.value, handoffTimeout_);
        if (waited != WAIT_OBJECT_0)
            throw Failure{waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError(),
                L"Explorer child did not acknowledge startup"};
        return 0;
    }

private:
    ShellExecutor execute_;
    DWORD handoffTimeout_;
};

template<typename Platform>
Result Normalize(std::wstring_view command, Platform& platform,
    bool desktopLaunch = true, bool currentPrivilegesAccepted = false) noexcept
{
    const auto failureResult = [&](const Failure& failure) noexcept -> Result {
        try
        {
            if (platform.ConfirmContinue(failure))
                return {false, failure.code, 0, failure.operation, true};
        }
        catch (...) {} // A failed dialog is never consent.
        return {true, ERROR_CANCELLED, 0, failure.operation};
    };
    try
    {
        // Helpers with inherited handles cannot survive an Explorer handoff.
        // Their real dispatch/validation runs separately from desktop startup.
        if (!desktopLaunch) return {};
        AcknowledgeExplorerLaunch(command);
        // Only the supervising launcher's PID-bound startup channel supplies
        // this decision. It is not a persistent setting or a command-line flag.
        if (currentPrivilegesAccepted) return {false, ERROR_SUCCESS, 0, L"Startup privileges accepted", true};
        const bool elevated = platform.IsElevated();
        const Arguments arguments(command);
        bool autoStart = false;
        bool relaunched = false;
        for (int index = 1; index < arguments.count; ++index)
        {
            autoStart = autoStart || IsOwnerArgument(arguments.value[index]);
            relaunched = relaunched || arguments.value[index] ==
                std::wstring_view(kExplorerRelaunchArgument);
        }
        if (relaunched && !elevated) return {};
        if (!autoStart && !elevated) return {};
        // A high-privilege Explorer cannot lower us. Match that desktop
        // without another process, including login and already-handed-off launches.
        if (platform.IsExplorerElevated()) return {};
        if (relaunched)
        {
            // RUNASADMIN may elevate the child again even with an ordinary
            // Explorer. Ask before continuing; never loop through Explorer.
            throw Failure{ERROR_ELEVATION_REQUIRED, L"Explorer child still has elevated startup privileges"};
        }
        return {true, ERROR_SUCCESS, platform.Relaunch(command)};
    }
    catch (const Failure& failure)
    {
        return failureResult(failure);
    }
    catch (const std::bad_alloc&)
    {
        return failureResult({ERROR_OUTOFMEMORY, L"AutoStart shell launch allocation"});
    }
    catch (...)
    {
        return failureResult({ERROR_GEN_FAILURE, L"AutoStart shell launch exception"});
    }
}

inline Result Normalize(bool desktopLaunch = true, bool currentPrivilegesAccepted = false) noexcept
{
    NativePlatform platform;
    return Normalize(GetCommandLineW(), platform, desktopLaunch, currentPrivilegesAccepted);
}
}
#pragma pop_macro("ShellExecute")
