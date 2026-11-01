// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "manager_workshop_item.h"
#include "package_tool.h"
#include "workshop_staging.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace snowdesktop::steam_bridge
{
namespace manager_package_export_detail
{
struct FileHandle
{
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    HANDLE value = INVALID_HANDLE_VALUE;
};

inline bool OpenRegularRead(const std::filesystem::path& path,
    FileHandle& file, std::string& error)
{
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    if (file.value == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(file.value, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
    {
        error = "Package export file is unavailable, linked, or unsafe";
        return false;
    }
    return true;
}

inline bool ExistingMatches(const std::filesystem::path& source,
    const std::filesystem::path& destination, std::string& error)
{
    FileHandle destinationLock;
    if (!OpenRegularRead(destination, destinationLock, error)) return false;
    std::ifstream first(source, std::ios::binary);
    std::ifstream second(destination, std::ios::binary);
    if (!first || !second)
    {
        error = "Cannot compare the existing package export";
        return false;
    }
    std::array<char, 64 * 1024> left{}, right{};
    do
    {
        first.read(left.data(), static_cast<std::streamsize>(left.size()));
        second.read(right.data(), static_cast<std::streamsize>(right.size()));
        if (first.bad() || second.bad())
        {
            error = "Cannot read the existing package export";
            return false;
        }
        if (first.gcount() != second.gcount() ||
            !std::equal(left.begin(), left.begin() + first.gcount(), right.begin()))
        {
            error = "An existing package export has different bytes; it was not replaced";
            return false;
        }
    } while (!first.eof() && !second.eof());
    return true;
}

struct TemporaryExport
{
    std::filesystem::path directory;
    std::filesystem::path package;
    ~TemporaryExport()
    {
        if (directory.empty()) return;
        std::error_code ignored;
        std::filesystem::remove(package, ignored);
        ignored.clear();
        std::filesystem::remove(directory, ignored);
    }
};
}

// Export a validated PackageTool artifact without transferring its temporary
// ownership. Existing files are never replaced; matching bytes are reusable.
inline bool ExportManagerPackage(const PackagedWidget& artifact,
    const std::filesystem::path& exportRoot,
    std::filesystem::path& exported, std::string& error)
{
    using namespace manager_package_export_detail;
    exported.clear();
    error.clear();
    if (!ManagerWorkshopUuid(artifact.packageId) ||
        !ManagerWorkshopHex(artifact.sha256, 64))
    {
        error = "Package export requires a valid component UUID and full SHA-256";
        return false;
    }
    FileHandle sourceLock;
    if (!OpenRegularRead(artifact.packagePath, sourceLock, error)) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(sourceLock.value, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 20ll * 1024 * 1024)
    {
        error = "The package export source is empty, unreadable, or exceeds 20 MiB";
        return false;
    }
    if (!PrepareWorkshopStaging(exportRoot, error)) return false;
    const std::string name = artifact.packageId + "-" + artifact.sha256 + ".snowwidget";
    const auto destination = exportRoot / std::wstring(name.begin(), name.end());
    const DWORD existing = GetFileAttributesW(destination.c_str());
    if (existing != INVALID_FILE_ATTRIBUTES)
    {
        if (!ExistingMatches(artifact.packagePath, destination, error)) return false;
        exported = destination;
        return true;
    }
    const DWORD lookupError = GetLastError();
    if (lookupError != ERROR_FILE_NOT_FOUND && lookupError != ERROR_PATH_NOT_FOUND)
    {
        error = "Cannot inspect the package export destination";
        return false;
    }

    TemporaryExport temporary;
    static std::atomic_uint64_t sequence = 0;
    std::error_code ec;
    for (unsigned attempt = 0; attempt != 32; ++attempt)
    {
        const auto candidate = exportRoot / (L".package-export-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) +
            L"-" + std::to_wstring(sequence.fetch_add(1)));
        if (std::filesystem::create_directory(candidate, ec))
        {
            temporary.directory = candidate;
            temporary.package = candidate / L"package.snowwidget";
            break;
        }
        if (ec && ec != std::errc::file_exists) break;
        ec.clear();
    }
    if (temporary.directory.empty() ||
        !std::filesystem::copy_file(artifact.packagePath, temporary.package,
            std::filesystem::copy_options::none, ec))
    {
        error = "Cannot copy the package into its private export directory";
        return false;
    }
    {
        FileHandle temporaryFile;
        temporaryFile.value = CreateFileW(temporary.package.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (temporaryFile.value == INVALID_HANDLE_VALUE ||
            !FlushFileBuffers(temporaryFile.value))
        {
            error = "Cannot flush the package export";
            return false;
        }
    }
    if (!MoveFileExW(temporary.package.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
    {
        // Another exporter may have published the same name after our first
        // lookup. Reuse only a newly inspected regular file with equal bytes.
        if (GetFileAttributesW(destination.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            error = "Cannot publish the package export without replacing an existing file";
            return false;
        }
        if (!ExistingMatches(artifact.packagePath, destination, error)) return false;
    }
    exported = destination;
    return true;
}
}
