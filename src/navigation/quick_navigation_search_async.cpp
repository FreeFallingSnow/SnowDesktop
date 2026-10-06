#include "quick_navigation_search_async.h"

#include <mutex>
#include <thread>
#include <utility>

namespace snowdesktop
{
namespace
{
QuickNavigationEverythingSearchResponse
UnavailableSearch(
    const std::wstring&,
    DWORD)
{
    QuickNavigationEverythingSearchResponse response;
    response.error = ERROR_NOT_SUPPORTED;
    return response;
}
}

struct QuickNavigationEverythingSearchAsync::State
{
    explicit State(SearchFunction value)
        : search(std::move(value))
    {
    }

    std::mutex mutex;
    SearchFunction search;
    std::optional<QuickNavigationEverythingSearchRequest>
        pending;
    std::optional<QuickNavigationEverythingSearchResult>
        completed;
    HWND notifyWindow = nullptr;
    UINT notifyMessage = 0;
    bool accepting = true;
    bool workerRunning = false;
};

void QuickNavigationEverythingSearchAsync::RunWorker(
    const std::shared_ptr<State>& state)
{
    for (;;)
    {
        QuickNavigationEverythingSearchRequest request;
        QuickNavigationEverythingSearchAsync::SearchFunction
            search;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->accepting || !state->pending)
            {
                state->workerRunning = false;
                return;
            }
            request = std::move(*state->pending);
            state->pending.reset();
            search = state->search;
        }

        QuickNavigationEverythingSearchResponse response;
        try
        {
            response = search(
                request.query, request.maxResults);
        }
        catch (...)
        {
            response.error = ERROR_GEN_FAILURE;
        }

        HWND notifyWindow = nullptr;
        UINT notifyMessage = 0;
        WPARAM cookie = 0;
        {
            std::scoped_lock lock(state->mutex);
            if (!state->accepting)
            {
                state->workerRunning = false;
                return;
            }

            // A request typed while this query was blocked supersedes the
            // completed value before it can reach the UI.
            if (state->pending)
                continue;

            QuickNavigationEverythingSearchResult result;
            result.generation = request.generation;
            result.query = std::move(request.query);
            result.maxResults = request.maxResults;
            result.results = std::move(response.results);
            result.error = response.error;
            state->completed = std::move(result);
            notifyWindow = state->notifyWindow;
            notifyMessage = state->notifyMessage;
            cookie = reinterpret_cast<WPARAM>(state.get());
        }

        if (notifyWindow && notifyMessage)
            PostMessageW(
                notifyWindow,
                notifyMessage,
                cookie,
                0);
    }
}

QuickNavigationEverythingSearchAsync::
    QuickNavigationEverythingSearchAsync(
        SearchFunction search)
    : state_(std::make_shared<State>(
          search ? std::move(search)
                 : SearchFunction(&UnavailableSearch)))
{
}

QuickNavigationEverythingSearchAsync::
    ~QuickNavigationEverythingSearchAsync()
{
    Stop();
}

bool QuickNavigationEverythingSearchAsync::Submit(
    HWND notifyWindow,
    UINT notifyMessage,
    QuickNavigationEverythingSearchRequest request)
{
    const std::shared_ptr<State> state = state_;
    if (!state)
        return false;

    std::scoped_lock lock(state->mutex);
    if (!state->accepting)
        return false;

    state->notifyWindow = notifyWindow;
    state->notifyMessage = notifyMessage;
    state->pending = std::move(request);
    if (state->workerRunning)
        return true;

    state->workerRunning = true;
    try
    {
        std::thread(
            &QuickNavigationEverythingSearchAsync::RunWorker,
            state).detach();
    }
    catch (...)
    {
        state->workerRunning = false;
        state->pending.reset();
        return false;
    }
    return true;
}

std::optional<QuickNavigationEverythingSearchResult>
QuickNavigationEverythingSearchAsync::TakeCompleted()
{
    const std::shared_ptr<State> state = state_;
    if (!state)
        return std::nullopt;

    std::scoped_lock lock(state->mutex);
    std::optional<QuickNavigationEverythingSearchResult>
        result = std::move(state->completed);
    state->completed.reset();
    return result;
}

WPARAM QuickNavigationEverythingSearchAsync::
    MessageCookie() const noexcept
{
    return reinterpret_cast<WPARAM>(state_.get());
}

void QuickNavigationEverythingSearchAsync::Stop() noexcept
{
    const std::shared_ptr<State> state = state_;
    if (!state)
        return;

    std::scoped_lock lock(state->mutex);
    state->accepting = false;
    state->pending.reset();
    state->completed.reset();
    state->notifyWindow = nullptr;
    state->notifyMessage = 0;
}
}
