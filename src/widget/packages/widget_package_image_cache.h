#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop { class BackgroundWork; }

namespace snowdesktop::widget_runtime
{
struct PackageImageSource
{
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

struct PackageImageSize
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

enum class PackageImageAcquireError : unsigned char
{
    None,
    InvalidInput,
    DecodeFailed,
    QuotaExceeded,
};

class WidgetPackageImageCache
{
public:
    static constexpr std::size_t DefaultMaximumSingleBytes =
        64ull * 1024ull * 1024ull;
    static constexpr std::size_t DefaultMaximumTotalBytes =
        128ull * 1024ull * 1024ull;

    WidgetPackageImageCache();
    WidgetPackageImageCache(
        std::size_t maximumSingleBytes,
        std::size_t maximumTotalBytes);
    ~WidgetPackageImageCache();

    // Owner-thread entry points. The digest callback runs on a private worker
    // over immutable encoded bytes and must not capture Lua/device/host state.
    using ContentDigest = std::function<std::string(
        std::span<const std::uint8_t>)>;
    void Prewarm(std::vector<std::wstring> files,
        std::vector<std::wstring> images, ContentDigest digest);
    bool PrewarmPending();
    std::size_t PreparedBytes() const noexcept;

    const PackageImageSource* Acquire(
        const std::string& contentKey, const std::wstring& path,
        PackageImageAcquireError* error = nullptr, bool lazy = false,
        ContentDigest digest = {});
    bool Release(const std::string& contentKey) noexcept;
    const PackageImageSource* Find(
        const std::string& contentKey) const noexcept;
    bool Failed(const std::string& contentKey) const noexcept;
    std::size_t ReferenceCount(
        const std::string& contentKey) const noexcept;
    std::size_t Size() const noexcept;
    std::size_t Bytes() const noexcept;
    std::size_t EncodedBytes() const noexcept;
    PackageImageSize OriginalSize(const std::string& key) const noexcept;
    std::uint64_t Revision(const std::string& key) const noexcept;
    bool Ready(const std::string& key) const noexcept;
    bool DecodeFailed(const std::string& key) const noexcept;
    bool Validate(const std::string& key,
        PackageImageAcquireError* error = nullptr);

    using Clock = std::chrono::steady_clock;
    // Owner identifies one widget surface. Beginning its next paint replaces
    // that surface's demands; a static cached surface retains its last demand.
    void BeginFrame(const std::string& owner);
    void ForgetOwner(const std::string& prefix);
    const PackageImageSource* Request(const std::string& key,
        const std::string& owner, double rasterScale,
        Clock::time_point now = Clock::now(), bool synchronous = false);
    void Pump(Clock::time_point now = Clock::now());
    void SetReadyCallback(std::function<void()> callback);
    std::vector<std::string> TakeChanged();
    static std::uint32_t ResolutionBucket(
        PackageImageSize original, double rasterScale) noexcept;
    void Clear() noexcept;

private:
    struct Entry
    {
        PackageImageSource source;
        std::size_t references = 0;
        std::shared_ptr<const std::vector<std::uint8_t>> encoded;
        PackageImageSize original;
        std::size_t reservedBytes = 0;
        std::uint64_t identity = 0;
        std::uint64_t revision = 0;
        bool ready = false;
        bool decodeFailed = false;
        bool pending = false;
        std::uint32_t wantedEdge = 0;
        Clock::time_point changedAt = Clock::now();
        std::unordered_map<std::string, std::uint32_t> demands;
    };

    struct PreparedImage
    {
        PackageImageSource source;
        std::shared_ptr<const std::vector<std::uint8_t>> encoded;
    };

    const PackageImageSource* Fail(const std::string& contentKey,
        PackageImageAcquireError* error);
    void CancelPrewarm() noexcept;
    void UpdateDemand(Entry& entry, Clock::time_point now);
    void Schedule(const std::string& key, Entry& entry,
        Clock::time_point now, bool synchronous);

    std::size_t maximumSingleBytes_ = DefaultMaximumSingleBytes;
    std::size_t maximumTotalBytes_ = DefaultMaximumTotalBytes;
    std::size_t bytes_ = 0;
    std::size_t reservedBytes_ = 0;
    std::size_t encodedBytes_ = 0;
    std::size_t pendingBytes_ = 0;
    std::uint64_t nextIdentity_ = 0;
    std::unordered_map<std::string, Entry> sources_;
    // Speculative pixels never consume live resource quota or references.
    std::unordered_map<std::string, PreparedImage> prepared_;
    std::size_t preparedBytes_ = 0;
    std::unique_ptr<snowdesktop::BackgroundWork> prewarmWork_;
    std::shared_ptr<std::atomic_bool> prewarmCancelled_;
    bool prewarmPending_ = false;
    std::unique_ptr<snowdesktop::BackgroundWork> decodeWork_;
    std::function<void()> onReady_;
    std::vector<std::string> changed_;
    // Failed digests have no resource handle whose release can retire them.
    // Bound their lifetime independently of decoded sources and references.
    static constexpr std::size_t MaximumFailureEntries = 512;
    std::deque<std::string> failureOrder_;
    std::unordered_set<std::string> failures_;
};
}
