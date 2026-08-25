#include "quick_navigation_search_async.h"

#include "search_match.h"
#include "utils.h"

#include <algorithm>

namespace snowdesktop
{
namespace
{
std::wstring FileNameFromPath(
    const std::wstring& path)
{
    const size_t separator =
        path.find_last_of(L"\\/");
    return separator == std::wstring::npos
        ? path
        : path.substr(separator + 1);
}

QuickNavigationEverythingSearchResponse
RunEverythingSearch(
    const std::wstring& query,
    DWORD maxResults)
{
    QuickNavigationEverythingSearchResponse response;
    EverythingSearchClient search;
    response.results = search.Search(query, maxResults);
    response.error = search.LastError();

    const std::wstring normalizedQuery =
        ToUpperInvariant(query);
    std::stable_sort(
        response.results.begin(),
        response.results.end(),
        [&](const EverythingSearchResult& left,
            const EverythingSearchResult& right) {
            const std::wstring leftName = left.name.empty()
                ? FileNameFromPath(left.path)
                : left.name;
            const std::wstring rightName = right.name.empty()
                ? FileNameFromPath(right.path)
                : right.name;
            const int leftRank =
                NameSearchMatchRank(
                    leftName, normalizedQuery);
            const int rightRank =
                NameSearchMatchRank(
                    rightName, normalizedQuery);
            if (leftRank != rightRank)
                return leftRank < rightRank;

            const std::wstring leftNameKey =
                ToUpperInvariant(leftName);
            const std::wstring rightNameKey =
                ToUpperInvariant(rightName);
            if (leftNameKey != rightNameKey)
                return leftNameKey < rightNameKey;

            const int timeComparison = CompareFileTime(
                &left.dateModified,
                &right.dateModified);
            if (timeComparison != 0)
                return timeComparison > 0;

            return ToUpperInvariant(left.path) <
                ToUpperInvariant(right.path);
        });
    return response;
}
}

QuickNavigationEverythingSearchAsync::
    QuickNavigationEverythingSearchAsync()
    : QuickNavigationEverythingSearchAsync(
          &RunEverythingSearch)
{
}
}
