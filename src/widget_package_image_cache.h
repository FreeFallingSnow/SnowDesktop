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

    const PackageImageSource* Load(const std::wstring& path);
    const PackageImageSource* Find(const std::wstring& path) const noexcept;
    bool Failed(const std::wstring& path) const noexcept;
    std::size_t Size() const noexcept;
    std::size_t Bytes() const noexcept;
    void Clear() noexcept;

private:
    const PackageImageSource* Fail(const std::wstring& path);

    std::size_t maximumSingleBytes_ = DefaultMaximumSingleBytes;
    std::size_t maximumTotalBytes_ = DefaultMaximumTotalBytes;
    std::size_t bytes_ = 0;
    std::unordered_map<std::wstring, PackageImageSource> sources_;
    std::unordered_set<std::wstring> failures_;
};
}
