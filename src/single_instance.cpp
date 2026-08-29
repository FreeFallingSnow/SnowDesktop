#include "single_instance.h"

#include "constants.h"
#include "steam_runtime_context.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <appmodel.h>
#include <shlobj.h>
#include <winver.h>

namespace snowdesktop::single_instance
{
namespace
{
std::wstring QueryProcessImagePath(HANDLE process)
{
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(
            process, 0, path.data(), &length))
    {
        return {};
    }
    path.resize(length);
    return path;
}

std::wstring ReadExecutableVersion(const std::wstring& path)
{
    if (path.empty())
        return {};

    DWORD ignored = 0;
    const DWORD size =
        GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size)
        return {};

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(
            path.c_str(), 0, size, data.data()))
    {
        return {};
    }

    VS_FIXEDFILEINFO* version = nullptr;
    UINT versionSize = 0;
    if (!VerQueryValueW(data.data(), L"\\",
            reinterpret_cast<void**>(&version), &versionSize) ||
        !version || versionSize < sizeof(VS_FIXEDFILEINFO) ||
        version->dwSignature != VS_FFI_SIGNATURE)
    {
        return {};
    }

    return std::to_wstring(HIWORD(version->dwFileVersionMS)) + L"." +
        std::to_wstring(LOWORD(version->dwFileVersionMS)) + L"." +
        std::to_wstring(HIWORD(version->dwFileVersionLS)) + L"." +
        std::to_wstring(LOWORD(version->dwFileVersionLS));
}

std::wstring QueryPackageFamilyName(HANDLE process)
{
    UINT32 length = 0;
    const LONG sizeResult =
        GetPackageFamilyName(process, &length, nullptr);
    if (sizeResult != ERROR_INSUFFICIENT_BUFFER || length <= 1)
        return {};

    std::wstring familyName(length, L'\0');
    if (GetPackageFamilyName(
            process, &length, familyName.data()) != ERROR_SUCCESS)
    {
        return {};
    }
    familyName.resize(length - 1);
    return familyName;
}

std::wstring BuildLegacyDataDirectory(
    const std::wstring& executablePath,
    const std::wstring& packageFamilyName)
{
    if (!packageFamilyName.empty())
    {
        PWSTR localAppData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(
                FOLDERID_LocalAppData, 0, nullptr, &localAppData)))
        {
            std::filesystem::path path(localAppData);
            CoTaskMemFree(localAppData);
            path /= L"Packages";
            path /= packageFamilyName;
            path /= L"LocalState";
            path /= L"data";
            return path.wstring();
        }
    }

    std::filesystem::path path(executablePath);
    if (!path.empty())
    {
        path = path.parent_path();
        path /= L"data";
        return path.wstring();
    }
    return {};
}

std::wstring NormalizePath(std::wstring_view value)
{
    if (value.empty())
        return {};

    std::wstring input(value);
    std::wstring result(32768, L'\0');
    const DWORD length = GetFullPathNameW(
        input.c_str(), static_cast<DWORD>(result.size()),
        result.data(), nullptr);
    if (length > 0 && length < result.size())
        result.resize(length);
    else
        result = std::move(input);

    std::replace(result.begin(), result.end(), L'/', L'\\');
    while (result.size() > 3 && result.back() == L'\\')
        result.pop_back();
    std::transform(result.begin(), result.end(), result.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(towlower(value));
        });
    return result;
}

bool ParseVersion(
    std::wstring_view text, std::array<unsigned long, 4>& parts)
{
    parts.fill(0);
    if (text.empty())
        return false;

    std::size_t begin = 0;
    std::size_t part = 0;
    while (begin < text.size() && part < parts.size())
    {
        const std::size_t end = text.find(L'.', begin);
        const std::size_t count =
            (end == std::wstring_view::npos ? text.size() : end) - begin;
        if (count == 0)
            return false;

        unsigned long long value = 0;
        for (std::size_t index = begin; index < begin + count; ++index)
        {
            if (text[index] < L'0' || text[index] > L'9')
                return false;
            value = value * 10 +
                static_cast<unsigned long long>(text[index] - L'0');
            if (value > std::numeric_limits<unsigned long>::max())
                return false;
        }
        parts[part++] = static_cast<unsigned long>(value);
        if (end == std::wstring_view::npos)
            return true;
        begin = end + 1;
        if (begin == text.size())
            return false;
    }
    return begin == text.size();
}

InstanceInfo DescribeProcess(
    HWND window, DWORD processId, std::wstring_view knownVersion)
{
    InstanceInfo info;
    info.controlWindow = window;
    info.processId = processId;

    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return info;

    info.executablePath = QueryProcessImagePath(process);
    const std::wstring packageFamilyName =
        QueryPackageFamilyName(process);
    info.packaged = !packageFamilyName.empty();
    info.dataDirectory = ResolveInstanceDataDirectory(
        info.executablePath, packageFamilyName);
    info.version = knownVersion.empty()
        ? ReadExecutableVersion(info.executablePath)
        : std::wstring(knownVersion);
    CloseHandle(process);
    return info;
}
}

std::wstring ResolveInstanceDataDirectory(
    std::wstring_view executablePath,
    std::wstring_view packageFamilyName)
{
    const std::wstring executable(executablePath);
    const std::wstring familyName(packageFamilyName);
    if (!executable.empty() || !familyName.empty())
    {
        const auto context =
            snowdesktop::deployment::ResolveRuntimeDeploymentContext(
                std::filesystem::path(executable), !familyName.empty());
        switch (context.kind)
        {
        case snowdesktop::deployment::RuntimeDeploymentKind::SteamManaged:
        case snowdesktop::deployment::RuntimeDeploymentKind::
            SteamLocalDevelopment:
            return context.dataRoot.wstring();
        case snowdesktop::deployment::RuntimeDeploymentKind::Invalid:
            return {};
        case snowdesktop::deployment::RuntimeDeploymentKind::Portable:
        case snowdesktop::deployment::RuntimeDeploymentKind::Packaged:
            break;
        }
    }
    return BuildLegacyDataDirectory(executable, familyName);
}

