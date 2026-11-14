#pragma once

#include "widget/packages/widget_package.h"

#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace snowdesktop::widget
{
// Background discovery reuses validation when the same NTFS file still has
// its validated change sequence. Size and timestamps alone cannot prove this.
// Each inspection holds a short read lease that excludes writers/replacement.
class SteamWorkshopValidationCache final
{
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
        const auto stamp = ReadStamp(lease.value, information);
        if (stamp)
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.stamp == stamp)
                return Reuse(found->second);
        }

        const std::string digest = WidgetPackageManager::Sha256File(archive);
        if (digest.empty()) return std::forward<Validator>(validator)();
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found != entries_.end() && found->second.result.digest == digest)
            {
                found->second.stamp = stamp;
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
        if (stable && WidgetPackageManager::Sha256File(archive) == digest)
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
            entries_[key] = Entry{stamp, result, ++sequence_};
        }
        return result;
    }

private:
    struct ArchiveStamp
    {
        DWORD volume;
        DWORD indexHigh;
        DWORD indexLow;
        DWORD size;
        LONGLONG creationTime;
        LONGLONG writeTime;
        LONGLONG changeTime;
        USN sequence;

        bool operator==(const ArchiveStamp&) const = default;
    };

    static std::optional<ArchiveStamp> ReadStamp(HANDLE file, const BY_HANDLE_FILE_INFORMATION& info)
    {
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)))
            return std::nullopt;

        // NTFS exposes its 64-bit file reference in a v2 record. Older
        // systems accept the same request without an input buffer. Unknown
        // record formats and an inactive/recreated journal fall back to hashing.
        struct
        {
            USN_RECORD_V2 record;
            WCHAR nameSpace[MAX_PATH];
        } buffer{};
        DWORD transferred = 0;
        if (!DeviceIoControl(file, FSCTL_READ_FILE_USN_DATA, nullptr, 0,
                &buffer, sizeof(buffer), &transferred, nullptr) ||
            transferred < offsetof(USN_RECORD_V2, FileName) ||
            buffer.record.MajorVersion != 2 || buffer.record.Usn <= 0)
            return std::nullopt;

        const auto reference = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        if (!reference || buffer.record.FileReferenceNumber != reference)
            return std::nullopt;
        return ArchiveStamp{info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
            info.nFileSizeLow, basic.CreationTime.QuadPart, basic.LastWriteTime.QuadPart,
            basic.ChangeTime.QuadPart, buffer.record.Usn};
    }

    struct Entry
    {
        std::optional<ArchiveStamp> stamp;
        Result result;
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
