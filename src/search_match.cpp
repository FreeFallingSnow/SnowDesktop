#include "search_match.h"

#include "pinyin_full_table.h"
#include "utils.h"

#include <shlwapi.h>

#include <string_view>

namespace
{
bool IsAsciiLetterOrDigit(wchar_t ch)
{
    return (ch >= L'A' && ch <= L'Z') ||
        (ch >= L'a' && ch <= L'z') ||
        (ch >= L'0' && ch <= L'9');
}

char ToAsciiSearchChar(wchar_t ch)
{
    if (ch >= L'a' && ch <= L'z')
        return static_cast<char>(ch - L'a' + L'A');
    return static_cast<char>(ch);
}

bool IsLatinUmlaut(wchar_t ch)
{
    switch (ch)
    {
    case L'\u00FC': // u with diaeresis
    case L'\u01D6': // u with diaeresis and macron
    case L'\u01D8': // u with diaeresis and acute
    case L'\u01DA': // u with diaeresis and caron
    case L'\u01DC': // u with diaeresis and grave
    case L'\u00DC':
    case L'\u01D5':
    case L'\u01D7':
    case L'\u01D9':
    case L'\u01DB':
        return true;
    default:
        return false;
    }
}

std::string BuildNamePinyinFullKey(const std::wstring& name)
{
    std::string result;
    result.reserve(name.size() * 4);
    for (wchar_t ch : name)
    {
        std::string_view syllable = PinyinFullSyllableForCodepoint(ch);
        if (!syllable.empty())
            result.append(syllable);
        else if (IsAsciiLetterOrDigit(ch))
            result.push_back(ToAsciiSearchChar(ch));
        else if (IsLatinUmlaut(ch))
            result.push_back('V');
    }
    return result;
}

std::string BuildNamePinyinInitialKey(const std::wstring& name)
{
    std::string result;
    result.reserve(name.size());
    for (wchar_t ch : name)
    {
        std::string_view syllable = PinyinFullSyllableForCodepoint(ch);
        if (!syllable.empty())
            result.push_back(syllable.front());
        else if (IsAsciiLetterOrDigit(ch))
            result.push_back(ToAsciiSearchChar(ch));
        else if (IsLatinUmlaut(ch))
            result.push_back('V');
    }
    return result;
}

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}
}

int NameSearchMatchRank(const std::wstring& name, const std::wstring& query)
{
    if (query.empty())
        return 0;

    const std::wstring normalizedQuery = ToUpperInvariant(query);
    const std::wstring normalizedName = ToUpperInvariant(name);
    if (normalizedName == normalizedQuery)
        return 0;

    const wchar_t* ext = PathFindExtensionW(name.c_str());
    if (ext && *ext && ext > name.c_str())
    {
        std::wstring stem(name.c_str(), static_cast<size_t>(ext - name.c_str()));
        if (ToUpperInvariant(stem) == normalizedQuery)
            return 0;
    }

    const std::string pinyinQuery = BuildNamePinyinFullKey(query);
    std::string fullPinyin;
    std::string initials;
    if (!pinyinQuery.empty())
    {
        fullPinyin = BuildNamePinyinFullKey(name);
        if (fullPinyin == pinyinQuery)
            return 1;

        initials = BuildNamePinyinInitialKey(name);
        if (initials == pinyinQuery)
            return 2;

        if (StartsWith(fullPinyin, pinyinQuery))
            return 3;

        if (StartsWith(initials, pinyinQuery))
            return 4;
    }

    if (normalizedName.rfind(normalizedQuery, 0) == 0)
        return 5;

    if (!pinyinQuery.empty())
    {
        if (fullPinyin.find(pinyinQuery) != std::string::npos)
            return 6;

        if (initials.find(pinyinQuery) != std::string::npos)
            return 7;
    }

    return normalizedName.find(normalizedQuery) != std::wstring::npos
        ? 8
        : kNameSearchNoMatchRank;
}

bool NameMatchesQuery(const std::wstring& name, const std::wstring& query)
{
    return NameSearchMatchRank(name, query) < kNameSearchNoMatchRank;
}
