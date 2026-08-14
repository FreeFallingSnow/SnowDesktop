#include "widget_text_input_rules.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::uint32_t kReplacementCharacter = 0xFFFD;

std::size_t Utf8CodePointBytes(std::uint32_t codePoint) noexcept
{
    if (codePoint <= 0x7F) return 1;
    if (codePoint <= 0x7FF) return 2;
    if (codePoint <= 0xFFFF) return 3;
    return 4;
}
}

std::size_t Utf8BytesForHostText(std::wstring_view text) noexcept
{
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        std::uint32_t codePoint =
            static_cast<std::uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2)
        {
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
            {
                if (index + 1 < text.size())
                {
                    const std::uint32_t low =
                        static_cast<std::uint32_t>(text[index + 1]);
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        codePoint = 0x10000 +
                            ((codePoint - 0xD800) << 10) +
                            (low - 0xDC00);
                        ++index;
                    }
                    else
                        codePoint = kReplacementCharacter;
                }
                else
                    codePoint = kReplacementCharacter;
            }
            else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
                codePoint = kReplacementCharacter;
        }
        else if (codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        {
            codePoint = kReplacementCharacter;
        }
        const std::size_t codePointBytes =
            Utf8CodePointBytes(codePoint);
        if (bytes > (std::numeric_limits<std::size_t>::max)() -
                codePointBytes)
            return (std::numeric_limits<std::size_t>::max)();
        bytes += codePointBytes;
    }
    return bytes;
}

bool HostTextReplacementFits(
    const std::wstring& text,
    std::size_t selectionStart,
    std::size_t selectionEnd,
    std::wstring_view replacement,
    std::size_t maximumUtf8Bytes)
{
    if (maximumUtf8Bytes == 0) return true;
    selectionStart = std::min(selectionStart, text.size());
    selectionEnd = std::clamp(selectionEnd,
        selectionStart, text.size());
    std::wstring candidate = text;
    candidate.replace(selectionStart,
        selectionEnd - selectionStart, replacement);
    return Utf8BytesForHostText(candidate) <= maximumUtf8Bytes;
}

bool TryApplyHostTextReplacement(
    std::wstring& text,
    std::size_t selectionStart,
    std::size_t selectionEnd,
    std::wstring_view replacement,
    std::size_t maximumUtf8Bytes,
    std::size_t& cursor)
{
    selectionStart = std::min(selectionStart, text.size());
    selectionEnd = std::clamp(selectionEnd,
        selectionStart, text.size());
    std::wstring candidate = text;
    candidate.replace(selectionStart,
        selectionEnd - selectionStart, replacement);
    if (maximumUtf8Bytes != 0 &&
        Utf8BytesForHostText(candidate) > maximumUtf8Bytes)
        return false;
    text = std::move(candidate);
    cursor = selectionStart + replacement.size();
    return true;
}
}
