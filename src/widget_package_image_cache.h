#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct PackageImageSource
{
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
};

class WidgetPackageImageCache
{
public:
    static constexpr std::size_t DefaultMaximumSingleBytes =
        64ull * 1024ull * 1024ull;
    static constexpr std::size_t DefaultMaximumTotalBytes =
        128ull * 1024ull * 1024ull;

    WidgetPackageImageCache() = default;
    WidgetPackageImageCache(
        std::size_t maximumSingleBytes,
        std::size_t maximumTotalBytes);

    const PackageImageSource* Acquire(
        const std::string& contentKey, const std::wstring& path);
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

    const PackageImageSource* Fail(const std::string& contentKey);

    std::size_t maximumSingleBytes_ = DefaultMaximumSingleBytes;
    std::size_t maximumTotalBytes_ = DefaultMaximumTotalBytes;
    std::size_t bytes_ = 0;
    std::unordered_map<std::string, Entry> sources_;
    std::unordered_set<std::string> failures_;
};
}
