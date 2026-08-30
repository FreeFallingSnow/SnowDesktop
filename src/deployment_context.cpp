/**
 * @file deployment_context.cpp
 * @brief 便携版与 MSIX 版的运行时部署环境适配实现。
 */

#include "deployment_context.h"
#include "data_paths.h"
#include "taskbar_hook/taskbar_hook_protocol.h"

#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <future>
#include <iterator>
#include <cwchar>
#include <mutex>
#include <optional>
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
constexpr wchar_t kRuntimeHookOwnerLockFilename[] = L".owner.lock";
constexpr wchar_t kVersion[] = SNOWDESKTOP_WIDEN(SNOWDESKTOP_VERSION);
constexpr wchar_t kStoreId[] = SNOWDESKTOP_WIDEN(SNOWDESKTOP_STORE_ID);
constexpr wchar_t kPackageFamilyName[] =
    SNOWDESKTOP_WIDEN(SNOWDESKTOP_PACKAGE_FAMILY_NAME);
constexpr wchar_t kPackageApplicationId[] = L"SnowDesktop";
constexpr wchar_t kPackagedStartupQueryArgumentPrefix[] =
    L"--snowdesktop-packaged-startup-operation=";
constexpr wchar_t kPackagedStartupQueryMappingPrefix[] =
    L"Local\\SnowDesktop.PackagedStartupQuery.Mapping.";
constexpr wchar_t kPackagedStartupQueryEventPrefix[] =
    L"Local\\SnowDesktop.PackagedStartupQuery.Event.";
constexpr std::uint32_t kPackagedStartupQueryMagic = 0x53445051; // "SDPQ"
constexpr std::uint32_t kPackagedStartupQueryVersion = 2;
constexpr LONG kPackagedStartupQueryPending = 0;
constexpr LONG kPackagedStartupQueryCompleted = 1;

enum class PackagedStartupOperation : std::uint32_t
{
    Query = 0,
    Enable = 1,
    Disable = 2,
};

struct SharedPackagedStartupQueryState
{
    std::uint32_t magic = kPackagedStartupQueryMagic;
    std::uint32_t version = kPackagedStartupQueryVersion;
    std::uint32_t size = sizeof(SharedPackagedStartupQueryState);
    DWORD ownerProcessId = 0;
    PackagedStartupOperation operation = PackagedStartupOperation::Query;
    LONG status = kPackagedStartupQueryPending;
    snowdesktop::deployment::PackagedAutoStartState state =
        snowdesktop::deployment::PackagedAutoStartState::Unavailable;
};

std::wstring PackagedStartupQueryObjectName(
    const wchar_t* prefix, std::wstring_view token)
{
    std::wstring result(prefix);
    result.append(token);
    return result;
}

bool IsValidPackagedStartupQueryToken(std::wstring_view token) noexcept
{
    if (token.empty() || token.size() > 96)
        return false;
    for (const wchar_t character : token)
    {
        if (!iswalnum(character) && character != L'.' &&
            character != L'-')
        {
            return false;
        }
    }
    return true;
}

std::wstring CreatePackagedStartupQueryToken()
{
    GUID identifier{};
    if (FAILED(CoCreateGuid(&identifier)))
        return {};
    wchar_t guid[40]{};
    if (StringFromGUID2(identifier, guid, static_cast<int>(std::size(guid))) <=
        2)
    {
        return {};
    }
    std::wstring token = std::to_wstring(GetCurrentProcessId());
    token.push_back(L'.');
    for (const wchar_t character : std::wstring_view(guid))
    {
        if (iswalnum(character) || character == L'-')
            token.push_back(character);
    }
    return token;
}

std::optional<std::wstring> PackagedStartupQueryTokenFromCommandLine()
{
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(
        GetCommandLineW(), &argumentCount);
    if (!arguments)
        return std::nullopt;
    std::optional<std::wstring> result;
    const std::wstring_view prefix(kPackagedStartupQueryArgumentPrefix);
    for (int index = 1; index < argumentCount; ++index)
    {
        const std::wstring_view argument(arguments[index]);
        if (argument.starts_with(prefix))
        {
            const std::wstring_view token = argument.substr(prefix.size());
            if (IsValidPackagedStartupQueryToken(token))
                result = std::wstring(token);
            break;
        }
    }
    LocalFree(arguments);
    return result;
}

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

std::uint32_t DeleteCurrentUserValueDirect(
    const wchar_t* subKey, const wchar_t* valueName) noexcept
{
    HKEY key = nullptr;
    const LONG opened = RegOpenKeyExW(
        HKEY_CURRENT_USER, subKey, 0, KEY_SET_VALUE, &key);
    if (opened != ERROR_SUCCESS)
        return static_cast<std::uint32_t>(opened);
    const LONG deleted = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    return static_cast<std::uint32_t>(deleted);
}

