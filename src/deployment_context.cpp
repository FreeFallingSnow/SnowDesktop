/**
 * @file deployment_context.cpp
 * @brief 便携版与 MSIX 版的运行时部署环境适配实现。
 */

#include "deployment_context.h"
#include "data_paths.h"
#include "taskbar_hook/taskbar_hook_protocol.h"

#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <future>
#include <iterator>
#include <cwchar>
#include <mutex>
#include <system_error>
#include <utility>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

namespace
{
#define SNOWDESKTOP_WIDEN_INNER(value) L##value
#define SNOWDESKTOP_WIDEN(value) SNOWDESKTOP_WIDEN_INNER(value)

constexpr wchar_t kStartupTaskId[] = L"SnowDesktopStartup";
constexpr wchar_t kRuntimeDirectory[] = L"SnowDesktop.Runtime";
constexpr wchar_t kTaskbarHookFilename[] = L"SnowDesktopTaskbarHook.dll";
constexpr wchar_t kVersion[] = SNOWDESKTOP_WIDEN(SNOWDESKTOP_VERSION);
constexpr wchar_t kStoreId[] = SNOWDESKTOP_WIDEN(SNOWDESKTOP_STORE_ID);
constexpr wchar_t kPackageFamilyName[] =
    SNOWDESKTOP_WIDEN(SNOWDESKTOP_PACKAGE_FAMILY_NAME);

std::wstring GetCurrentPackageFamilyNameString()
{
    UINT32 length = 0;
    const LONG sizeResult = GetCurrentPackageFamilyName(&length, nullptr);
    if (sizeResult != ERROR_INSUFFICIENT_BUFFER || length <= 1)
        return {};

    std::wstring familyName(length, L'\0');
    if (GetCurrentPackageFamilyName(&length, familyName.data()) != ERROR_SUCCESS)
        return {};
    familyName.resize(length - 1);
    return familyName;
}

bool IsBareFilename(const wchar_t* filename) noexcept
{
    if (!filename || !*filename)
        return false;
    for (const wchar_t* character = filename; *character; ++character)
    {
        if (*character == L'\\' || *character == L'/' ||
            *character == L':')
        {
            return false;
        }
    }
    return true;
}

snowdesktop::deployment::UnvirtualizedRegistryValue
QueryCurrentUserValueDirect(
    const wchar_t* subKey, const wchar_t* valueName) noexcept
{
    snowdesktop::deployment::UnvirtualizedRegistryValue result;
    HKEY key = nullptr;
    const LONG opened = RegOpenKeyExW(
        HKEY_CURRENT_USER, subKey, 0, KEY_QUERY_VALUE, &key);
    if (opened != ERROR_SUCCESS)
    {
        result.win32Result = static_cast<std::uint32_t>(opened);
        return result;
    }
    DWORD type = REG_NONE;
    DWORD size = static_cast<DWORD>(result.data.size());
    const LONG queried = RegQueryValueExW(key, valueName, nullptr,
        &type, result.data.data(), &size);
    RegCloseKey(key);
    result.win32Result = static_cast<std::uint32_t>(queried);
    result.type = type;
    result.size = size;
    return result;
}

std::wstring RegistryQueryMappingName(DWORD ownerProcessId)
{
    return std::wstring(
        snowdesktop::taskbar_hook::kRegistryQueryMappingPrefix) + L"." +
        std::to_wstring(ownerProcessId);
}

snowdesktop::deployment::UnvirtualizedRegistryValue
QueryCurrentUserValueThroughExplorer(
    const wchar_t* subKey, const wchar_t* valueName) noexcept
{
    using namespace snowdesktop::taskbar_hook;
    snowdesktop::deployment::UnvirtualizedRegistryValue result;
    result.win32Result = ERROR_INVALID_PARAMETER;
    if (!subKey || !valueName || !*subKey || !*valueName ||
        wcslen(subKey) >= kMaximumRegistrySubKeyLength ||
        wcslen(valueName) >= kMaximumRegistryValueNameLength)
    {
        return result;
    }

    static std::mutex queryMutex;
    std::lock_guard queryLock(queryMutex);
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD explorerProcessId = 0;
    const DWORD taskbarThreadId = taskbar
        ? GetWindowThreadProcessId(taskbar, &explorerProcessId) : 0;
    if (!taskbar || !taskbarThreadId || !explorerProcessId)
    {
        result.win32Result = ERROR_NOT_READY;
        return result;
    }

    const std::wstring hookPath =
        snowdesktop::deployment::GetTaskbarHookPath();
    HMODULE module = hookPath.empty()
        ? nullptr : LoadLibraryW(hookPath.c_str());
    if (!module)
    {
        result.win32Result = GetLastError();
        return result;
    }
    auto hookProcedure = reinterpret_cast<HOOKPROC>(GetProcAddress(
        module, "SnowDesktopRegistryQueryHookProc"));
    if (!hookProcedure)
    {
        result.win32Result = GetLastError();
        FreeLibrary(module);
        return result;
    }

    const DWORD ownerProcessId = GetCurrentProcessId();
    const std::wstring mappingName =
        RegistryQueryMappingName(ownerProcessId);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedRegistryQueryState),
        mappingName.c_str());
    if (!mapping)
    {
        result.win32Result = GetLastError();
        FreeLibrary(module);
        return result;
    }
    auto* state = static_cast<SharedRegistryQueryState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0,
        sizeof(SharedRegistryQueryState)));
    if (!state)
    {
        result.win32Result = GetLastError();
        CloseHandle(mapping);
        FreeLibrary(module);
        return result;
    }
    *state = SharedRegistryQueryState{};
    state->ownerProcessId = ownerProcessId;
    wcscpy_s(state->subKey, subKey);
    wcscpy_s(state->valueName, valueName);
    MemoryBarrier();

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, hookProcedure,
        module, taskbarThreadId);
    if (!hook)
    {
        result.win32Result = GetLastError();
    }
    else
    {
        const UINT message = RegisterWindowMessageW(
            kRegistryQueryMessageName);
        DWORD_PTR ignored = 0;
        if (!message || !SendMessageTimeoutW(taskbar, message,
                ownerProcessId, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                2000, &ignored))
        {
            result.win32Result = GetLastError();
            if (result.win32Result == ERROR_SUCCESS)
                result.win32Result = ERROR_TIMEOUT;
        }
        else if (state->status != kRegistryQueryCompleted)
        {
            result.win32Result = ERROR_RETRY;
        }
        else
        {
            result.win32Result =
                static_cast<std::uint32_t>(state->queryResult);
            result.type = state->valueType;
            result.size = state->valueSize;
            const std::size_t copySize = std::min<std::size_t>(
                result.data.size(), state->valueSize);
            std::copy_n(state->value, copySize, result.data.begin());
        }
        UnhookWindowsHookEx(hook);
    }
    UnmapViewOfFile(state);
    CloseHandle(mapping);
    FreeLibrary(module);
    return result;
}

