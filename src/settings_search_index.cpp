#include "settings_search_index.h"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace snowdesktop
{
namespace
{

char32_t FoldCase(char32_t value) noexcept
{
    if (value >= U'A' && value <= U'Z') return value + 0x20;
    if ((value >= 0x00c0 && value <= 0x00d6) ||
        (value >= 0x00d8 && value <= 0x00de))
        return value + 0x20;

    if (value >= 0x0100 && value <= 0x012f && value % 2 == 0)
        return value + 1;
    if (value >= 0x0132 && value <= 0x0137 && value % 2 == 0)
        return value + 1;
    if (value >= 0x0139 && value <= 0x0148 && value % 2 == 1)
        return value + 1;
    if (value >= 0x014a && value <= 0x0177 && value % 2 == 0)
        return value + 1;
    if (value >= 0x0179 && value <= 0x017e && value % 2 == 1)
        return value + 1;

    if (value >= 0x0391 && value <= 0x03a1) return value + 0x20;
    if (value >= 0x03a3 && value <= 0x03ab) return value + 0x20;
    if (value == 0x03c2) return 0x03c3;
    if (value >= 0x0400 && value <= 0x040f) return value + 0x50;
    if (value >= 0x0410 && value <= 0x042f) return value + 0x20;
    if (value >= 0x0460 && value <= 0x0481 && value % 2 == 0)
        return value + 1;
    if (value >= 0x0531 && value <= 0x0556) return value + 0x30;
    if (value >= 0x1c90 && value <= 0x1cbf) return value - 0x0bc0;
    if (value >= 0xff21 && value <= 0xff3a) return value + 0x20;
    if (value >= 0x10400 && value <= 0x10427) return value + 0x28;

    switch (value)
    {
    case 0x0130: return U'i';
    case 0x0178: return 0x00ff;
    case 0x1e9e: return 0x00df;
    case 0x2126: return 0x03c9;
    case 0x212a: return U'k';
    case 0x212b: return 0x00e5;
    default: return value;
    }
}

bool IsSeparator(char32_t value) noexcept
{
    if (value <= 0x7f)
    {
        return !((value >= U'a' && value <= U'z') ||
            (value >= U'A' && value <= U'Z') ||
            (value >= U'0' && value <= U'9'));
    }

    if (value == 0x0085 || value == 0x00a0 || value == 0x1680 ||
        value == 0x180e || value == 0x3000 || value == 0xfeff)
        return true;
    if ((value >= 0x2000 && value <= 0x206f) ||
        (value >= 0x2e00 && value <= 0x2e7f) ||
        (value >= 0x3001 && value <= 0x303f) ||
        (value >= 0xfe10 && value <= 0xfe1f) ||
        (value >= 0xfe30 && value <= 0xfe4f) ||
        (value >= 0xff01 && value <= 0xff0f) ||
        (value >= 0xff1a && value <= 0xff20) ||
        (value >= 0xff3b && value <= 0xff40) ||
        (value >= 0xff5b && value <= 0xff65))
        return true;
    return false;
}

template <typename Callback>
void ForEachCodePoint(std::wstring_view text, Callback&& callback)
{
    if constexpr (sizeof(wchar_t) == 2)
    {
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            char32_t value = static_cast<char32_t>(text[index]);
            if (value >= 0xd800 && value <= 0xdbff &&
                index + 1 < text.size())
            {
                const char32_t low =
                    static_cast<char32_t>(text[index + 1]);
                if (low >= 0xdc00 && low <= 0xdfff)
                {
                    value = 0x10000 + ((value - 0xd800) << 10) +
                        (low - 0xdc00);
                    ++index;
                }
            }
            callback(value);
        }
    }
    else
    {
        for (wchar_t value : text)
            callback(static_cast<char32_t>(value));
    }
}

std::u32string Normalize(std::wstring_view text)
{
    std::u32string result;
    result.reserve(text.size());
    bool pendingSpace = false;
    ForEachCodePoint(text, [&](char32_t value) {
        value = FoldCase(value);
        if (IsSeparator(value))
        {
            pendingSpace = !result.empty();
            return;
        }
        if (pendingSpace)
        {
            result.push_back(U' ');
            pendingSpace = false;
        }
        result.push_back(value);
    });
    return result;
}

void AppendNormalized(std::u32string& destination,
    std::wstring_view value)
{
    const std::u32string normalized = Normalize(value);
    if (normalized.empty()) return;
    if (!destination.empty()) destination.push_back(U' ');
    destination.append(normalized);
}

std::vector<std::u32string> SplitTerms(const std::u32string& query)
{
    std::vector<std::u32string> terms;
    std::size_t begin = 0;
    while (begin < query.size())
    {
        const std::size_t end = query.find(U' ', begin);
        terms.emplace_back(query.substr(begin,
            end == std::u32string::npos
                ? std::u32string::npos : end - begin));
        if (end == std::u32string::npos) break;
        begin = end + 1;
    }
    return terms;
}

int MatchField(const std::u32string& field,
    const std::u32string& term, int baseScore)
{
    const std::size_t position = field.find(term);
    if (position == std::u32string::npos)
        return (std::numeric_limits<int>::max)();
    if (position == 0) return baseScore;
    if (field[position - 1] == U' ') return baseScore + 2;
    return baseScore + 5;
}

bool PageMayBeIndexed(SettingsPage page,
    const SettingsSearchIndexInput& input) noexcept
{
    if (page == SettingsPage::DeveloperTools)
        return input.developerToolsVisible;
    if (page == SettingsPage::Debug)
        return input.debugVisible;
    return true;
}

using EntryIdentity = std::tuple<int, std::wstring, std::string>;

} // namespace