Guard::~Guard()
{
    if (mutex_)
        CloseHandle(mutex_);
}

AcquireResult Guard::Acquire(const wchar_t* mutexName)
{
    if (mutex_)
    {
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
    lastError_ = ERROR_SUCCESS;
    if (!mutexName || !*mutexName)
    {
        lastError_ = ERROR_INVALID_PARAMETER;
        return AcquireResult::Error;
    }

    SetLastError(ERROR_SUCCESS);
    mutex_ = CreateMutexW(nullptr, FALSE, mutexName);
    if (!mutex_)
    {
        lastError_ = GetLastError();
        return AcquireResult::Error;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mutex_);
        mutex_ = nullptr;
        return AcquireResult::Existing;
    }
    return AcquireResult::Primary;
}

DWORD ParseRestartPredecessorProcessId(std::wstring_view commandLine)
{
    constexpr std::wstring_view prefix = L"--wait-for-pid=";
    std::size_t position = commandLine.find(prefix);
    while (position != std::wstring_view::npos)
    {
        const bool startsToken = position == 0 ||
            commandLine[position - 1] == L' ' ||
            commandLine[position - 1] == L'\t';
        if (startsToken)
        {
            const std::size_t digitsBegin = position + prefix.size();
            std::size_t digitsEnd = digitsBegin;
            unsigned long long value = 0;
            while (digitsEnd < commandLine.size() &&
                commandLine[digitsEnd] >= L'0' &&
                commandLine[digitsEnd] <= L'9')
            {
                value = value * 10 +
                    static_cast<unsigned long long>(
                        commandLine[digitsEnd] - L'0');
                if (value > std::numeric_limits<DWORD>::max())
                    return 0;
                ++digitsEnd;
            }
            const bool endsToken = digitsEnd == commandLine.size() ||
                commandLine[digitsEnd] == L' ' ||
                commandLine[digitsEnd] == L'\t' ||
                commandLine[digitsEnd] == L'"';
            if (digitsEnd > digitsBegin && endsToken && value != 0)
                return static_cast<DWORD>(value);
        }
        position = commandLine.find(prefix, position + prefix.size());
    }
    return 0;
}

bool WaitForRestartPredecessor(
    DWORD processId, DWORD timeoutMilliseconds)
{
    if (!processId || processId == GetCurrentProcessId())
        return true;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER;
    const DWORD waitResult =
        WaitForSingleObject(process, timeoutMilliseconds);
    CloseHandle(process);
    return waitResult == WAIT_OBJECT_0;
}

InstanceInfo DescribeCurrentInstance(std::wstring_view version)
{
    return DescribeProcess(
        nullptr, GetCurrentProcessId(), version);
}

std::optional<InstanceInfo> FindExistingInstance(
    DWORD timeoutMilliseconds)
{
    const ULONGLONG started = GetTickCount64();
    while (true)
    {
        const HWND window = FindWindowW(
            kControlWindowClassName, L"SnowDesktopControl");
        if (window)
        {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId && processId != GetCurrentProcessId())
                return DescribeProcess(window, processId, {});
        }

        const ULONGLONG elapsed = GetTickCount64() - started;
        if (elapsed >= timeoutMilliseconds)
            return std::nullopt;
        Sleep(static_cast<DWORD>(std::min<ULONGLONG>(
            50, timeoutMilliseconds - elapsed)));
    }
}

bool VersionsMatch(
    std::wstring_view left, std::wstring_view right)
{
    std::array<unsigned long, 4> leftParts{};
    std::array<unsigned long, 4> rightParts{};
    if (!ParseVersion(left, leftParts) ||
        !ParseVersion(right, rightParts))
    {
        return left == right;
    }
    return leftParts == rightParts;
}

bool DataDirectoriesMatch(
    std::wstring_view left, std::wstring_view right)
{
    if (left.empty() || right.empty())
        return false;
    return NormalizePath(left) == NormalizePath(right);
}

bool NotifyExistingInstance(const InstanceInfo& instance)
{
    if (!instance.controlWindow ||
        !IsWindow(instance.controlWindow))
    {
        return false;
    }
    if (instance.processId &&
        instance.processId != GetCurrentProcessId())
    {
        AllowSetForegroundWindow(instance.processId);
    }
    return PostMessageW(instance.controlWindow,
        kActivateExistingInstanceMessage, 0, 0) != FALSE;
}

bool NotifyExistingInstance(DWORD timeoutMilliseconds)
{
    const auto instance =
        FindExistingInstance(timeoutMilliseconds);
    return instance && NotifyExistingInstance(*instance);
}

bool RequestExistingInstanceExit(
    const InstanceInfo& instance, DWORD timeoutMilliseconds)
{
    if (!instance.processId ||
        instance.processId == GetCurrentProcessId())
    {
        return false;
    }

    HANDLE process = OpenProcess(
        SYNCHRONIZE, FALSE, instance.processId);
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER;

    if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0)
    {
        CloseHandle(process);
        return true;
    }

    DWORD_PTR ignored = 0;
    const BOOL delivered = instance.controlWindow &&
        IsWindow(instance.controlWindow) &&
        SendMessageTimeoutW(instance.controlWindow, WM_CLOSE, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &ignored);
    if (!delivered)
    {
        CloseHandle(process);
        return false;
    }

    const DWORD result =
        WaitForSingleObject(process, timeoutMilliseconds);
    CloseHandle(process);
    return result == WAIT_OBJECT_0;
}
}
