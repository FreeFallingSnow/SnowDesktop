#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct EverythingSearchResult
{
    std::wstring name;
    std::wstring path;
    bool isDirectory = false;
};

class EverythingSearchClient
{
public:
    EverythingSearchClient() = default;
    ~EverythingSearchClient();

    EverythingSearchClient(const EverythingSearchClient&) = delete;
    EverythingSearchClient& operator=(const EverythingSearchClient&) = delete;

    std::vector<EverythingSearchResult> Search(const std::wstring& query, DWORD maxResults);
    DWORD LastError() const { return lastError_; }

private:
    DWORD lastError_ = ERROR_SUCCESS;
};
