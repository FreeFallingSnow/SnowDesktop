#pragma once

#include "widget/packages/widget_package.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace snowdesktop::widget
{
// Background discovery may reuse successful validation of the exact bytes.
// Metadata alone is insufficient: an archive can change without changing its
// size or timestamp. The reader also prevents writers/replacement while the
// digest and validator inspect it. Explicit online queries still validate fully.
class SteamWorkshopValidationCache final
{
public:
    struct Result
    {
        ValidationReport report;
        PackageManifest manifest;
        bool reused = false;
    };

    explicit SteamWorkshopValidationCache(std::size_t capacity = 256)
        : capacity_((std::max)(std::size_t{1}, capacity)) {}

    static SteamWorkshopValidationCache& ForProcess()
    {
        // Independently owned discovery jobs can finish during CRT teardown.
        static auto* cache = new SteamWorkshopValidationCache;
        return *cache;
    }

    template<class Validator>
    Result Validate(const std::filesystem::path& archive,
        const std::filesystem::path& validationRoot, Validator&& validator)
    {
        struct ReadLease
        {
            HANDLE value = INVALID_HANDLE_VALUE;
            ~ReadLease()
            {
                if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            }
        } lease{CreateFileW(archive.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        BY_HANDLE_FILE_INFORMATION information{};
        if (lease.value == INVALID_HANDLE_VALUE ||
            GetFileType(lease.value) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(lease.value, &information) ||
            (information.dwFileAttributes &
                (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
            information.nFileSizeHigh != 0 ||
            information.nFileSizeLow > kMaxArchiveBytes)
            return std::forward<Validator>(validator)();

        const std::string digest = WidgetPackageManager::Sha256File(archive);
        if (digest.empty()) return std::forward<Validator>(validator)();
        const auto key = std::make_pair(archive.lexically_normal().native(),
            validationRoot.lexically_normal().native());
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.digest == digest)
            {
                found->second.used = ++sequence_;
                Result result = found->second.result;
                result.reused = true;
                return result;
            }
        }

        Result result = std::forward<Validator>(validator)();
        // Never cache errors, incomplete reads, or a result whose path resolved
        // to different bytes during validation (for example a parent rename).
        if (result.report.Ok() &&
            WidgetPackageManager::Sha256File(archive) == digest)
        {
            std::lock_guard lock(mutex_);
            if (!entries_.contains(key) && entries_.size() >= capacity_)
            {
                const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                    [](const auto& left, const auto& right) {
                        return left.second.used < right.second.used;
                    });
                entries_.erase(oldest);
            }
            entries_[key] = Entry{digest, result, ++sequence_};
        }
        return result;
    }

private:
    struct Entry
    {
        std::string digest;
        Result result;
        std::uint64_t used = 0;
    };
    const std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::pair<std::wstring, std::wstring>, Entry> entries_;
    std::uint64_t sequence_ = 0;
};
}
