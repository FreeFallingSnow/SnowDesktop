#pragma once
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdint>
#include <vector>

namespace snowdesktop::widget_runtime
{
// The PDH API explicitly says the size returned for an undersized non-null
// buffer is unreliable. Probe with null again, then retry a bounded number of
// times. Retain allocation across samples without retaining stale item counts.
class WidgetGpuCounterBuffer
{
public:
    template<class Read>
    PDH_STATUS ReadArray(Read&& read)
    {
        count_ = 0;
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            DWORD bytes = static_cast<DWORD>(storage_.size() * sizeof(std::uint64_t));
            DWORD count = 0;
            if (bytes)
            {
                const auto result = read(&bytes, &count, storage_.data());
                if (result == ERROR_SUCCESS) { count_ = count; return result; }
                if (result != PDH_MORE_DATA && result != PDH_INVALID_ARGUMENT) return result;
            }
            bytes = count = 0;
            const auto result = read(&bytes, &count, nullptr);
            if (result == ERROR_SUCCESS && count == 0) return result;
            if (result != PDH_MORE_DATA || !bytes) return result;
            constexpr DWORD maximum = 64 * 1024 * 1024;
            if (bytes > maximum) return PDH_MEMORY_ALLOCATION_FAILURE;
            const auto needed = (static_cast<std::size_t>(bytes) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
            if (needed > storage_.size()) { storage_.resize(needed); ++growths_; }
        }
        return PDH_MORE_DATA;
    }
    template<class T> const T* Items() const { return reinterpret_cast<const T*>(storage_.data()); }
    DWORD Count() const { return count_; }
    std::uint64_t Growths() const { return growths_; }
private:
    std::vector<std::uint64_t> storage_;
    DWORD count_ = 0;
    std::uint64_t growths_ = 0;
};
// The monotonic wall clock includes suspend; unbiased interrupt time does not.
// Long scheduling intervals alone must not continually reset valid baselines.
inline bool WidgetGpuResumed(std::uint64_t wallBefore, std::uint64_t awakeBefore,
    std::uint64_t wallNow, std::uint64_t awakeNow)
{
    if (!wallBefore || wallNow < wallBefore || awakeNow < awakeBefore) return false;
    const auto wall = wallNow - wallBefore, awake = awakeNow - awakeBefore;
    return wall > awake && wall - awake > 1000;
}
}
