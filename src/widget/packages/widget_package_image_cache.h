#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
        PackageImageAcquireError* error = nullptr);
    bool Release(const std::string& contentKey) noexcept;
    const PackageImageSource* Find(
        const std::string& contentKey) const noexcept;
    bool Failed(const std::string& contentKey) const noexcept;
    std::size_t ReferenceCount(
        const std::string& contentKey) const noexcept;
    std::size_t Size() const noexcept;
    std::size_t Bytes() const noexcept;
    void Clear() noexcept;

private:
    struct Entry
    {
        PackageImageSource source;
        std::size_t references = 0;
    };

    const PackageImageSource* Fail(const std::string& contentKey,
        PackageImageAcquireError* error);
    void CancelPrewarm() noexcept;

    std::size_t maximumSingleBytes_ = DefaultMaximumSingleBytes;
    std::size_t maximumTotalBytes_ = DefaultMaximumTotalBytes;
    std::size_t bytes_ = 0;
    std::unordered_map<std::string, Entry> sources_;
    // Speculative pixels never consume live resource quota or references.
    std::unordered_map<std::string, PackageImageSource> prepared_;
    std::size_t preparedBytes_ = 0;
    std::unique_ptr<snowdesktop::BackgroundWork> prewarmWork_;
    std::shared_ptr<std::atomic_bool> prewarmCancelled_;
    bool prewarmPending_ = false;
    // Failed digests have no resource handle whose release can retire them.
    // Bound their lifetime independently of decoded sources and references.
    static constexpr std::size_t MaximumFailureEntries = 512;
    std::deque<std::string> failureOrder_;
    std::unordered_set<std::string> failures_;
};
}
