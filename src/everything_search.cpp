#include "everything_search.h"

#define EVERYTHINGUSERAPI
#include <Everything.h>
#include <shlwapi.h>

#include <algorithm>

namespace
{
std::wstring FileNameFromPath(const std::wstring& path)
{
    if (path.empty()) return {};
    const wchar_t* name = PathFindFileNameW(path.c_str());
    return name ? std::wstring(name) : path;
}
}

EverythingSearchClient::~EverythingSearchClient()
{
    Everything_CleanUp();
}

std::vector<EverythingSearchResult> EverythingSearchClient::Search(
    const std::wstring& query, DWORD maxResults)
{
    std::vector<EverythingSearchResult> results;
    if (query.empty() || maxResults == 0)
        return results;

    Everything_Reset();
    Everything_SetSearchW(query.c_str());
    Everything_SetMatchPath(FALSE);
    Everything_SetMatchCase(FALSE);
    Everything_SetMatchWholeWord(FALSE);
    Everything_SetRegex(FALSE);
    Everything_SetOffset(0);
    Everything_SetMax(maxResults);
    Everything_SetSort(EVERYTHING_SORT_DATE_RECENTLY_CHANGED_DESCENDING);
    Everything_SetRequestFlags(EVERYTHING_REQUEST_FILE_NAME | EVERYTHING_REQUEST_FULL_PATH_AND_FILE_NAME);

    if (!Everything_QueryW(TRUE))
    {
        lastError_ = Everything_GetLastError();
        return results;
    }

    const DWORD count = std::min(Everything_GetNumResults(), maxResults);
    results.reserve(count);
    for (DWORD i = 0; i < count; ++i)
    {
        DWORD needed = Everything_GetResultFullPathNameW(i, nullptr, 0);
        std::wstring path(static_cast<size_t>(needed) + 1, L'\0');
        DWORD copied = Everything_GetResultFullPathNameW(i, path.data(), static_cast<DWORD>(path.size()));
        path.resize(static_cast<size_t>(copied));
        if (path.empty())
            continue;

        EverythingSearchResult result;
        const wchar_t* name = Everything_GetResultFileNameW(i);
        result.name = (name && *name) ? std::wstring(name) : FileNameFromPath(path);
        result.path = std::move(path);
        result.isDirectory = Everything_IsFolderResult(i) != FALSE;
        results.push_back(std::move(result));
    }

    lastError_ = ERROR_SUCCESS;
    return results;
}
