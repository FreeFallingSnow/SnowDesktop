/**
 * @file portable_data_migration.h
 * @brief Restart-safe portable data migration transaction.
 */

#pragma once

#include <filesystem>
#include <string>

namespace snowdesktop::migration
{
struct CopyResult
{
    bool ok = false;
    std::size_t files = 0;
    std::uintmax_t bytes = 0;
    std::string error;
};

struct ApplyResult
{
    bool ok = true;
    bool pending = false;
    bool applied = false;
    std::filesystem::path backup;
    std::string error;
};

/**
 * @brief Copy a portable data tree into a migration staging directory.
 * @details Uses Windows extended-length paths and rejects every reparse point.
 */
CopyResult CopyDataTree(const std::filesystem::path& source,
    const std::filesystem::path& destination);

/**
 * @brief Atomically publish a staged data directory for the next startup.
 */
bool Queue(const std::filesystem::path& stateRoot,
    const std::wstring& token, std::string& error);

/**
 * @brief Apply a queued data directory before the application opens data files.
 */
ApplyResult ApplyPending(const std::filesystem::path& stateRoot);
}
