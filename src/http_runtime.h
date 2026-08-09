#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace snowdesktop::http_security
{
bool IsAllowedRemoteIpLiteral(std::wstring_view address);
bool IsAllowedHttpsUrlForDomains(const std::wstring& url,
    const std::vector<std::string>& domains);
bool IsResolutionPinningSupportedVersion(int majorVersion, int buildNumber);
}

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
    // 非空时响应体流式写入该文件（替代内存缓冲），用于大文件下载。
    std::wstring bodyFilePath;
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
    void Complete(HttpResponse response);

    std::atomic<int> nextId_{ 1 };
    std::mutex mutex_;
    std::unordered_map<int, std::unique_ptr<RequestState>> requests_;
    std::deque<HttpResponse> completed_;
    std::unordered_map<std::wstring, CacheEntry> cache_;
};