SettingsSearchIndex::SettingsSearchIndex(
    const SettingsSearchIndexInput& input)
{
    Rebuild(input);
}

void SettingsSearchIndex::Rebuild(
    const SettingsSearchIndexInput& input)
{
    std::vector<Entry> rebuilt;
    rebuilt.reserve(input.staticSettings.size());
    std::set<EntryIdentity> identities;
    std::size_t ordinal = 0;

    for (const auto& descriptor : input.staticSettings)
    {
        if (!descriptor.visible || descriptor.focusId.empty() ||
            descriptor.label.empty() ||
            !PageMayBeIndexed(descriptor.page, input))
            continue;

        const SettingsRoute route = SettingsRoute::ForPage(
            descriptor.page, descriptor.focusId);
        if (!route.IsValid()) continue;
        const EntryIdentity identity{
            static_cast<int>(descriptor.page), {}, descriptor.focusId };
        if (!identities.insert(identity).second) continue;

        Entry entry;
        entry.result.kind = SettingsSearchEntryKind::StaticSetting;
        entry.result.route = route;
        entry.result.focusId = descriptor.focusId;
        entry.result.label = descriptor.label;
        entry.result.description = descriptor.description;
        entry.normalizedLabel = Normalize(descriptor.label);
        entry.normalizedDescription = Normalize(descriptor.description);
        for (const auto& keyword : descriptor.keywords)
            AppendNormalized(entry.normalizedKeywords, keyword);
        entry.ordinal = ordinal++;
        rebuilt.push_back(std::move(entry));
    }

    for (const auto& widget : input.widgets)
    {
        if (!widget.installed || !widget.visible ||
            widget.instanceId.empty() || widget.widgetName.empty())
            continue;
        for (const auto& field : widget.fields)
        {
            const std::string focusId = field.focusId.empty()
                ? field.key : field.focusId;
            if (!field.visible || focusId.empty() || field.label.empty())
                continue;

            const SettingsRoute route = SettingsRoute::ForWidget(
                widget.instanceId, focusId);
            if (!route.IsValid()) continue;
            const EntryIdentity identity{
                static_cast<int>(SettingsPage::WidgetSettings),
                widget.instanceId, focusId };
            if (!identities.insert(identity).second) continue;

            Entry entry;
            entry.result.kind = SettingsSearchEntryKind::WidgetSetting;
            entry.result.route = route;
            entry.result.focusId = focusId;
            entry.result.label = field.label;
            entry.result.description = field.description;
            entry.result.context = widget.widgetName;
            entry.normalizedLabel = Normalize(field.label);
            entry.normalizedDescription = Normalize(field.description);
            AppendNormalized(entry.normalizedContext, widget.widgetName);
            AppendNormalized(entry.normalizedContext, field.groupLabel);
            entry.ordinal = ordinal++;
            rebuilt.push_back(std::move(entry));
        }
    }

    languageTag_ = input.languageTag;
    entries_.swap(rebuilt);
}

std::vector<SettingsSearchResult> SettingsSearchIndex::Search(
    std::wstring_view query, std::size_t maximumResults) const
{
    const std::u32string normalizedQuery = Normalize(query);
    if (normalizedQuery.empty() || maximumResults == 0) return {};
    const std::vector<std::u32string> terms =
        SplitTerms(normalizedQuery);

    struct Match
    {
        const Entry* entry = nullptr;
        int score = 0;
    };
    std::vector<Match> matches;
    matches.reserve(entries_.size());

    for (const Entry& entry : entries_)
    {
        int score = 0;
        bool matched = true;
        for (const std::u32string& term : terms)
        {
            const int termScore = (std::min)({
                MatchField(entry.normalizedLabel, term, 0),
                MatchField(entry.normalizedKeywords, term, 10),
                MatchField(entry.normalizedDescription, term, 20),
                MatchField(entry.normalizedContext, term, 30),
            });
            if (termScore == (std::numeric_limits<int>::max)())
            {
                matched = false;
                break;
            }
            score += termScore;
        }
        if (!matched) continue;

        if (entry.normalizedLabel == normalizedQuery)
            score -= 100;
        else if (entry.normalizedLabel.starts_with(normalizedQuery))
            score -= 40;
        matches.push_back({ &entry, score });
    }

    std::stable_sort(matches.begin(), matches.end(),
        [](const Match& left, const Match& right) {
            if (left.score != right.score)
                return left.score < right.score;
            return left.entry->ordinal < right.entry->ordinal;
        });

    const std::size_t count = (std::min)(
        maximumResults, matches.size());
    std::vector<SettingsSearchResult> results;
    results.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        results.push_back(matches[index].entry->result);
    return results;
}

} // namespace snowdesktop
