#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace snowdesktop::widget_surface_retention
{
// A brief page visit can reuse its surfaces. Long-hidden pages retain only
// runtime state; this budget counts BGRA pixels, not driver allocations.
inline constexpr std::uint64_t kRetentionMilliseconds = 10'000;
inline constexpr std::uint64_t kHiddenByteBudget = 16 * 1024 * 1024;

struct Candidate
{
    bool visible = false;
    std::uint64_t hiddenSince = 0;
    std::uint64_t bytes = 0;
};

enum class Reason { Expired, Budget };
struct Release
{
    std::size_t index = 0;
    Reason reason = Reason::Expired;
};

inline std::vector<Release> SelectReleases(
    std::span<const Candidate> candidates, std::uint64_t now,
    std::uint64_t retention = kRetentionMilliseconds,
    std::uint64_t budget = kHiddenByteBudget)
{
    std::vector<std::size_t> hidden;
    std::uint64_t retained = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        if (candidates[i].visible || candidates[i].bytes == 0)
            continue;
        retained += candidates[i].bytes;
        hidden.push_back(i);
    }
    std::stable_sort(hidden.begin(), hidden.end(), [&](auto a, auto b) {
        return candidates[a].hiddenSince < candidates[b].hiddenSince;
    });
    std::vector<Release> releases;
    for (const auto i : hidden)
    {
        const auto& candidate = candidates[i];
        const bool expired = now >= candidate.hiddenSince &&
            now - candidate.hiddenSince >= retention;
        if (!expired && retained <= budget)
            continue;
        releases.push_back({i, expired ? Reason::Expired : Reason::Budget});
        retained -= candidate.bytes;
    }
    return releases;
}
}
