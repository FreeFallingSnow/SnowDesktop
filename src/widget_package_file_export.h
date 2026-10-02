#pragma once

#include "widget_package.h"
#include <windows.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace snowdesktop::widget::detail
{
struct PackageFileExportResult
{
    bool succeeded = false;
    ValidationReport report;
    std::string error;
};

// Read-only package IO used by the settings process worker. Never initializes
// the package registry, copies user storage, or changes the active source.
inline PackageFileExportResult ExportDevelopmentPackageFile(
    const std::filesystem::path& source,
    const std::filesystem::path& output,
    std::string_view expectedId, std::string_view expectedVersion)
{
    PackageFileExportResult result;
    const auto extension = output.extension().wstring();
    if (CompareStringOrdinal(extension.c_str(), -1, L".snowwidget", -1,
            TRUE) != CSTR_EQUAL)
    {
        result.error = "output must use the .snowwidget extension";
        return result;
    }
    struct DirectoryLock
    {
        HANDLE value = INVALID_HANDLE_VALUE;
        ~DirectoryLock() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } lock;
    lock.value = CreateFileW(source.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (lock.value == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandleEx(lock.value, FileAttributeTagInfo,
            &info, sizeof(info)) || !(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
    {
        result.error = "development component directory is unsafe or unavailable";
        return result;
    }

    const std::string uuid = WidgetPackageManager::GenerateUuid();
    const auto temporaryDirectory = output.parent_path() /
        (L".snowwidget-export-" + std::wstring(uuid.begin(), uuid.end()));
    std::error_code error;
    if (!std::filesystem::create_directory(temporaryDirectory, error))
    {
        result.error = error ? error.message() : "temporary export directory already exists";
        return result;
    }
    struct TemporaryExport
    {
        std::filesystem::path directory;
        std::filesystem::path archive;
        ~TemporaryExport()
        {
            std::error_code ignored;
            std::filesystem::remove(archive, ignored);
            std::filesystem::remove(archive.wstring() + L".tmp", ignored);
            std::filesystem::remove(directory, ignored);
        }
    } temporary{temporaryDirectory, temporaryDirectory / L"package.snowwidget"};
    WidgetPackageManager manager(PackagePaths{});
    PackageArtifact artifact;
    if (!manager.ExportDirectory(source, temporary.archive, artifact,
            result.report, result.error))
        return result;

    PackageManifest manifest;
    result.report = manager.ValidateArchive(temporary.archive, &manifest);
    if (!result.report.Ok() || manifest.id != expectedId ||
        manifest.version != expectedVersion || artifact.packageId != expectedId ||
        artifact.version != expectedVersion || artifact.sha256.empty() ||
        artifact.sha256 != WidgetPackageManager::Sha256File(temporary.archive))
    {
        result.error = "exported package does not match the selected development component";
        return result;
    }
    // Only publish a completely validated file. A failed export or rename
    // leaves any existing destination intact; both paths are on one volume.
    if (!MoveFileExW(temporary.archive.c_str(), output.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        result.error = std::error_code(static_cast<int>(GetLastError()),
            std::system_category()).message();
        return result;
    }
    result.succeeded = true;
    return result;
}
} // namespace snowdesktop::widget::detail
