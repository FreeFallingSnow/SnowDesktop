#include "search_match.h"

#include "name_pinyin.h"
#include "utils.h"

#include <shlwapi.h>

#include <string_view>

namespace
{
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

    const std::string pinyinQuery =
        BuildNamePinyinFullKey(query);
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