std::uint32_t SetCurrentUserValueDirect(
    const wchar_t* subKey, const wchar_t* valueName, std::uint32_t type,
    const void* data, std::uint32_t size) noexcept
{
    HKEY key = nullptr;
    const LONG created = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0,
        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS)
        return static_cast<std::uint32_t>(created);
    const LONG written = RegSetValueExW(key, valueName, 0, type,
        static_cast<const BYTE*>(data), size);
    RegCloseKey(key);
    return static_cast<std::uint32_t>(written);
}

std::wstring RegistryQueryMappingName(DWORD ownerProcessId)
{
    return std::wstring(
        snowdesktop::taskbar_hook::kRegistryQueryMappingPrefix) + L"." +
        std::to_wstring(ownerProcessId);
}

snowdesktop::deployment::UnvirtualizedRegistryValue
QueryCurrentUserValueThroughExplorer(
    const wchar_t* subKey, const wchar_t* valueName,
    snowdesktop::taskbar_hook::RegistryOperation operation =
        snowdesktop::taskbar_hook::RegistryOperation::Query,
    std::uint32_t valueType = REG_NONE, const void* valueData = nullptr,
    std::uint32_t valueSize = 0) noexcept
{
    using namespace snowdesktop::taskbar_hook;
    snowdesktop::deployment::UnvirtualizedRegistryValue result;
    result.win32Result = ERROR_INVALID_PARAMETER;
    if (!subKey || !valueName || !*subKey || !*valueName ||
        wcslen(subKey) >= kMaximumRegistrySubKeyLength ||
        wcslen(valueName) >= kMaximumRegistryValueNameLength ||
        valueSize > kMaximumRegistryValueBytes ||
        (valueSize != 0 && !valueData))
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
    state->operation = operation;
    wcscpy_s(state->subKey, subKey);
    wcscpy_s(state->valueName, valueName);
    state->valueType = valueType;
    state->valueSize = valueSize;
    if (valueSize != 0)
    {
        std::copy_n(static_cast<const BYTE*>(valueData), valueSize,
            state->value);
    }
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
                static_cast<std::uint32_t>(state->operationResult);
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

bool IsReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsLegacyRuntimeDirectoryOwnerAlive(
    const std::filesystem::path& directory)
{
    const std::wstring name = directory.filename().wstring();
    const std::size_t suffix = name.rfind(L'-');
    const std::size_t separator = suffix == std::wstring::npos
        ? std::wstring::npos : name.rfind(L'-', suffix - 1);
    if (separator == std::wstring::npos || suffix == std::wstring::npos ||
        separator + 1 >= suffix)
        return true;
    const std::wstring processText =
        name.substr(separator + 1, suffix - separator - 1);
    wchar_t* end = nullptr;
    const unsigned long processId =
        std::wcstoul(processText.c_str(), &end, 10);
    if (processId == 0 || !end || *end != L'\0')
        return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE,
        static_cast<DWORD>(processId));
    if (!process)
        return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT || state == WAIT_FAILED;
}

bool RuntimeDirectoryHasLiveOwner(const std::filesystem::path& directory)
{
    const auto ownerLock = directory / kRuntimeHookOwnerLockFilename;
    const DWORD attributes = GetFileAttributesW(ownerLock.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return IsLegacyRuntimeDirectoryOwnerAlive(directory);
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return true;
    HANDLE probe = CreateFileW(ownerLock.c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe == INVALID_HANDLE_VALUE)
        return true;
    CloseHandle(probe);
    return false;
}

void CleanupStaleRuntimeDirectories(
    const std::filesystem::path& copiesRoot)
{
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(copiesRoot,
             std::filesystem::directory_options::skip_permission_denied,
             error), end;
         !error && iterator != end; iterator.increment(error))
    {
        std::error_code entryError;
        if (!iterator->is_directory(entryError) || entryError ||
            IsReparsePoint(iterator->path()) ||
            RuntimeDirectoryHasLiveOwner(iterator->path()))
            continue;
        std::filesystem::remove_all(iterator->path(), entryError);
    }
}

