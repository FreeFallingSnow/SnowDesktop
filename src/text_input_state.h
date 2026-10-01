#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

// Internal editing rules. Offsets are UTF-16 code units on Windows; callers
// translate public Lua selections to/from UTF-8 at the existing API boundary.
namespace snowdesktop::text_input
{
inline std::uint32_t Codepoint(std::wstring_view value, std::size_t at,
    std::size_t& next) noexcept
{
    next = (std::min)(at + 1, value.size());
    if (at >= value.size()) return 0;
    auto point = static_cast<std::uint32_t>(value[at]);
    if constexpr (sizeof(wchar_t) == 2)
    {
        if (point >= 0xd800 && point <= 0xdbff && next < value.size())
        {
            const auto low = static_cast<std::uint32_t>(value[next]);
            if (low >= 0xdc00 && low <= 0xdfff)
            { ++next; point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00; }
        }
    }
    return point;
}

inline bool ExtendsCluster(std::uint32_t point) noexcept
{
#ifdef _WIN32
    if(point<=0xffff)
    {
        const auto ch=static_cast<wchar_t>(point);WORD type=0;
        if(GetStringTypeW(CT_CTYPE3,&ch,1,&type)&&(type&(C3_NONSPACING|C3_VOWELMARK)))return true;
    }
#endif
    // Combining marks used by the supported scripts, variation selectors,
    // emoji modifiers and tags. ZWJ is handled together with its next scalar.
    return (point >= 0x0300 && point <= 0x036f) ||
        (point >= 0x0483 && point <= 0x0489) ||
        (point >= 0x0591 && point <= 0x05bd) || point == 0x05bf ||
        (point >= 0x05c1 && point <= 0x05c2) || (point >= 0x05c4 && point <= 0x05c5) ||
        (point >= 0x0610 && point <= 0x061a) || (point >= 0x064b && point <= 0x065f) ||
        point == 0x0670 || (point >= 0x06d6 && point <= 0x06ed) ||
        (point >= 0x1ab0 && point <= 0x1aff) || (point >= 0x1dc0 && point <= 0x1dff) ||
        (point >= 0x20d0 && point <= 0x20ff) || (point >= 0xfe00 && point <= 0xfe0f) ||
        (point >= 0xfe20 && point <= 0xfe2f) || (point >= 0x1f3fb && point <= 0x1f3ff) ||
        (point >= 0xe0100 && point <= 0xe01ef) || (point >= 0xe0020 && point <= 0xe007f);
}

inline std::size_t NextBoundary(std::wstring_view value, std::size_t at) noexcept
{
    at = (std::min)(at, value.size());
    std::size_t next = at;
    const auto first = Codepoint(value, at, next);
    if (at == value.size()) return at;
    if (first == '\r' && next < value.size() && value[next] == '\n') return next + 1;
    bool regional = first >= 0x1f1e6 && first <= 0x1f1ff;
    while (next < value.size())
    {
        std::size_t end = next;
        const auto point = Codepoint(value, next, end);
        if (ExtendsCluster(point)) { next = end; continue; }
        if (point == 0x200d && end < value.size())
        { Codepoint(value, end, next); regional = false; continue; }
        if (regional && point >= 0x1f1e6 && point <= 0x1f1ff)
        { next = end; regional = false; continue; }
        break;
    }
    return next;
}

inline std::size_t PreviousBoundary(std::wstring_view value, std::size_t at) noexcept
{
    at = (std::min)(at, value.size());
    std::size_t previous = 0;
    for (std::size_t next = 0; next < at;)
    {
        previous = next;
        next = NextBoundary(value, next);
    }
    return previous;
}

inline std::size_t SnapBoundary(std::wstring_view value, std::size_t at) noexcept
{
    at = (std::min)(at, value.size());
    std::size_t next = 0;
    while (next < at)
    {
        const auto end = NextBoundary(value, next);
        if (end > at) return next;
        next = end;
    }
    return next;
}

inline bool WordSeparator(wchar_t ch) noexcept
{
    return ch <= L' ' || std::wstring_view(L".,;:!?/\\()[]{}<>\"'|+-=*").find(ch) != std::wstring_view::npos;
}

inline std::size_t WordBoundary(std::wstring_view value, std::size_t at, bool forward) noexcept
{
    at = SnapBoundary(value, at);
    if (forward)
    {
        while (at < value.size() && !WordSeparator(value[at])) at = NextBoundary(value, at);
        while (at < value.size() && WordSeparator(value[at])) at = NextBoundary(value, at);
    }
    else
    {
        while (at && WordSeparator(value[PreviousBoundary(value, at)])) at = PreviousBoundary(value, at);
        while (at && !WordSeparator(value[PreviousBoundary(value, at)])) at = PreviousBoundary(value, at);
    }
    return at;
}

struct Snapshot
{
    std::wstring text;
    std::size_t cursor = 0, anchor = 0;
};

class History
{
public:
    void Clear() { undo_.clear(); redo_.clear(); }
    void Record(Snapshot before, const Snapshot& after)
    {
        if (before.text == after.text) return;
        if (undo_.size() == 128) undo_.erase(undo_.begin());
        undo_.push_back(std::move(before)); redo_.clear();
    }
    bool Undo(std::wstring& text, std::size_t& cursor, std::size_t& anchor)
    { return Restore(undo_, redo_, text, cursor, anchor); }
    bool Redo(std::wstring& text, std::size_t& cursor, std::size_t& anchor)
    { return Restore(redo_, undo_, text, cursor, anchor); }
    bool CanUndo() const noexcept { return !undo_.empty(); }
    bool CanRedo() const noexcept { return !redo_.empty(); }
private:
    static bool Restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to,
        std::wstring& text, std::size_t& cursor, std::size_t& anchor)
    {
        if (from.empty()) return false;
        to.push_back({std::move(text), cursor, anchor});
        auto state = std::move(from.back()); from.pop_back();
        text = std::move(state.text); cursor = state.cursor; anchor = state.anchor;
        return true;
    }
    std::vector<Snapshot> undo_, redo_;
};

// One record per public operation, including a committed IME result. Password
// adapters deliberately never construct this guard or an ordinary History.
class EditRecord
{
public:
    EditRecord(History& history, const std::wstring& text,
        const std::size_t& cursor, const std::size_t& anchor)
        : history_(history), text_(text), cursor_(cursor), anchor_(anchor),
          before_{text, cursor, anchor} {}
    ~EditRecord() { history_.Record(std::move(before_), {text_, cursor_, anchor_}); }
    EditRecord(const EditRecord&) = delete;
    EditRecord& operator=(const EditRecord&) = delete;
private:
    History& history_;
    const std::wstring& text_;
    const std::size_t& cursor_;
    const std::size_t& anchor_;
    Snapshot before_;
};
}
