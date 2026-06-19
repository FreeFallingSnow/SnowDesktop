#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct HttpRequestOptions
{
    std::wstring widgetId;
    std::wstring url;
    std::wstring method = L"GET";
    std::wstring headers;
    std::string body;
    int timeoutMs = 10000;
    int cacheSeconds = 0;
    std::vector<std::string> allowedDomains;
};

struct HttpResponse
{
    int id = 0;
    std::wstring widgetId;
    int status = 0;
    std::string body;
    std::string error;
    bool fromCache = false;
};

class AsyncHttpService
{
public:
    AsyncHttpService() = default;
    ~AsyncHttpService();

    int Submit(HttpRequestOptions options);
    bool Cancel(const std::wstring& widgetId, int requestId);
    std::vector<HttpResponse> Drain();
    void CancelWidget(const std::wstring& widgetId);
    void Stop();

private:
    struct RequestState
    {
        std::wstring widgetId;
        std::jthread worker;
    };
    struct CacheEntry
    {
        HttpResponse response;
        std::chrono::steady_clock::time_point expires;
    };

    static HttpResponse Execute(int id, const HttpRequestOptions& options, std::stop_token token);
    static bool IsDomainAllowed(const std::wstring& url, const std::vector<std::string>& domains);
    void Complete(HttpResponse response);

    std::atomic<int> nextId_{ 1 };
    std::mutex mutex_;
    std::unordered_map<int, std::unique_ptr<RequestState>> requests_;
    std::deque<HttpResponse> completed_;
    std::unordered_map<std::wstring, CacheEntry> cache_;
};
