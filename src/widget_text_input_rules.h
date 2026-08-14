#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace snowdesktop::widget_runtime
{
std::size_t Utf8BytesForHostText(std::wstring_view text) noexcept;

bool HostTextReplacementFits(
    const std::wstring& text,
    std::size_t selectionStart,
    std::size_t selectionEnd,
    std::wstring_view replacement,
    std::size_t maximumUtf8Bytes);

bool TryApplyHostTextReplacement(
    std::wstring& text,
    std::size_t selectionStart,
    std::size_t selectionEnd,
    std::wstring_view replacement,
    std::size_t maximumUtf8Bytes,
    std::size_t& cursor);
}
