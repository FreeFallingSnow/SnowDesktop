#pragma once
#include <cstdint>
#include <optional>

namespace snowdesktop::status_bar_notification
{
// Internal status-bar sampling only. Empty means unknown/unavailable, never
// false or zero. Windows 11's notification-center total is not an unread count.
struct Snapshot
{
    std::optional<bool> quiet;
    std::optional<std::uint32_t> unreadCount;
    std::optional<std::uint32_t> totalCount;
};

// Process-wide five-second cache, shared by all bars/monitors. It performs only
// two fixed-size native queries, with no COM activation, worker or subscription,
// and never asks for notification access or retrieves notification contents.
Snapshot Current();

namespace detail
{
struct WordReply
{
    std::int32_t status = -1;
    std::uint32_t bytes = 0;
    std::uint32_t value = 0;
};

// The private payload is accepted only at its known exact size. An unsupported
// Windows family, empty state, failed query or changed schema stays unknown.
inline Snapshot Decode(std::uint32_t major, std::uint32_t build,
    const WordReply& quiet, const WordReply& count)
{
    Snapshot result;
    if (major != 10 || build < 10240) return result;
    if (quiet.status == 0 && quiet.bytes == sizeof(std::uint32_t) && quiet.value <= 2)
        result.quiet = quiet.value != 0;
    if (count.status == 0 && count.bytes == sizeof(std::uint32_t))
    {
        if (build >= 22000) result.totalCount = count.value;
        else result.unreadCount = count.value;
    }
    return result;
}

// A caller holds the lock. Kept independent of Windows so the production cache
// can be tested with a controlled clock and query failure without reading the
// test machine's notification state.
class Cache
{
public:
    template<class Read>
    Snapshot Get(std::uint64_t now, Read&& read)
    {
        if (!sampled_ || now - sampledAt_ >= 5000)
        {
            value_ = read(); // Failure replaces old values; it never reports stale success.
            sampledAt_ = now;
            sampled_ = true;
        }
        return value_;
    }
private:
    Snapshot value_;
    std::uint64_t sampledAt_ = 0;
    bool sampled_ = false;
};
}
}