class InjectableRuntimeDirectory
{
public:
    InjectableRuntimeDirectory()
    {
        const std::filesystem::path temporary = GetTemporaryDirectory();
        if (temporary.empty())
            return;
        copiesRoot_ = temporary / L"SnowDesktop" / L"RuntimeHooks";

        std::error_code error;
        std::filesystem::create_directories(copiesRoot_, error);
        if (error || IsReparsePoint(copiesRoot_))
        {
            copiesRoot_.clear();
            return;
        }
        CleanupStaleRuntimeDirectories(copiesRoot_);

        path_ = copiesRoot_ / (std::wstring(kVersion) + L"-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(path_, error);
        if (error || IsReparsePoint(path_))
        {
            path_.clear();
            return;
        }
        ownerHandle_ = CreateFileW(
            (path_ / kRuntimeHookOwnerLockFilename).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (ownerHandle_ == INVALID_HANDLE_VALUE)
        {
            std::filesystem::remove_all(path_, error);
            path_.clear();
        }
    }

    ~InjectableRuntimeDirectory()
    {
        if (ownerHandle_ != INVALID_HANDLE_VALUE)
            CloseHandle(ownerHandle_);
        if (path_.empty())
            return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        error.clear();
        std::filesystem::remove(copiesRoot_, error);
        error.clear();
        std::filesystem::remove(copiesRoot_.parent_path(), error);
    }

    InjectableRuntimeDirectory(const InjectableRuntimeDirectory&) = delete;
    InjectableRuntimeDirectory& operator=(
        const InjectableRuntimeDirectory&) = delete;

    const std::filesystem::path& Path() const noexcept { return path_; }

private:
    std::filesystem::path copiesRoot_;
    std::filesystem::path path_;
    HANDLE ownerHandle_ = INVALID_HANDLE_VALUE;
};

std::wstring DeployInjectableRuntimeCopy(const wchar_t* filename)
{
    const std::filesystem::path source =
        snowdesktop::deployment::GetRuntimeFilePath(filename);
    if (!std::filesystem::is_regular_file(source))
        return source.wstring();

    static const InjectableRuntimeDirectory runtimeDirectory;
    const std::filesystem::path& targetDirectory = runtimeDirectory.Path();
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

bool IsStorePackageRegisteredForCurrentUser() noexcept
{
    UINT32 packageCount = 0;
    UINT32 bufferLength = 0;
    const LONG result = GetPackagesByPackageFamily(
        kPackageFamilyName, &packageCount, nullptr, &bufferLength, nullptr);
    return result == ERROR_INSUFFICIENT_BUFFER && packageCount > 0;
}

snowdesktop::deployment::PackagedAutoStartState
RunInstalledPackagedAutoStartOperationThroughActivation(
    PackagedStartupOperation operation) noexcept
{
    using snowdesktop::deployment::PackagedAutoStartState;
    const std::wstring token = CreatePackagedStartupQueryToken();
    if (token.empty())
        return PackagedAutoStartState::Unavailable;

    const std::wstring mappingName = PackagedStartupQueryObjectName(
        kPackagedStartupQueryMappingPrefix, token);
    const std::wstring eventName = PackagedStartupQueryObjectName(
        kPackagedStartupQueryEventPrefix, token);
    winrt::handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, sizeof(SharedPackagedStartupQueryState),
        mappingName.c_str())};
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
        return PackagedAutoStartState::Unavailable;
    auto* shared = static_cast<SharedPackagedStartupQueryState*>(
        MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0,
            sizeof(SharedPackagedStartupQueryState)));
    if (!shared)
        return PackagedAutoStartState::Unavailable;

    *shared = SharedPackagedStartupQueryState{};
    shared->ownerProcessId = GetCurrentProcessId();
    shared->operation = operation;
    winrt::handle completed{CreateEventW(
        nullptr, TRUE, FALSE, eventName.c_str())};
    if (!completed || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        UnmapViewOfFile(shared);
        return PackagedAutoStartState::Unavailable;
    }

    winrt::com_ptr<IApplicationActivationManager> activationManager;
    const HRESULT created = CoCreateInstance(
        CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(activationManager.put()));
    DWORD activatedProcessId = 0;
    const std::wstring applicationUserModelId =
        std::wstring(kPackageFamilyName) + L"!" + kPackageApplicationId;
    const std::wstring arguments =
        std::wstring(kPackagedStartupQueryArgumentPrefix) + token;
    const HRESULT activated = SUCCEEDED(created)
        ? activationManager->ActivateApplication(
            applicationUserModelId.c_str(), arguments.c_str(), AO_NONE,
            &activatedProcessId)
        : created;

    PackagedAutoStartState result = PackagedAutoStartState::Unavailable;
    if (SUCCEEDED(activated) && activatedProcessId != 0 &&
        WaitForSingleObject(completed.get(), 5000) == WAIT_OBJECT_0)
    {
        MemoryBarrier();
        if (shared->magic == kPackagedStartupQueryMagic &&
            shared->version == kPackagedStartupQueryVersion &&
            shared->size == sizeof(SharedPackagedStartupQueryState) &&
            shared->ownerProcessId == GetCurrentProcessId() &&
            shared->status == kPackagedStartupQueryCompleted)
        {
            result = shared->state;
        }
    }
    UnmapViewOfFile(shared);
    return result;
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

const RuntimeDeploymentContext& GetRuntimeDeploymentContext() noexcept
{
    static const RuntimeDeploymentContext context = [] {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size())
        {
            RuntimeDeploymentContext failed;
            failed.kind = RuntimeDeploymentKind::Invalid;
            failed.explicitContext = true;
            failed.error = "Current executable path is unavailable";
            return failed;
        }
        executable.resize(length);
        return ResolveRuntimeDeploymentContext(executable, IsPackaged());
    }();
    return context;
}

