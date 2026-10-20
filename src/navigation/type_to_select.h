#pragma once

#include "name_pinyin.h"

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace snowdesktop::type_to_select
{
struct InputContext
{
    bool textInput = false;
    bool overlay = false;
    bool gesture = false;
    bool commandModifiers = false;
};

constexpr bool AllowsTyping(const InputContext& context)
{
    return !context.textInput && !context.overlay &&
        !context.gesture && !context.commandModifiers;
}

struct Candidate
{
    std::wstring_view name;
    bool selected = false;
};

inline bool MatchesPrefix(std::wstring_view name, std::wstring_view query)
{
    if (query.empty() || name.empty()) return false;
    if (name.size() >= query.size() &&
        CompareStringOrdinal(name.data(), static_cast<int>(query.size()),
            query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL)
        return true;

    // Punctuation and committed Chinese text must retain their literal meaning.
    // Only a romanized query uses the existing full-pinyin/initials keys.
    if (!std::all_of(query.begin(), query.end(), [](wchar_t ch) {
            return name_pinyin::IsAsciiLetterOrDigit(ch) ||
                name_pinyin::IsLatinUmlaut(ch);
        }))
        return false;
    const std::string key = BuildNamePinyinFullKey(std::wstring(query));
    const auto startsWith = [&](const std::string& value) {
        return value.size() >= key.size() &&
            value.compare(0, key.size(), key) == 0;
    };
    return startsWith(BuildNamePinyinFullKey(std::wstring(name))) ||
        startsWith(BuildNamePinyinInitialKey(std::wstring(name)));
}

class Session
{
public:
    static constexpr std::uint64_t timeoutMs = 1000;

    void Reset()
    {
        query_.clear();
        scope_.clear();
        lastTick_ = 0;
    }

    // Automatic expansion of the current collection keeps the same query.
    void ContinueInScope(std::wstring_view scope) { scope_ = scope; }

    std::optional<size_t> Input(wchar_t ch, std::uint64_t tick,
        std::wstring_view scope, std::span<const Candidate> candidates,
        const InputContext& context = {}, bool repeated = false)
    {
        if (!AllowsTyping(context))
        {
            Reset();
            return std::nullopt;
        }
        if (ch < L' ' || ch == 0x7f) return std::nullopt;
        // A held key is not another deliberate incremental-search character.
        if (repeated) return std::nullopt;
        const bool fresh = query_.empty() || scope_ != scope ||
            tick < lastTick_ || tick - lastTick_ >= timeoutMs;
        if (fresh) query_.clear();
        scope_ = scope;
        lastTick_ = tick;
        query_.push_back(ch);

        size_t current = candidates.size();
        for (size_t i = 0; i < candidates.size(); ++i)
            if (candidates[i].selected) { current = i; break; }
        const auto find = [&](std::wstring_view query, bool advance)
            -> std::optional<size_t> {
            if (candidates.empty()) return std::nullopt;
            const size_t start = current < candidates.size()
                ? (current + (advance ? 1 : 0)) % candidates.size() : 0;
            for (size_t offset = 0; offset < candidates.size(); ++offset)
            {
                const size_t index = (start + offset) % candidates.size();
                if (MatchesPrefix(candidates[index].name, query)) return index;
            }
            return std::nullopt;
        };
        if (const auto match = find(query_, fresh)) return match;

        // Repeated letters cycle, but a real repeated prefix such as "aa" in
        // "Aaron" wins and the next "r" still forms "aar".
        if (query_.size() > 1 && name_pinyin::IsAsciiLetterOrDigit(ch) &&
            std::all_of(query_.begin(), query_.end(), [&](wchar_t value) {
                return name_pinyin::ToAsciiSearchChar(value) ==
                    name_pinyin::ToAsciiSearchChar(ch);
            }))
        {
            query_.resize(1);
            return find(query_, true);
        }
        return std::nullopt;
    }

private:
    std::wstring query_;
    std::wstring scope_;
    std::uint64_t lastTick_ = 0;
};
}
