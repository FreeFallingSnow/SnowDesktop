#pragma once

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace snowdesktop::icon_customization
{
inline constexpr std::int64_t kRetentionSeconds = 24 * 60 * 60;
using MissingTargets = std::map<std::wstring, std::int64_t>;
enum class Presence { Unknown, Present, Missing };
struct Observation
{
    std::wstring target;
    Presence object = Presence::Unknown, asset = Presence::Unknown;
};

inline std::int64_t NowSeconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// Run on a worker. Access failures and disconnected drives are not deletions.
// A missing parent on accessible local storage still confirms a removed tree.
inline Presence Probe(const std::filesystem::path& path)
{
    const auto root = path.root_path();
    const auto drive = GetDriveTypeW(root.c_str());
    if (!path.is_absolute() || (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK) ||
        GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) return Presence::Unknown;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) return Presence::Present;
    const DWORD error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return Presence::Unknown;
    return Presence::Missing;
}

inline bool Visible(const std::wstring& target, const MissingTargets& missing)
{
    return !missing.contains(target);
}

inline bool Expired(std::int64_t since, std::int64_t now)
{
    return since <= 0 || (now >= since && now - since >= kRetentionSeconds);
}

template<class Replacements, class Appearances>
bool PruneExpired(Replacements& replacements, Appearances& appearances,
    MissingTargets& missing, std::int64_t now)
{
    bool changed = false;
    for (auto it = missing.begin(); it != missing.end();)
    {
        if (!replacements.contains(it->first) && !appearances.contains(it->first))
        { it = missing.erase(it); changed = true; continue; }
        if (Expired(it->second, now))
        {
            replacements.erase(it->first); appearances.erase(it->first);
            it = missing.erase(it); changed = true; continue;
        }
        if (it->second > now) { it->second = now; changed = true; }
        ++it;
    }
    return changed;
}

// Shared host transaction: expire before recovering, preserve the original
// deadline across scans/restarts, and never reactivate a missing replacement.
template<class Replacements, class Appearances>
bool Reconcile(Replacements& replacements, Appearances& appearances,
    MissingTargets& missing, const std::vector<Observation>& observations, std::int64_t now)
{
    bool changed = PruneExpired(replacements, appearances, missing, now);
    for (const auto& observation : observations)
    {
        const auto& target = observation.target;
        if (!replacements.contains(target) && !appearances.contains(target)) continue;
        if (observation.object == Presence::Missing)
            changed = missing.try_emplace(target, now).second || changed;
        else if (observation.object == Presence::Present)
        {
            if (replacements.contains(target))
            {
                if (observation.asset == Presence::Unknown) continue;
                if (observation.asset == Presence::Missing)
                { replacements.erase(target); changed = true; }
            }
            changed = missing.erase(target) != 0 || changed;
        }
    }
    return changed;
}
}
