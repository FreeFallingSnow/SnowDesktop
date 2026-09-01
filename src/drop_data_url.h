#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::drop_data_url
{

enum class DecodeStatus : std::uint8_t
{
    Decoded,
    NotDataUrl,
    Malformed,
    TooLarge,
};

struct DecodeResult
{
    DecodeStatus status = DecodeStatus::NotDataUrl;
    std::string contentType;
    std::vector<std::uint8_t> bytes;

    explicit operator bool() const noexcept
    {
        return status == DecodeStatus::Decoded;
    }
};

/**
 * Decode an RFC 2397 data URL without performing any external I/O.
 *
 * Unescaped input must be printable ASCII, percent escapes are interpreted as
 * individual bytes, and base64 input must use canonical padding and pad bits.
 * maximumBytes bounds the decoded payload; the limit is checked before the
 * output buffer is allocated and again after decoding.
 */
DecodeResult Decode(
    std::wstring_view uri, std::size_t maximumBytes) noexcept;

} // namespace snowdesktop::drop_data_url
