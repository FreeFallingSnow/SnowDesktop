#pragma once

#include "everything_search.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop
{
struct QuickNavigationEverythingSearchRequest
{
    std::uint64_t generation = 0;
    std::wstring query;
    DWORD maxResults = 0;
};

struct QuickNavigationEverythingSearchResponse
{
    std::vector<EverythingSearchResult> results;
    DWORD error = ERROR_SUCCESS;
};

struct QuickNavigationEverythingSearchResult
{
    std::uint64_t generation = 0;
    std::wstring query;
    DWORD maxResults = 0;
    std::vector<EverythingSearchResult> results;
    DWORD error = ERROR_SUCCESS;
};

/**
 * Runs the Everything SDK outside the UI thread.
 *
 * The SDK's synchronous query can wait forever for its IPC peer. This class
 * deliberately owns detached workers through shared state, so application
 * shutdown and later search input never join an unbounded IPC call. While a
 * worker is busy, pending submissions are coalesced to the newest request.
 */
class QuickNavigationEverythingSearchAsync
{
public:
    using SearchFunction = std::function<
        QuickNavigationEverythingSearchResponse(
            const std::wstring&, DWORD)>;

    QuickNavigationEverythingSearchAsync();
    explicit QuickNavigationEverythingSearchAsync(
        SearchFunction search);
    ~QuickNavigationEverythingSearchAsync();

    QuickNavigationEverythingSearchAsync(
        const QuickNavigationEverythingSearchAsync&) = delete;
    QuickNavigationEverythingSearchAsync& operator=(
        const QuickNavigationEverythingSearchAsync&) = delete;

    bool Submit(
        HWND notifyWindow,
        UINT notifyMessage,
        QuickNavigationEverythingSearchRequest request);
    std::optional<QuickNavigationEverythingSearchResult>
        TakeCompleted();
    WPARAM MessageCookie() const noexcept;
    void Stop() noexcept;

private:
    struct State;
    static void RunWorker(
        const std::shared_ptr<State>& state);
    std::shared_ptr<State> state_;
};
}