std::filesystem::path GetTemporaryDirectory()
{
    const DWORD required = GetTempPathW(0, nullptr);
    if (required == 0)
        return {};
    std::wstring buffer(required, L'\0');
    const DWORD length = GetTempPathW(required, buffer.data());
    if (length == 0 || length >= required)
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer);
}

std::filesystem::path CreateInjectableRuntimeDirectory()
{
    const std::filesystem::path temporary = GetTemporaryDirectory();
    if (temporary.empty())
        return {};
    const std::filesystem::path copiesRoot = temporary /
        L"SnowDesktop" / L"RuntimeHooks";

    std::error_code error;
    std::filesystem::create_directories(copiesRoot, error);
    if (error)
        return {};

    // Injected modules can remain mapped after the owner exits. Remove only
    // stale copies that Windows no longer considers busy; locked copies are
    // kept until their target process releases them.
    for (std::filesystem::directory_iterator iterator(copiesRoot, error), end;
         !error && iterator != end; iterator.increment(error))
    {
        std::error_code cleanupError;
        if (iterator->is_directory(cleanupError))
            std::filesystem::remove_all(iterator->path(), cleanupError);
    }

    const std::filesystem::path targetDirectory = copiesRoot /
        (std::wstring(kVersion) + L"-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    error.clear();
    std::filesystem::create_directories(targetDirectory, error);
    if (error)
        return {};
    return targetDirectory;
}

std::wstring DeployInjectableRuntimeCopy(const wchar_t* filename)
{
    const std::filesystem::path source =
        snowdesktop::deployment::GetRuntimeFilePath(filename);
    if (!std::filesystem::is_regular_file(source))
        return source.wstring();

    static const std::filesystem::path targetDirectory =
        CreateInjectableRuntimeDirectory();
    if (targetDirectory.empty())
        return source.wstring();

    const std::filesystem::path target =
        targetDirectory / source.filename();
    std::error_code error;
    if (std::filesystem::is_regular_file(target, error))
        return target.wstring();
    error.clear();
    std::filesystem::copy_file(source, target,
        std::filesystem::copy_options::none, error);
    if (!error && std::filesystem::is_regular_file(target))
        return target.wstring();
    return source.wstring();
}

snowdesktop::deployment::PackagedAutoStartState ConvertStartupTaskState(
    winrt::Windows::ApplicationModel::StartupTaskState state)
{
    using winrt::Windows::ApplicationModel::StartupTaskState;
    using snowdesktop::deployment::PackagedAutoStartState;
    switch (state)
    {
    case StartupTaskState::Disabled:
        return PackagedAutoStartState::Disabled;
    case StartupTaskState::DisabledByUser:
        return PackagedAutoStartState::DisabledByUser;
    case StartupTaskState::Enabled:
        return PackagedAutoStartState::Enabled;
    case StartupTaskState::DisabledByPolicy:
        return PackagedAutoStartState::DisabledByPolicy;
    case StartupTaskState::EnabledByPolicy:
        return PackagedAutoStartState::EnabledByPolicy;
    default:
        return PackagedAutoStartState::Unavailable;
    }
}

snowdesktop::deployment::PackagedAutoStartState ConvertStartupTaskState(
    DWORD state)
{
    using snowdesktop::deployment::PackagedAutoStartState;
    switch (state)
    {
    case 0:
        return PackagedAutoStartState::Disabled;
    case 1:
        return PackagedAutoStartState::DisabledByUser;
    case 2:
        return PackagedAutoStartState::Enabled;
    case 3:
        return PackagedAutoStartState::DisabledByPolicy;
    case 4:
        return PackagedAutoStartState::EnabledByPolicy;
    default:
        return PackagedAutoStartState::Unavailable;
    }
}

bool IsStorePackageRegisteredForCurrentUser() noexcept
{
    UINT32 packageCount = 0;
    UINT32 bufferLength = 0;
    const LONG result = GetPackagesByPackageFamily(
        kPackageFamilyName, &packageCount, nullptr, &bufferLength, nullptr);
    return result == ERROR_INSUFFICIENT_BUFFER && packageCount > 0;
}

winrt::Windows::ApplicationModel::StartupTask
GetStartupTaskOnCurrentApartment()
{
    using winrt::Windows::ApplicationModel::IStartupTaskStatics;
    using winrt::Windows::ApplicationModel::StartupTask;

    // This helper runs on a short-lived MTA. StartupTask::GetAsync uses the
    // process-wide C++/WinRT activation-factory cache; after this apartment is
    // uninitialized, that cache can retain a proxy whose backing stub has been
    // unloaded. A later settings query then dereferences the stale proxy.
    // Acquire an uncached factory so it is released on this apartment before
    // RunStartupTaskOnMta calls uninit_apartment().
    auto factory = winrt::try_get_activation_factory<IStartupTaskStatics>(
        winrt::name_of<StartupTask>());
    if (!factory)
        throw winrt::hresult_error(REGDB_E_CLASSNOTREG);
    return factory.GetAsync(kStartupTaskId).get();
}

template<typename Callback>
snowdesktop::deployment::PackagedAutoStartState RunStartupTaskOnMta(
    Callback&& callback) noexcept
{
    using snowdesktop::deployment::PackagedAutoStartState;
    try
    {
        auto result = std::async(
            std::launch::async,
            [operation = std::forward<Callback>(callback)]() mutable
            {
                winrt::init_apartment(
                    winrt::apartment_type::multi_threaded);
                struct ApartmentScope final
                {
                    ~ApartmentScope()
                    {
                        winrt::uninit_apartment();
                    }
                } apartmentScope;
                return ConvertStartupTaskState(operation());
            });
        return result.get();
    }
    catch (...)
    {
        return PackagedAutoStartState::Unavailable;
    }
}
}

