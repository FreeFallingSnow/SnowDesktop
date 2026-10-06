/**
 * @file portable_data_migration.h
 * @brief Restart-safe portable data migration transaction.
 */

#pragma once

#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace snowdesktop
{
/**
 * @brief Optional cooperative cancellation for staged data operations.
 * @details beginNonInterruptible arbitrates the final publication boundary.
 * Returning true commits the operation and makes later cancellation advisory;
 * returning false leaves the staged result unpublished.
 */
struct CancellationContext
{
    std::stop_token stopToken;
    std::function<bool()> beginNonInterruptible;
};

namespace migration
{
struct CopyResult
{
    bool ok = false;
    bool cancelled = false;
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
    const std::filesystem::path& destination,
    const CancellationContext& cancellation = {});

/**
 * @brief Atomically publish a staged data directory for the next startup.
 */
bool Queue(const std::filesystem::path& stateRoot,
    const std::wstring& token, std::string& error,
    const CancellationContext& cancellation = {},
    bool* cancelled = nullptr);

/**
 * @brief Apply a queued data directory before the application opens data files.
 */
ApplyResult ApplyPending(const std::filesystem::path& stateRoot);
}
}