bool HasInvalidRuntimeDeploymentContext() noexcept
{
    return GetRuntimeDeploymentContext().kind ==
        RuntimeDeploymentKind::Invalid;
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

std::uint32_t DeleteUnvirtualizedCurrentUserValue(
    const wchar_t* subKey, const wchar_t* valueName) noexcept
{
    if (!subKey || !valueName)
        return ERROR_INVALID_PARAMETER;
    if (!IsPackaged())
        return DeleteCurrentUserValueDirect(subKey, valueName);
    return QueryCurrentUserValueThroughExplorer(subKey, valueName,
        taskbar_hook::RegistryOperation::DeleteValue).win32Result;
}

std::uint32_t SetUnvirtualizedCurrentUserValue(
    const wchar_t* subKey, const wchar_t* valueName, std::uint32_t type,
    const void* data, std::uint32_t size) noexcept
{
    if (!subKey || !valueName || (size != 0 && !data))
        return ERROR_INVALID_PARAMETER;
    if (!IsPackaged())
    {
        return SetCurrentUserValueDirect(
            subKey, valueName, type, data, size);
    }
    return QueryCurrentUserValueThroughExplorer(subKey, valueName,
        taskbar_hook::RegistryOperation::SetValue,
        type, data, size).win32Result;
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
    if (!IsInstalledPackageRegisteredForCurrentUser())
        return PackagedAutoStartState::Unavailable;
    return RunInstalledPackagedAutoStartOperationThroughActivation(
        PackagedStartupOperation::Query);
}

bool IsInstalledPackageRegisteredForCurrentUser() noexcept
{
    return IsStorePackageRegisteredForCurrentUser();
}

bool TryHandlePackagedAutoStartQueryCommand() noexcept
{
    const auto token = PackagedStartupQueryTokenFromCommandLine();
    if (!token)
        return false;
    if (!IsPackaged())
        return true;

    const std::wstring mappingName = PackagedStartupQueryObjectName(
        kPackagedStartupQueryMappingPrefix, *token);
    const std::wstring eventName = PackagedStartupQueryObjectName(
        kPackagedStartupQueryEventPrefix, *token);
    winrt::handle mapping{OpenFileMappingW(
        FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str())};
    winrt::handle completed{OpenEventW(
        EVENT_MODIFY_STATE, FALSE, eventName.c_str())};
    if (!mapping || !completed)
        return true;
    auto* shared = static_cast<SharedPackagedStartupQueryState*>(
        MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0,
            sizeof(SharedPackagedStartupQueryState)));
    if (!shared)
        return true;

    const std::wstring ownerPrefix =
        std::to_wstring(shared->ownerProcessId) + L".";
    if (shared->magic == kPackagedStartupQueryMagic &&
        shared->version == kPackagedStartupQueryVersion &&
        shared->size == sizeof(SharedPackagedStartupQueryState) &&
        shared->status == kPackagedStartupQueryPending &&
        token->starts_with(ownerPrefix))
    {
        switch (shared->operation)
        {
        case PackagedStartupOperation::Query:
            shared->state = GetPackagedAutoStartState();
            break;
        case PackagedStartupOperation::Enable:
            shared->state = SetPackagedAutoStartEnabled(true);
            break;
        case PackagedStartupOperation::Disable:
            shared->state = SetPackagedAutoStartEnabled(false);
            break;
        default:
            shared->state = PackagedAutoStartState::Unavailable;
            break;
        }
        MemoryBarrier();
        InterlockedExchange(
            &shared->status, kPackagedStartupQueryCompleted);
        SetEvent(completed.get());
    }
    UnmapViewOfFile(shared);
    return true;
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

PackagedAutoStartState SetInstalledPackagedAutoStartEnabled(
    bool enable) noexcept
{
    if (IsPackaged())
        return SetPackagedAutoStartEnabled(enable);
    if (!IsInstalledPackageRegisteredForCurrentUser())
        return PackagedAutoStartState::Unavailable;
    return RunInstalledPackagedAutoStartOperationThroughActivation(
        enable ? PackagedStartupOperation::Enable
               : PackagedStartupOperation::Disable);
}

bool IsPackagedAutoStartStateEnabled(
    PackagedAutoStartState state) noexcept
{
    return state == PackagedAutoStartState::Enabled ||
        state == PackagedAutoStartState::EnabledByPolicy;
}
}
