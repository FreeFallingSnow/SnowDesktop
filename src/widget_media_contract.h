#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop::widget_runtime
{
inline constexpr std::size_t MaximumExposedMediaSessions = 32;

inline std::string OpaqueMediaSessionId(
    std::wstring_view sourceId, std::size_t occurrence)
{
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = offset;
    for (const wchar_t character : sourceId)
    {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= prime;
    }
    hash ^= occurrence;
    hash *= prime;
    return "media-session-" + std::to_string(hash);
}
}
