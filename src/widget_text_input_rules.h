#pragma once

#include <cstddef>
#include <string>
#include <string_view>

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
