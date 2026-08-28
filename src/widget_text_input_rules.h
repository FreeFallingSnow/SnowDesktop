#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct IDWriteTextLayout;

namespace snowdesktop::widget_runtime
{
class DeferredHostInputFocus
{
public:
    void Request(std::string controlId, std::string surface);
    void Clear() noexcept;

    bool Active() const noexcept;
    bool MatchesSurface(std::string_view surface) const noexcept;
    const std::string& ControlId() const noexcept;

private:
    std::string controlId_;
    std::string surface_;
};

class HostInputCaretVisibilityRequest
{
public:
    void Request() noexcept;
    void PreserveManualScroll() noexcept;
    bool Consume() noexcept;

private:
    bool pending_ = false;
};

enum class HostInputVerticalDirection
{
    Up,
    Down,
};

std::optional<std::size_t> ResolveHostInputVerticalCaretPosition(
    IDWriteTextLayout* layout,
    std::wstring_view text,
    std::size_t cursor,
    HostInputVerticalDirection direction);

std::size_t Utf8BytesForHostText(std::wstring_view text) noexcept;
std::size_t Utf8ByteOffsetForHostTextOffset(
    std::wstring_view text, std::size_t hostOffset) noexcept;
std::optional<std::size_t> HostTextOffsetFromUtf8ByteOffset(
    std::wstring_view text, std::size_t utf8ByteOffset) noexcept;

bool HostInputAllowsMutation(bool enabled, bool readOnly) noexcept;

enum class HostInputEditCommand
{
    SelectAll,
    Cut,
    Copy,
    Paste,
};

struct HostInputContextMenuState
{
    bool canSelectAll = false;
    bool canCut = false;
    bool canCopy = false;
    bool canPaste = false;
};

HostInputContextMenuState ResolveHostInputContextMenuState(
    std::size_t textLength,
    std::size_t cursor,
    std::size_t selectionAnchor,
    bool enabled,
    bool readOnly,
    bool clipboardHasText) noexcept;

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
