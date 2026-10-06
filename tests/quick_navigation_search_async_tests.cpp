#include "navigation/quick_navigation_search_async.h"

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace std::chrono_literals;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
}

void TestBlockedSearchDoesNotBlockSubmissionAndCoalesces()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool firstEntered = false;
    bool releaseFirst = false;
    bool providerTimedOut = false;
    std::vector<std::wstring> queries;

    snowdesktop::QuickNavigationEverythingSearchAsync search(
        [&](const std::wstring& query, DWORD) {
            {
                std::unique_lock lock(mutex);
                queries.push_back(query);
                if (query == L"first")
                {
                    firstEntered = true;
                    condition.notify_all();
                    providerTimedOut = !condition.wait_for(lock, 2s,
                        [&]() { return releaseFirst; });
                }
            }

            snowdesktop::QuickNavigationEverythingSearchResponse
                response;
            EverythingSearchResult result;
            result.name = query;
            result.path = L"C:\\" + query;
            response.results.push_back(std::move(result));
            return response;
        });

    Check(search.Submit(
              nullptr, 0,
              {1, L"first", 200}),
        "the first asynchronous search must be accepted");

    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(
                  lock, 2s,
                  [&]() { return firstEntered; }),
            "the background worker must start the first request");
    }

    Check(search.Submit(
              nullptr, 0,
              {2, L"second", 200}) &&
            search.Submit(
              nullptr, 0,
              {3, L"third", 200}),
        "requests typed during a blocked query must remain accepted");
    {
        std::scoped_lock lock(mutex);
        Check(!providerTimedOut,
            "all submissions return before the controlled provider is released; its deadline prevents a hung test");
        releaseFirst = true;
    }
    condition.notify_all();

    std::optional<
        snowdesktop::QuickNavigationEverythingSearchResult>
        completed;
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        completed = search.TakeCompleted();
        if (completed)
            break;
        std::this_thread::sleep_for(5ms);
    }

    Check(completed.has_value(),
        "the newest coalesced request must complete");
    if (completed)
    {
        Check(completed->generation == 3 &&
                completed->query == L"third" &&
                completed->results.size() == 1 &&
                completed->results[0].name == L"third",
            "only the newest request result may be published");
    }

    {
        std::scoped_lock lock(mutex);
        Check(queries.size() == 2 &&
                queries[0] == L"first" &&
                queries[1] == L"third",
            "an intermediate pending request must be coalesced away");
    }
}

void TestFailedSearchSettlesAndAllowsNextQuery()
{
    snowdesktop::QuickNavigationEverythingSearchAsync search(
        [](const std::wstring& query, DWORD) {
            snowdesktop::QuickNavigationEverythingSearchResponse response;
            response.error = query == L"unavailable" ? 2 : ERROR_SUCCESS;
            return response;
        });
    const auto waitForResult = [&]() {
        std::optional<snowdesktop::QuickNavigationEverythingSearchResult> result;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            result = search.TakeCompleted();
            if (result) break;
            std::this_thread::sleep_for(5ms);
        }
        return result;
    };
    Check(search.Submit(nullptr,0,{1,L"unavailable",200}),"unavailable search is accepted asynchronously");
    const auto failed = waitForResult();
    Check(failed && failed->generation == 1 && failed->error == 2 && failed->results.empty(),
        "an unavailable provider publishes a completed error rather than leaving search pending");
    Check(search.Submit(nullptr,0,{2,L"retry",200}),"a subsequent search is accepted after an error");
    const auto retry = waitForResult();
    Check(retry && retry->generation == 2 && retry->error == ERROR_SUCCESS,
        "a later successful query replaces the unavailable state");
}

void TestStopDoesNotJoinBlockedProvider()
{
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    bool providerReturned = false;
    bool providerTimedOut = false;

    snowdesktop::QuickNavigationEverythingSearchAsync search(
        [&](const std::wstring&, DWORD) {
            std::unique_lock lock(mutex);
            entered = true;
            condition.notify_all();
            providerTimedOut = !condition.wait_for(lock, 2s,
                [&]() { return released; });
            providerReturned = true;
            condition.notify_all();
            return snowdesktop::
                QuickNavigationEverythingSearchResponse{};
        });
    Check(search.Submit(
              nullptr, 0,
              {1, L"blocked", 200}),
        "the blocked shutdown request must be accepted");
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(
                  lock, 2s,
                  [&]() { return entered; }),
            "the shutdown test provider must start");
    }

    search.Stop();

    {
        std::scoped_lock lock(mutex);
        Check(!providerReturned && !providerTimedOut,
            "Stop returns while the provider is still blocked, before its release or deadline");
        released = true;
    }
    condition.notify_all();
    {
        std::unique_lock lock(mutex);
        Check(condition.wait_for(
                  lock, 2s,
                  [&]() { return providerReturned; }),
            "the test provider must be released before process exit");
    }
}
}

int main()
{
    TestBlockedSearchDoesNotBlockSubmissionAndCoalesces();
    TestFailedSearchSettlesAndAllowsNextQuery();
    TestStopDoesNotJoinBlockedProvider();
    if (failures == 0)
    {
        std::cout <<
            "quick navigation async search tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
