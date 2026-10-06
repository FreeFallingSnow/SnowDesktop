#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct IDWriteTextLayout;

namespace snowdesktop::widget_runtime
{
// A keyboard submit may restore its own editor after storage commit has blurred
// it. The outer widget selection is deliberately cleared by input-field clicks.
// Borrowed identifiers live for the synchronous callback; nested scopes restore
// the previous grant and never retain a request for a later render or timer.
class HostInputSubmitFocusScope
{
public:
    HostInputSubmitFocusScope(const HostInputSubmitFocusScope*& active,
        std::wstring_view widgetId, std::string_view controlId,
        std::string_view surface) noexcept
        : active_(active), previous_(active), widgetId_(widgetId),
          controlId_(controlId), surface_(surface)
    {
        active_ = this;
    }

    ~HostInputSubmitFocusScope() { active_ = previous_; }

    bool Matches(std::wstring_view widgetId, std::string_view controlId,
        std::string_view surface) const noexcept
    {
        return widgetId_ == widgetId && controlId_ == controlId && surface_ == surface;
    }

    bool AllowsFocus(std::wstring_view widgetId, std::string_view controlId,
        std::string_view surface, bool valid, bool preview,
        bool visible) const noexcept
    {
        return valid && !preview && visible && Matches(widgetId, controlId, surface);
    }

    HostInputSubmitFocusScope(const HostInputSubmitFocusScope&) = delete;
    HostInputSubmitFocusScope& operator=(const HostInputSubmitFocusScope&) = delete;

private:
    const HostInputSubmitFocusScope*& active_;
    const HostInputSubmitFocusScope* previous_;
    std::wstring_view widgetId_;
    std::string_view controlId_;
    std::string_view surface_;
};

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

struct HostInputVerticalExtents
{
    int content = 1;
    int viewport = 1;
};

HostInputVerticalExtents ResolveHostInputVerticalExtents(
    float viewportHeight, float measuredContentHeight) noexcept;

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