namespace snowdesktop::deployment
{
bool IsPackaged() noexcept
{
    static const bool packaged = [] {
        UINT32 length = 0;
        return GetCurrentPackageFullName(&length, nullptr) ==
            ERROR_INSUFFICIENT_BUFFER;
    }();
    return packaged;
}

std::wstring GetPackageLocalStatePath()
{
    if (!IsPackaged())
        return {};

    const std::wstring familyName = GetCurrentPackageFamilyNameString();
    if (familyName.empty())
        return {};

    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(
            FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData)))
        return {};

    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    path /= L"Packages";
    path /= familyName;
    path /= L"LocalState";

    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error && !std::filesystem::is_directory(path))
        return {};
    return path.wstring();
}

std::wstring GetRuntimeFilePath(const wchar_t* filename)
{
    if (!IsBareFilename(filename))
        return filename ? std::wstring(filename) : std::wstring();

    const std::filesystem::path executableDirectory =
        GetExecutableDirectoryPath();
    const std::filesystem::path runtimePath =
        executableDirectory / kRuntimeDirectory / filename;
    if (std::filesystem::is_regular_file(runtimePath))
        return runtimePath.wstring();
    return (executableDirectory / filename).wstring();
}

std::wstring GetInjectableRuntimeFilePath(const wchar_t* filename)
{
    if (!IsBareFilename(filename))
        return {};
    static std::mutex deploymentMutex;
    std::lock_guard lock(deploymentMutex);
    return DeployInjectableRuntimeCopy(filename);
}

