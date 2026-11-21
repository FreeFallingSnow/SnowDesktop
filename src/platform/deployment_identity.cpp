#include "deployment_identity.h"

#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>

#include <filesystem>

namespace
{
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
}
