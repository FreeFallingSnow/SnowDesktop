/**
 * @file full_data_backup.h
 * @brief Complete SnowDesktop data backups and .snowbackup archives.
 */

#pragma once

#include "portable_data_migration.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::backup
{
inline constexpr std::uint64_t kMaxArchiveBytes =
    2ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxExtractedBytes =
    4ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kMaxFileBytes =
    1ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::size_t kMaxFiles = 100000;

namespace detail
{
// Stored ZIP32 archives contain an EOCD plus a local header and central
// directory entry for each file. Include both copies of each UTF-8 name.
inline bool AppendStoreZipSize(std::uint64_t& bytes,
    std::uint64_t payloadBytes, std::size_t nameBytes) noexcept
{
    if (nameBytes > 65535 || payloadBytes > kMaxArchiveBytes ||
        bytes > kMaxArchiveBytes) return false;
    const std::uint64_t overhead = 76 + 2 * nameBytes;
    if (overhead > kMaxArchiveBytes - bytes ||
        payloadBytes > kMaxArchiveBytes - bytes - overhead) return false;
    bytes += overhead + payloadBytes;
    return true;
}
}

struct BackupInfo
{
    std::wstring id;
    std::filesystem::path root;
    std::filesystem::path data;
    std::string createdAt;
    std::string hostVersion;
    std::string sourceType;
    std::size_t fileCount = 0;
    std::uint64_t totalBytes = 0;
    bool migrationRollback = false;
};

struct OperationResult
{
    bool ok = false;
    bool cancelled = false;
    BackupInfo backup;
    std::string error;
};

class FullDataBackupManager
{
public:
    FullDataBackupManager(std::filesystem::path stateRoot,
        std::filesystem::path activeData, std::string hostVersion,
        std::string sourceType);

    const std::filesystem::path& BackupRoot() const
    {
        return backupRoot_;
    }

    std::vector<BackupInfo> List() const;
    OperationResult Create(
        const CancellationContext& cancellation = {});
    OperationResult Export(const BackupInfo& backup,
        const std::filesystem::path& archive,
        const CancellationContext& cancellation = {}) const;
    OperationResult QueueRestore(const BackupInfo& backup,
        const CancellationContext& cancellation = {}) const;
    OperationResult ImportAndQueue(
        const std::filesystem::path& archive,
        const CancellationContext& cancellation = {}) const;
    OperationResult QueueDirectory(
        const std::filesystem::path& sourceData,
        const CancellationContext& cancellation = {}) const;
    OperationResult Delete(const BackupInfo& backup,
        const CancellationContext& cancellation = {}) const;

private:
    std::filesystem::path stateRoot_;
    std::filesystem::path activeData_;
    std::filesystem::path backupRoot_;
    std::string hostVersion_;
    std::string sourceType_;
};
}
