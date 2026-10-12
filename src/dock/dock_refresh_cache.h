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
template<class Value, class Key = std::wstring>
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

    Lookup Read(const Key& key, const std::wstring& version = {},
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

    bool Publish(const Key& key, std::uint64_t ticket, Value value, bool replaceCurrent = true)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.generation != generation_ ||
            found->second.ticket != ticket)
            return false;
        auto& entry = found->second;
        // Fast local pixels only fill an empty cache. A refresh keeps the last
        // visible icon across tickets until the complete Shell result arrives.
        if (!replaceCurrent && entry.value) return false;
        entry.value = std::move(value);
        entry.valueVersion = entry.requestVersion;
        entry.fresh = true;
        // A provisional local result must retain a failed refinement's retry.
        if (replaceCurrent) entry.retryAt.reset();
        return true;
    }

    // Failed Shell queries are not durable metadata. Keep a previously valid
    // icon while throttling retries, and reject failures from retired requests.
    bool PublishFailure(const Key& key, std::uint64_t ticket,
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

    // A value can remain visible while a bounded-age identity revalidation is
    // pending. Publishing still rejects obsolete tickets and source versions.
    template<class Submit>
    Lookup ReadOrSubmit(const Key& key, const std::wstring& version, Submit submit,
        Clock::time_point now = Clock::now())
    {
        const auto lookup = Read(key, version, now);
        if (!lookup.fresh) submit(lookup.ticket);
        return lookup;
    }

    bool PublishWithLifetime(const Key& key, std::uint64_t ticket, Value value,
        std::chrono::milliseconds lifetime, Clock::time_point now = Clock::now())
    {
        if (!Publish(key, ticket, std::move(value))) return false;
        entries_.at(key).retryAt = now + lifetime;
        return true;
    }

    std::optional<bool> PublishWithLifetimeChanged(const Key& key,
        std::uint64_t ticket, Value value, std::chrono::milliseconds lifetime,
        Clock::time_point now = Clock::now())
    {
        const auto found = entries_.find(key);
        if (found == entries_.end()) return std::nullopt;
        const auto previous = PeekValue(key, found->second.requestVersion);
        const bool changed = previous.value_or(Value{}) != value;
        if (!PublishWithLifetime(key, ticket, std::move(value), lifetime, now))
            return std::nullopt;
        return changed;
    }

    std::optional<Value> PeekValue(const Key& key,
        const std::wstring& version = {}) const
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.valueVersion != version)
            return std::nullopt;
        return found->second.value;
    }

    void Invalidate() { ++generation_; }

    // Window notifications invalidate only that window. Preserve the visible
    // value and reject any completion belonging to the retired request.
    void Invalidate(const Key& key)
    {
        const auto found = entries_.find(key);
        if (found != entries_.end()) found->second.generation = 0;
    }

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
    std::unordered_map<Key, Entry> entries_;
    std::uint64_t generation_ = 1;
    std::uint64_t ticket_ = 0;
};

inline std::wstring SourceKey(const std::wstring& reference, const std::wstring& path)
{
    return reference + L"\n" + path;
}
} // namespace snowdesktop::dock_refresh_cache
