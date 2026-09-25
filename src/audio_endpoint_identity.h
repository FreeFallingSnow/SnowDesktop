#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop
{
// Keep the API v2 output identity stable, including its historical prefix.
inline std::string OpaqueAudioEndpointId(std::wstring_view id)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t character : id)
    { hash ^= static_cast<std::uint16_t>(character); hash *= 1099511628211ull; }
    return "audio-output-" + std::to_string(hash);
}
}
