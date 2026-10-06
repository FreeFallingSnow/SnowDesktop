#pragma once
#include <windows.h>
#include "../steam_bridge/src/theme_workshop_publish.h"
#include <chrono>
#include <map>
#include <mutex>
#include <optional>

namespace snowdesktop::themes::workshop::detail
{
struct BridgeAvailability
{
    bool workshop = false;
    bool sharing = false;
};

struct BridgeFileIdentity
{
    DWORD volume = 0, indexHigh = 0, indexLow = 0, sizeHigh = 0, sizeLow = 0;
    LONGLONG written = 0, changed = 0;
    bool operator==(const BridgeFileIdentity&) const = default;
};

inline std::optional<BridgeFileIdentity> ReadBridgeFileIdentity(const std::filesystem::path& path)
{
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_BASIC_INFO basic{};
    const bool read = GetFileInformationByHandle(file, &information) &&
        GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic));
    CloseHandle(file);
    if (!read || (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        (information.nFileIndexHigh == 0 && information.nFileIndexLow == 0)) return {};
    return BridgeFileIdentity{information.dwVolumeSerialNumber, information.nFileIndexHigh,
        information.nFileIndexLow, information.nFileSizeHigh, information.nFileSizeLow,
        basic.LastWriteTime.QuadPart, basic.ChangeTime.QuadPart};
}

// Only immutable protocol/compiled capabilities are cached. Steam sessions,
// ownership and advanced-feature entitlement always retain their live checks.
class BridgeAvailabilityCache
{
public:
    using Clock = std::chrono::steady_clock;
    using IdentityReader = std::optional<BridgeFileIdentity> (*)(const std::filesystem::path&);

    template<class Probe, class ReadIdentity = IdentityReader>
    BridgeAvailability Query(const std::filesystem::path& path, std::string_view hostVersion,
        Clock::time_point now, Probe&& probe, ReadIdentity readIdentity = ReadBridgeFileIdentity)
    {
        const auto started = Clock::now();
        std::lock_guard lock(mutex_);
        const auto observedNow = now + (Clock::now() - started);
        const Key key{path.lexically_normal().wstring(), std::string(hostVersion)};
        if (!steam_bridge::ThemeSafePath(path)) { entries_.erase(key); return {}; }
        const auto identity = readIdentity(path);
        // Some removable/network filesystems cannot provide the strong stamp.
        // Preserve the original probe instead of treating that as incompatibility.
        if (!identity)
        {
            entries_.erase(key);
            const auto result = probe();
            return steam_bridge::ThemeSafePath(path) ? result : BridgeAvailability{};
        }
        if (const auto found = entries_.find(key); found != entries_.end() &&
            found->second.identity == *identity && observedNow < found->second.expires)
            return found->second.result;
        const auto result = probe();
        const auto after = readIdentity(path);
        if (!steam_bridge::ThemeSafePath(path) || !after || *after != *identity)
        { entries_.erase(key); return {}; }
        if (!entries_.contains(key) && entries_.size() >= 8) entries_.clear();
        // Count the lifetime from completion, including a timed-out probe, so
        // one snapshot cannot retry the same failure for every theme selector.
        const auto lifetime = result.workshop ? std::chrono::seconds(30) : std::chrono::seconds(1);
        entries_[key] = {*identity, result, now + (Clock::now() - started) + lifetime};
        return result;
    }

private:
    using Key = std::pair<std::wstring, std::string>;
    struct Entry { BridgeFileIdentity identity; BridgeAvailability result; Clock::time_point expires; };
    std::mutex mutex_;
    std::map<Key, Entry> entries_;
};
}
