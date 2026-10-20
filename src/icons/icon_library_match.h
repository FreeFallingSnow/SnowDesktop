#pragma once
#include "icon_library.h"
#include <algorithm>
#include <array>
#include <cwctype>
#include <numeric>
#include <string_view>

namespace snowdesktop::icon_library_match
{
namespace detail
{
struct Name
{
    std::wstring key;
    std::vector<std::wstring> words;
};
inline Name Normalize(std::wstring_view text)
{
    if (const auto slash = text.find_last_of(L"/\\"); slash != text.npos) text.remove_prefix(slash + 1);
    std::wstring lower(text.substr(0, 192));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    for (const auto extension : {L".lnk", L".url", L".exe", L".png", L".ico", L".jpg", L".jpeg", L".bmp", L".svg", L".webp"})
        if (lower.ends_with(extension)) { lower.resize(lower.size() - std::wstring_view(extension).size()); break; }
    Name result;
    std::wstring word;
    const auto append = [&] {
        if (!word.empty() && word != L"icon" && word != L"icons" && word != L"ic" && word != L"launcher" &&
            word != L"shortcut" && word != L"快捷方式") // l10n-allow: filename suffix, not user-facing text.
        { result.key += word; result.words.push_back(word); }
        word.clear();
    };
    for (const auto ch : lower)
    {
        // Retain CJK letters even when the process uses the C locale.
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9') ||
            (ch >= 128 && !std::iswspace(ch) && !std::iswpunct(ch))) word += ch;
        else append();
    }
    append();
    return result;
}
inline int Score(const Name& target, const Name& candidate)
{
    if (target.key.empty() || candidate.key.empty()) return 0;
    if (target.key == candidate.key) return 1000;
    const auto shorter = std::min(target.key.size(), candidate.key.size());
    const auto longer = std::max(target.key.size(), candidate.key.size());
    const bool cjk = std::any_of(target.key.begin(), target.key.end(), [](wchar_t ch) { return ch >= 0x2e80; });
    if (shorter >= (cjk ? 2u : 3u) &&
        (target.key.find(candidate.key) != target.key.npos || candidate.key.find(target.key) != candidate.key.npos))
        return 800 + static_cast<int>(100 * shorter / longer);
    size_t matched = 0;
    for (const auto& word : target.words)
        if (std::find(candidate.words.begin(), candidate.words.end(), word) != candidate.words.end()) matched += word.size();
    if (matched >= 3 && matched * 2 >= target.key.size()) return 700 + static_cast<int>(100 * matched / target.key.size());
    // Small spelling errors are useful; short names and large edit distances
    // otherwise recommend unrelated assets. Bound both work and tolerance.
    const size_t tolerance = shorter >= 6 ? 2 : 1;
    if (shorter < 4 || longer > 64 || longer - shorter > tolerance) return 0;
    std::array<size_t, 65> previous{}, current{};
    std::iota(previous.begin(), previous.end(), size_t{});
    for (size_t i = 1; i <= target.key.size(); ++i)
    {
        current[0] = i;
        for (size_t j = 1; j <= candidate.key.size(); ++j)
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1,
                previous[j - 1] + (target.key[i - 1] == candidate.key[j - 1] ? 0u : 1u)});
        previous.swap(current);
    }
    const auto distance = previous[candidate.key.size()];
    return distance <= tolerance ? 600 - static_cast<int>(distance * 20) : 0;
}
}

// Indices keep stable asset identities and avoid copying/renaming stored data.
inline std::vector<size_t> Recommend(const std::vector<IconLibraryEntry>& entries, std::wstring_view targetName)
{
    const auto target = detail::Normalize(targetName);
    struct Match { size_t index; int score; };
    std::vector<Match> matches;
    if (target.key.empty()) return {};
    for (size_t index = 0; index < entries.size(); ++index)
    {
        const auto& entry = entries[index];
        if (entry.missing) continue;
        int score = detail::Score(target, detail::Normalize(entry.name));
        for (const auto& [group, alias] : entry.groupNames)
        { (void)group; score = std::max(score, detail::Score(target, detail::Normalize(alias))); }
        if (score > 0) matches.push_back({index, score});
    }
    std::stable_sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) { return left.score > right.score; });
    std::vector<size_t> result;
    result.reserve(matches.size());
    for (const auto& match : matches) result.push_back(match.index);
    return result;
}

inline std::vector<size_t> AllWithRecommendations(const std::vector<IconLibraryEntry>& entries, std::wstring_view targetName)
{
    auto result = Recommend(entries, targetName);
    std::vector<bool> included(entries.size(), false);
    for (const auto index : result) included[index] = true;
    result.reserve(entries.size());
    for (size_t index = 0; index < entries.size(); ++index)
        if (!included[index]) result.push_back(index);
    return result;
}
}
