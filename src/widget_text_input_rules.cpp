#include "widget_text_input_rules.h"

#include <algorithm>
#include <cstdint>
#include <dwrite.h>
#include <limits>
#include <utility>
#include <vector>

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

std::uint32_t HostCodePointAt(std::wstring_view text,
    std::size_t index, std::size_t& units) noexcept
{
    units = 1;
    std::uint32_t codePoint = static_cast<std::uint32_t>(text[index]);
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
                    units = 2;
                    return 0x10000 + ((codePoint - 0xD800) << 10) +
                        (low - 0xDC00);
                }
            }
            return kReplacementCharacter;
        }
        if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
            return kReplacementCharacter;
    }
    else if (codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF))
    {
        return kReplacementCharacter;
    }
    return codePoint;
}
}

void DeferredHostInputFocus::Request(
    std::string controlId, std::string surface)
{
    controlId_ = std::move(controlId);
    surface_ = std::move(surface);
}

void DeferredHostInputFocus::Clear() noexcept
{
    controlId_.clear();
    surface_.clear();
}

bool DeferredHostInputFocus::Active() const noexcept
{
    return !controlId_.empty();
}

bool DeferredHostInputFocus::MatchesSurface(
    std::string_view surface) const noexcept
{
    return Active() && surface_ == surface;
}

const std::string& DeferredHostInputFocus::ControlId() const noexcept
{
    return controlId_;
}

void HostInputCaretVisibilityRequest::Request() noexcept
{
    pending_ = true;
}

void HostInputCaretVisibilityRequest::PreserveManualScroll() noexcept
{
    pending_ = false;
}

bool HostInputCaretVisibilityRequest::Consume() noexcept
{
    const bool pending = pending_;
    pending_ = false;
    return pending;
}

std::optional<std::size_t> ResolveHostInputVerticalCaretPosition(
    IDWriteTextLayout* layout,
    std::wstring_view text,
    std::size_t cursor,
    HostInputVerticalDirection direction)
{
    if (!layout || text.size() >
            static_cast<std::size_t>(
                (std::numeric_limits<UINT32>::max)()))
        return std::nullopt;

    cursor = std::min(cursor, text.size());
    UINT32 hitPosition = 0;
    BOOL trailing = FALSE;
    if (!text.empty())
    {
        if (cursor >= text.size() &&
            (text.back() == L'\n' || text.back() == L'\r'))
        {
            hitPosition = static_cast<UINT32>(text.size());
        }
        else if (cursor >= text.size())
        {
            hitPosition = static_cast<UINT32>(text.size() - 1);
            trailing = TRUE;
        }
        else
        {
            hitPosition = static_cast<UINT32>(cursor);
        }
    }

    float caretX = 0.0f;
    float caretY = 0.0f;
    DWRITE_HIT_TEST_METRICS caretMetrics{};
    if (FAILED(layout->HitTestTextPosition(
            hitPosition, trailing, &caretX, &caretY,
            &caretMetrics)))
        return std::nullopt;

    UINT32 lineCount = 0;
    (void)layout->GetLineMetrics(nullptr, 0, &lineCount);
    if (lineCount == 0)
        return std::nullopt;
    std::vector<DWRITE_LINE_METRICS> lines(lineCount);
    UINT32 actualLineCount = 0;
    if (FAILED(layout->GetLineMetrics(
            lines.data(), lineCount, &actualLineCount)) ||
        actualLineCount == 0)
        return std::nullopt;
    lines.resize(actualLineCount);

    std::size_t currentLine = lines.size() - 1;
    float lineTop = 0.0f;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const float lineBottom = lineTop + lines[index].height;
        if (caretY < lineBottom || index + 1 == lines.size())
        {
            currentLine = index;
            break;
        }
        lineTop = lineBottom;
    }

    if (direction == HostInputVerticalDirection::Up)
    {
        if (currentLine == 0)
            return cursor;
        --currentLine;
    }
    else
    {
        if (currentLine + 1 >= lines.size())
            return cursor;
        ++currentLine;
    }

    std::size_t targetStart = 0;
    float targetTop = 0.0f;
    for (std::size_t index = 0; index < currentLine; ++index)
    {
        targetStart += lines[index].length;
        targetTop += lines[index].height;
    }
    const auto& targetLine = lines[currentLine];
    const float targetY = targetTop +
        std::max(1.0f, targetLine.height) * 0.5f;
    BOOL targetTrailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS targetMetrics{};
    if (FAILED(layout->HitTestPoint(
            caretX, targetY, &targetTrailing, &inside,
            &targetMetrics)))
        return std::nullopt;

    std::size_t next =
        static_cast<std::size_t>(targetMetrics.textPosition) +
        (targetTrailing
            ? static_cast<std::size_t>(targetMetrics.length) : 0);
    const std::size_t targetLength =
        static_cast<std::size_t>(targetLine.length);
    const std::size_t newlineLength = std::min(
        static_cast<std::size_t>(targetLine.newlineLength),
        targetLength);
    const std::size_t targetEnd = targetStart +
        targetLength - newlineLength;
    next = std::clamp(next, targetStart, targetEnd);
    return std::min(next, text.size());
}

std::size_t Utf8BytesForHostText(std::wstring_view text) noexcept
{
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        std::size_t units = 1;
        const std::uint32_t codePoint =
            HostCodePointAt(text, index, units);
        const std::size_t codePointBytes =
            Utf8CodePointBytes(codePoint);
        if (bytes > (std::numeric_limits<std::size_t>::max)() -
                codePointBytes)
            return (std::numeric_limits<std::size_t>::max)();
        bytes += codePointBytes;
        index += units;
    }
    return bytes;
}

std::size_t Utf8ByteOffsetForHostTextOffset(
    std::wstring_view text, std::size_t hostOffset) noexcept
{
    hostOffset = std::min(hostOffset, text.size());
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < hostOffset;)
    {
        std::size_t units = 1;
        const std::uint32_t codePoint =
            HostCodePointAt(text, index, units);
        const std::size_t codePointBytes =
            Utf8CodePointBytes(codePoint);
        if (bytes > (std::numeric_limits<std::size_t>::max)() -
                codePointBytes)
            return (std::numeric_limits<std::size_t>::max)();
        bytes += codePointBytes;
        index += units;
    }
    return bytes;
}

std::optional<std::size_t> HostTextOffsetFromUtf8ByteOffset(
    std::wstring_view text, std::size_t utf8ByteOffset) noexcept
{
    if (utf8ByteOffset == 0) return std::size_t{0};
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        std::size_t units = 1;
        const std::uint32_t codePoint =
            HostCodePointAt(text, index, units);
        bytes += Utf8CodePointBytes(codePoint);
        index += units;
        if (bytes == utf8ByteOffset) return index;
        if (bytes > utf8ByteOffset) return std::nullopt;
    }
    return std::nullopt;
}

bool HostInputAllowsMutation(bool enabled, bool readOnly) noexcept
{
    return enabled && !readOnly;
}

HostInputContextMenuState ResolveHostInputContextMenuState(
    std::size_t textLength,
    std::size_t cursor,
    std::size_t selectionAnchor,
    bool enabled,
    bool readOnly,
    bool clipboardHasText) noexcept
{
    cursor = std::min(cursor, textLength);
    selectionAnchor = std::min(selectionAnchor, textLength);
    const bool hasSelection = cursor != selectionAnchor;
    const bool mutableInput = HostInputAllowsMutation(enabled, readOnly);
    return {
        textLength != 0,
        mutableInput && hasSelection,
        hasSelection,
        mutableInput && clipboardHasText,
    };
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
