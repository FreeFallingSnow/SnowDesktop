#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::widget_runtime
{
struct WidgetRuntimeImagePixels
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> bgraPremultiplied;
};

inline bool IsValidWidgetRuntimeImage(
    const WidgetRuntimeImagePixels& pixels,
    std::uint32_t maximumDimension = 512) noexcept
{
    return pixels.width > 0 && pixels.height > 0 &&
        pixels.width <= maximumDimension &&
        pixels.height <= maximumDimension &&
        pixels.stride == pixels.width * 4 &&
        pixels.bgraPremultiplied.size() ==
            static_cast<std::size_t>(pixels.stride) * pixels.height;
}

inline std::string MakeWidgetRuntimeImageToken(
    std::string_view source, const WidgetRuntimeImagePixels& pixels)
{
    if (source.empty() || source.size() > 16 ||
        !IsValidWidgetRuntimeImage(pixels))
        return {};
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    const auto mix = [&](std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
        {
            hash ^= (value >> shift) & 0xffu;
            hash *= prime;
        }
    };
    mix(pixels.width);
    mix(pixels.height);
    for (const std::uint8_t value : pixels.bgraPremultiplied)
    {
        hash ^= value;
        hash *= prime;
    }
    return "@" + std::string(source) + ":" + std::to_string(hash);
}

inline bool IsWidgetRuntimeImageToken(std::string_view token) noexcept
{
    return (token.starts_with("@media:") ||
            token.starts_with("@clipboard:")) &&
        token.size() <= 64;
}
}
