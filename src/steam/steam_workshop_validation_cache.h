#pragma once

#include "widget/packages/widget_package.h"

#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace snowdesktop::widget
{
// Background discovery can reuse the digest and validation while a read oplock
// still covers the same file. Otherwise it hashes the archive under a short
// lease that excludes writers. Explicit online queries still validate fully.
class SteamWorkshopValidationCache final
{
    struct ArchiveWatch
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        OVERLAPPED operation{};
        REQUEST_OPLOCK_OUTPUT_BUFFER output{};
        FILE_ID_INFO identity{};
        bool pending = false;

        ArchiveWatch() = default;
        ArchiveWatch(const ArchiveWatch&) = delete;
        ArchiveWatch& operator=(const ArchiveWatch&) = delete;
        ~ArchiveWatch()
        {
            if (pending)
            {
                CancelIoEx(file, &operation);
                DWORD transferred = 0;
                GetOverlappedResult(file, &operation, &transferred, TRUE);
            }
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (operation.hEvent) CloseHandle(operation.hEvent);
        }

        static std::unique_ptr<ArchiveWatch> Start(HANDLE lease, const FILE_ID_INFO& identity)
        {
            auto watch = std::make_unique<ArchiveWatch>();
            // Keep this handle shareable even for writers and replacements.
            // An R oplock breaks without an acknowledgement or waiting for this cache.
            watch->file = ReOpenFile(lease, GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
            if (watch->file == INVALID_HANDLE_VALUE) return {};
            watch->operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!watch->operation.hEvent) return {};
            REQUEST_OPLOCK_INPUT_BUFFER input{};
            input.StructureVersion = REQUEST_OPLOCK_CURRENT_VERSION;
            input.StructureLength = sizeof(input);
            input.RequestedOplockLevel = OPLOCK_LEVEL_CACHE_READ;
            input.Flags = REQUEST_OPLOCK_INPUT_FLAG_REQUEST;
            DWORD transferred = 0;
            if (DeviceIoControl(watch->file, FSCTL_REQUEST_OPLOCK, &input, sizeof(input),
                &watch->output, sizeof(watch->output), &transferred, &watch->operation) ||
                GetLastError() != ERROR_IO_PENDING)
                return {};
            watch->pending = true;
            watch->identity = identity;
            return watch;
        }

        bool Covers(const FILE_ID_INFO& current) const noexcept
        {
            return identity.VolumeSerialNumber == current.VolumeSerialNumber &&
                std::memcmp(identity.FileId.Identifier, current.FileId.Identifier,
                    sizeof(identity.FileId.Identifier)) == 0 &&
                WaitForSingleObject(operation.hEvent, 0) == WAIT_TIMEOUT;
        }
    };

public:
    struct Result
    {
        ValidationReport report;
        PackageManifest manifest;
        bool reused = false;
        std::string digest;
        bool malformedArchive = false;
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

        const auto key = std::make_pair(archive.lexically_normal().native(),
            validationRoot.lexically_normal().native());
        FILE_ID_INFO identity{};
        const bool canWatch = GetFileInformationByHandleEx(lease.value, FileIdInfo, &identity, sizeof(identity)) &&
            std::any_of(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
                [](BYTE byte) { return byte != 0; });
        if (canWatch)
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.watch && found->second.watch->Covers(identity))
                return Reuse(found->second);
        }

        auto watch = canWatch ? ArchiveWatch::Start(lease.value, identity) : nullptr;
        const std::string digest = WidgetPackageManager::Sha256File(archive);
        if (digest.empty()) return std::forward<Validator>(validator)();
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.digest == digest)
            {
                if (watch && watch->Covers(identity)) found->second.watch = std::move(watch);
                return Reuse(found->second);
            }
        }

        Result result = std::forward<Validator>(validator)();
        // Only reuse errors derived from the bytes. Reads and staging can
        // fail transiently with the very same archive.
        const bool stable = result.malformedArchive || std::all_of(result.report.issues.begin(),
            result.report.issues.end(), [](const auto& issue) {
                return issue.severity != ValidationSeverity::Error ||
                    (issue.code.starts_with("manifest.") && issue.code != "manifest.read");
            });
        if (stable &&
            WidgetPackageManager::Sha256File(archive) == digest)
        {
            result.digest = digest;
            std::lock_guard lock(mutex_);
            if (!entries_.contains(key) && entries_.size() >= capacity_)
            {
                const auto oldest = std::min_element(entries_.begin(), entries_.end(),
                    [](const auto& left, const auto& right) {
                        return left.second.used < right.second.used;
                    });
                entries_.erase(oldest);
            }
            if (watch && !watch->Covers(identity)) watch.reset();
            entries_[key] = Entry{digest, result, std::move(watch), ++sequence_};
        }
        return result;
    }

private:
    struct Entry
    {
        std::string digest;
        Result result;
        std::unique_ptr<ArchiveWatch> watch;
        std::uint64_t used = 0;
    };
    Result Reuse(Entry& entry)
    {
        entry.used = ++sequence_;
        Result result = entry.result;
        result.reused = true;
        return result;
    }
    const std::size_t capacity_;
    std::mutex mutex_;
    std::map<std::pair<std::wstring, std::wstring>, Entry> entries_;
    std::uint64_t sequence_ = 0;
};
}