std::wstring GetTaskbarHookPath()
{
    static const std::wstring deployedPath =
        GetInjectableRuntimeFilePath(kTaskbarHookFilename);
    return deployedPath;
}

UnvirtualizedRegistryValue QueryUnvirtualizedCurrentUserValue(
    const wchar_t* subKey, const wchar_t* valueName) noexcept
{
    if (!subKey || !valueName)
    {
        UnvirtualizedRegistryValue result;
        result.win32Result = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (!IsPackaged())
        return QueryCurrentUserValueDirect(subKey, valueName);
    return QueryCurrentUserValueThroughExplorer(subKey, valueName);
}

std::wstring GetStoreProductPageUri()
{
    if (kStoreId[0] == L'\0')
        return {};
    return std::wstring(L"ms-windows-store://pdp/?ProductId=") +
        kStoreId;
}

PackagedAutoStartState GetPackagedAutoStartState() noexcept
{
    if (!IsPackaged())
        return PackagedAutoStartState::Unavailable;

    return RunStartupTaskOnMta([] {
        const auto task = GetStartupTaskOnCurrentApartment();
        return task.State();
    });
}

PackagedAutoStartState GetInstalledPackagedAutoStartState() noexcept
{
    if (IsPackaged())
        return GetPackagedAutoStartState();
    if (!IsStorePackageRegisteredForCurrentUser())
        return PackagedAutoStartState::Unavailable;

    const std::wstring subKey =
        std::wstring(
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\"
            L"Windows\\CurrentVersion\\AppModel\\SystemAppData\\") +
        kPackageFamilyName + L"\\" + kStartupTaskId;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return PackagedAutoStartState::Unavailable;
    }

    DWORD state = 0;
    DWORD type = 0;
    DWORD size = sizeof(state);
    const LONG result = RegQueryValueExW(key, L"State", nullptr, &type,
        reinterpret_cast<BYTE*>(&state), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_DWORD ||
        size != sizeof(state))
    {
        return PackagedAutoStartState::Unavailable;
    }
    return ConvertStartupTaskState(state);
}

PackagedAutoStartState SetPackagedAutoStartEnabled(bool enable) noexcept
{
    if (!IsPackaged())
        return PackagedAutoStartState::Unavailable;

    return RunStartupTaskOnMta([enable] {
        using winrt::Windows::ApplicationModel::StartupTaskState;
        const auto task = GetStartupTaskOnCurrentApartment();
        auto state = task.State();
        if (enable)
        {
            if (state == StartupTaskState::Disabled)
                task.RequestEnableAsync().get();
        }
        else if (state == StartupTaskState::Enabled)
        {
            task.Disable();
        }

        const auto refreshedTask = GetStartupTaskOnCurrentApartment();
        return refreshedTask.State();
    });
}

bool IsPackagedAutoStartStateEnabled(
    PackagedAutoStartState state) noexcept
{
    return state == PackagedAutoStartState::Enabled ||
        state == PackagedAutoStartState::EnabledByPolicy;
}
}
