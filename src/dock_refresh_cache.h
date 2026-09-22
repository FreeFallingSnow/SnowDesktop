#pragma once

#include <cstdint>
#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace snowdesktop::dock_refresh_cache
{
// UI-owned metadata: invalidating a Shell query must not unpin a running app
// or move a folder into the main section while its replacement is pending.
// The key includes the logical item and source path; the version is its stamp.
template<class Value>
class Cache
{
public:
    using Clock = std::chrono::steady_clock;
    struct Lookup
    {
        std::optional<Value> value;
        std::uint64_t ticket = 0;
        bool fresh = false;
        bool sameSourceVersion = false;
    };

    Lookup Read(const std::wstring& key, const std::wstring& version = {},
        Clock::time_point now = Clock::now())
    {
        auto& entry = entries_[key];
        if (entry.generation != generation_ || entry.requestVersion != version ||
            (entry.retryAt && now >= *entry.retryAt))
        {
            entry.generation = generation_;
            entry.requestVersion = version;
            entry.ticket = ++ticket_;
            entry.fresh = false;
            entry.retryAt.reset();
        }
        return {entry.value, entry.ticket, entry.fresh,
            entry.value && entry.valueVersion == version};
    }

    bool Publish(const std::wstring& key, std::uint64_t ticket, Value value)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.generation != generation_ ||
            found->second.ticket != ticket)
            return false;
        auto& entry = found->second;
        entry.value = std::move(value);
        entry.valueVersion = entry.requestVersion;
        entry.fresh = true;
        entry.retryAt.reset();
        return true;
    }

    // Failed Shell queries are not durable metadata. Keep a previously valid
    // icon while throttling retries, and reject failures from retired requests.
    bool PublishFailure(const std::wstring& key, std::uint64_t ticket,
        std::chrono::milliseconds delay, Clock::time_point now = Clock::now())
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.generation != generation_ ||
            found->second.ticket != ticket)
            return false;
        found->second.fresh = true;
        found->second.retryAt = now + delay;
        return true;
    }

    void Invalidate() { ++generation_; }

    template<class Predicate>
    void Retain(Predicate keep)
    {
        std::erase_if(entries_, [&](const auto& entry) { return !keep(entry.first); });
    }

private:
    struct Entry
    {
        std::optional<Value> value;
        std::wstring valueVersion;
        std::wstring requestVersion;
        std::uint64_t generation = 0;
        std::uint64_t ticket = 0;
        bool fresh = false;
        std::optional<Clock::time_point> retryAt;
    };
    std::unordered_map<std::wstring, Entry> entries_;
    std::uint64_t generation_ = 1;
    std::uint64_t ticket_ = 0;
};

inline std::wstring SourceKey(const std::wstring& reference, const std::wstring& path)
{
    return reference + L"\n" + path;
}
} // namespace snowdesktop::dock_refresh_cache
