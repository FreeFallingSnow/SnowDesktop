#pragma once

#include <windows.h>
#include <shellapi.h>
#pragma push_macro("ShellExecute")
#undef ShellExecute
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <wrl/client.h>

#include <new>
#include <string>
#include <string_view>

namespace snowdesktop::auto_start_launch
{
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

struct Result
{
    bool handled = false;
    DWORD error = ERROR_SUCCESS;
    DWORD childProcessId = 0; // Shell automation does not return a process ID.
    const wchar_t* operation = L"AutoStart Explorer launch requested";
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
    explicit NativePlatform(ShellExecutor execute = &ExecuteThroughExplorer) : execute_(execute) {}

    DWORD Relaunch(std::wstring_view command)
    {
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
            // The Explorer-launched child takes the normal entry point.
            if (IsOwnerArgument(arguments.value[index])) continue;
            if (!parameters.empty()) parameters.push_back(L' ');
            parameters += QuoteArgument(arguments.value[index]);
        }
        execute_(executable, parameters, directory);
        return 0;
    }

private:
    ShellExecutor execute_;
};

template<typename Platform>
Result Normalize(std::wstring_view command, Platform& platform) noexcept
{
    try
    {
        if (!IsAutoStartCommand(command)) return {};
        return {true, ERROR_SUCCESS, platform.Relaunch(command)};
    }
    catch (const Failure& failure)
    {
        return {true, failure.code, 0, failure.operation};
    }
    catch (const std::bad_alloc&)
    {
        return {true, ERROR_OUTOFMEMORY, 0, L"AutoStart shell launch allocation"};
    }
    catch (...)
    {
        return {true, ERROR_GEN_FAILURE, 0, L"AutoStart shell launch exception"};
    }
}

inline Result Normalize() noexcept
{
    NativePlatform platform;
    return Normalize(GetCommandLineW(), platform);
}
}
#pragma pop_macro("ShellExecute")
