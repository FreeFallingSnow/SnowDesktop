#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::localization
{
namespace detail
{
struct LanguageTagParts
{
    std::string normalized;
    std::string language;
    std::string script;
    std::string region;
};

inline LanguageTagParts ParseLanguageTag(std::string_view value)
{
    LanguageTagParts result;
    result.normalized.assign(value.begin(), value.end());
    std::ranges::transform(result.normalized, result.normalized.begin(),
        [](unsigned char character) {
            return character == '_' ? '-' :
                static_cast<char>(std::tolower(character));
        });

    std::size_t begin = 0;
    int part = 0;
    while (begin < result.normalized.size())
    {
        const std::size_t end = result.normalized.find('-', begin);
        const std::string token = result.normalized.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (part == 0)
            result.language = token;
        else if (token.size() == 4 && result.script.empty())
            result.script = token;
        else if ((token.size() == 2 || token.size() == 3) &&
            result.region.empty())
            result.region = token;
        ++part;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

enum class ChineseVariant
{
    Unknown,
    Simplified,
    Traditional,
};

inline ChineseVariant GetChineseVariant(const LanguageTagParts& tag)
{
    if (tag.language != "zh") return ChineseVariant::Unknown;
    if (tag.script == "hant" || tag.region == "hk" ||
        tag.region == "mo" || tag.region == "tw")
        return ChineseVariant::Traditional;
    if (tag.script == "hans" || tag.region == "cn" ||
        tag.region == "my" || tag.region == "sg")
        return ChineseVariant::Simplified;
    return ChineseVariant::Unknown;
}

inline bool IsLatinAmericanSpanishRegion(const std::string& region)
{
    return region == "419" || region == "ar" || region == "bo" ||
        region == "cl" || region == "co" || region == "cr" ||
        region == "cu" || region == "do" || region == "ec" ||
        region == "gt" || region == "hn" || region == "mx" ||
        region == "ni" || region == "pa" || region == "pe" ||
        region == "pr" || region == "py" || region == "sv" ||
        region == "uy" || region == "ve";
}

inline int MatchScore(const LanguageTagParts& requested,
    const LanguageTagParts& candidate)
{
    if (requested.language.empty() ||
        requested.language != candidate.language)
        return -1;

    int score = 100;
    if (requested.script == candidate.script &&
        !requested.script.empty())
        score += 100;
    if (requested.region == candidate.region &&
        !requested.region.empty())
        score += 200;

    if (requested.language == "zh")
    {
        const ChineseVariant requestedVariant =
            GetChineseVariant(requested);
        const ChineseVariant candidateVariant =
            GetChineseVariant(candidate);
        if (requestedVariant != ChineseVariant::Unknown &&
            candidateVariant != ChineseVariant::Unknown)
        {
            if (requestedVariant != candidateVariant)
                return -1;
            score += 500;
        }
    }
    else if (requested.language == "es")
    {
        const bool requestedLatinAmerican =
            IsLatinAmericanSpanishRegion(requested.region);
        const bool candidateLatinAmerican =
            IsLatinAmericanSpanishRegion(candidate.region);
        if (requestedLatinAmerican == candidateLatinAmerican &&
            !requested.region.empty() && !candidate.region.empty())
            score += 75;
    }
    return score;
}
}

inline std::string ResolveBestLanguage(
    const std::vector<std::string>& availableLanguages,
    std::string_view requestedLanguage)
{
    const detail::LanguageTagParts requested =
        detail::ParseLanguageTag(requestedLanguage);
    std::string best;
    std::string bestNormalized;
    int bestScore = -1;

    for (const std::string& candidateCode : availableLanguages)
    {
        const detail::LanguageTagParts candidate =
            detail::ParseLanguageTag(candidateCode);
        if (requested.normalized == candidate.normalized)
            return candidateCode;

        const int score = detail::MatchScore(requested, candidate);
        if (score < 0) continue;
        if (score > bestScore ||
            (score == bestScore &&
                (best.empty() || candidate.normalized < bestNormalized)))
        {
            best = candidateCode;
            bestNormalized = candidate.normalized;
            bestScore = score;
        }
    }
    return best;
}
}
